import { randomBytes, timingSafeEqual } from "node:crypto";
import { closeSync, constants, fstatSync, lstatSync, openSync, readFileSync, realpathSync,
    renameSync, rmdirSync, unlinkSync, writeFileSync } from "node:fs";
import { createServer } from "node:net";
import type { Socket } from "node:net";
import { basename, isAbsolute, join, resolve } from "node:path";
import type { ExtensionAPI, ExtensionContext } from "@oh-my-pi/pi-coding-agent";

type ObjectValue = Record<string, unknown>;
export type WakeState = {
    providerSessionId: string;
    transitionNonce: string;
    unavailable?: string;
    deferred?: string;
};
type Registration = { registration_id: string; token: string; instance_id: string };
export type WakeBridge = { close(): void };

const record = (value: unknown): value is ObjectValue => typeof value === "object" && value !== null && !Array.isArray(value);
const text = (value: unknown, bound: number): value is string => typeof value === "string" && value.length > 0 &&
    Buffer.byteLength(value) <= bound && !value.includes("\0") && !/[\ud800-\udfff]/u.test(value);
const nonce = (value: unknown): value is string => typeof value === "string" && /^[0-9a-f]{32}$/u.test(value);
const keys = (value: ObjectValue, expected: string[]) => Object.keys(value).length === expected.length &&
    expected.every(key => Object.hasOwn(value, key));
const unavailable = (reason: string) => ({ disposition: "unavailable", reason });

export function nativeWakeState(ctx: ExtensionContext, owner: { sessionId: string; nonce: string;
    lost: boolean; turnId: string; interrupted: boolean; attention: boolean }): WakeState {
    const result: WakeState = { providerSessionId: owner.sessionId, transitionNonce: owner.nonce };
    if (owner.lost || !ctx.hasUI || ctx.sessionManager.getSessionId() !== owner.sessionId)
        return { ...result, unavailable: "native_wake_unavailable" };
    if (owner.attention) return { ...result, deferred: "native_attention_pending" };
    if (owner.interrupted) return { ...result, deferred: "native_interrupted" };
    if (owner.turnId || !ctx.isIdle()) return { ...result, deferred: "native_busy" };
    const modes = ctx.sessionModes?.getState();
    if (!modes || !["inactive", "active", "paused"].includes(modes.plan?.status) ||
        !["inactive", "active", "paused"].includes(modes.goal?.status) ||
        !["inactive", "active", "paused"].includes(modes.loop?.status))
        return { ...result, unavailable: "native_mode_query_unavailable" };
    if (modes.plan.status !== "inactive" || modes.goal.status === "paused" || modes.loop.status === "paused")
        return { ...result, deferred: "native_paused" };
    return result;
}

/** Native submission policy is synchronous: no await can transfer a checked
 * activation to a replacement before calling the native session-bound API. */
export function createWakeReceiver(pi: Pick<ExtensionAPI, "sendMessage">, registration: Registration,
    state: () => WakeState | undefined): (value: unknown) => ObjectValue {
    const receipts = new Map<string, ObjectValue>();
    let receiptActivation = "";
    return (value: unknown): ObjectValue => {
        if (!record(value) || (value.operation !== "inspect" && value.operation !== "notify"))
            return unavailable("invalid_native_request");
        const inspect = value.operation === "inspect";
        const expected = ["operation", "registration_id", "instance_id", "token", "provider_session_id", "transition_nonce",
            ...(inspect ? [] : ["notification_id", "instruction"])];
        const refuse = (reason: string) => inspect ? { available: false, reason } : unavailable(reason);
        if (!keys(value, expected) || value.registration_id !== registration.registration_id ||
            value.instance_id !== registration.instance_id || typeof value.token !== "string" ||
            !/^[0-9a-f]{64}$/u.test(value.token) ||
            !timingSafeEqual(Buffer.from(value.token), Buffer.from(registration.token)) ||
            !text(value.provider_session_id, 256) || !nonce(value.transition_nonce) ||
            (!inspect && (!text(value.notification_id, 256) || !text(value.instruction, 4096))))
            return refuse("invalid_native_request");
        let current: WakeState | undefined;
        try { current = state(); } catch { return refuse("native_state_unavailable"); }
        if (!current || current.providerSessionId !== value.provider_session_id || current.transitionNonce !== value.transition_nonce)
            return refuse("unbound_session");
        if (receiptActivation !== current.transitionNonce) {
            receipts.clear();
            receiptActivation = current.transitionNonce;
        }
        const id = inspect ? "" : value.notification_id as string;
        if (!inspect && receipts.has(id)) return receipts.get(id)!;
        if (current.unavailable || typeof pi.sendMessage !== "function")
            return refuse(current.unavailable ?? "native_wake_unsupported");
        if (current.deferred)
            return inspect ? { available: false, reason: current.deferred } : { disposition: "deferred", reason: current.deferred };
        if (receipts.size >= 256) return refuse("native_notification_limit");
        if (inspect) return { available: true };
        // The public API is void. Mark dispatch before calling it, so retries
        // cannot repeat a possibly admitted notification even after a throw.
        const outcome = { disposition: "uncertain", reason: "native_admission_unacknowledged" };
        receipts.set(id, outcome);
        try {
            pi.sendMessage({ customType: "logonomic-mailbox", content: value.instruction as string,
                display: false, attribution: "agent" }, { deliverAs: "aside" });
        } catch { /* An exception after entry cannot prove that native admission did not occur. */ }
        return outcome;
    };
}

function privateObject(path: string, directory: boolean): boolean {
    const status = lstatSync(path);
    return !status.isSymbolicLink() && (directory ? status.isDirectory() : status.isFile()) &&
        (process.platform === "win32" || (status.uid === process.geteuid?.() && (status.mode & 0o077) === 0));
}

function readPrivateJson(path: string): unknown {
    if (!privateObject(path, false)) throw new Error("invalid_private_registration");
    const descriptor = openSync(path, constants.O_RDONLY | (constants.O_NOFOLLOW ?? 0));
    try {
        const status = fstatSync(descriptor);
        if (!status.isFile() || status.size > 2048 || status.size === 0) throw new Error("invalid_private_registration");
        return JSON.parse(readFileSync(descriptor, "utf8"));
    } finally { closeSync(descriptor); }
}

/** Package-private listener; host authentication remains exclusively in the
 * SDK helper path. Credentials never enter host observations or model input. */
export async function startWakeBridge(pi: Pick<ExtensionAPI, "sendMessage">,
    state: () => WakeState | undefined, directory = process.env.VNM_OMP_WAKE_REGISTRATION): Promise<WakeBridge | undefined> {
    if (!directory || !isAbsolute(directory) || typeof pi.sendMessage !== "function") return undefined;
    const sockets = new Set<Socket>();
    let server: ReturnType<typeof createServer> | undefined;
    let instance = "";
    let closed = false;
    let claimed = false;
    const close = (): void => {
        if (closed) return;
        closed = true;
        try { server?.close(); } catch { /* Listener setup never completed. */ }
        for (const socket of sockets) socket.destroy();
        if (!claimed) return;
        // These exact files belong to this exclusive launch registration.
        for (const name of ["endpoint.json", `endpoint-${instance}.tmp`, "instance.json", "registration.json"]) {
            try { unlinkSync(join(directory, name)); } catch { /* Already retired. */ }
        }
        try { rmdirSync(directory); } catch { /* Do not recursively remove foreign files. */ }
    };
    try {
        if (realpathSync(directory) !== resolve(directory) || !privateObject(directory, true)) return undefined;
        const id = basename(directory).match(/^wake-([0-9a-f]{32})$/u)?.[1];
        const secret = readPrivateJson(join(directory, "registration.json"));
        if (!id || !record(secret) || !keys(secret, ["version", "registration_id", "token"]) || secret.version !== 1 ||
            secret.registration_id !== id || typeof secret.token !== "string" || !/^[0-9a-f]{64}$/u.test(secret.token))
            return undefined;
        instance = randomBytes(16).toString("hex");
        // An inherited child process or a second CLI cannot claim this launch.
        writeFileSync(join(directory, "instance.json"), JSON.stringify({ instance_id: instance }), { flag: "wx", mode: 0o600 });
        claimed = true;
        const receive = createWakeReceiver(pi, { registration_id: id, token: secret.token, instance_id: instance }, state);
        server = createServer(socket => {
            if (closed || sockets.size >= 4 || socket.remoteAddress !== "127.0.0.1") { socket.destroy(); return; }
            sockets.add(socket);
            socket.unref();
            socket.setTimeout(1000, () => socket.destroy());
            socket.on("error", () => {});
            socket.on("close", () => sockets.delete(socket));
            let bytes = Buffer.alloc(0);
            let complete = false;
            socket.on("data", chunk => {
                if (complete) { socket.destroy(); return; }
                bytes = Buffer.concat([bytes, chunk]);
                if (bytes.length > 32768) { socket.destroy(); return; }
                const end = bytes.indexOf(0x0a);
                if (end < 0) return;
                complete = true;
                if (end !== bytes.length - 1) { socket.destroy(); return; }
                try {
                    const data = new TextDecoder("utf-8", { fatal: true }).decode(bytes.subarray(0, end));
                    socket.end(`${JSON.stringify(receive(JSON.parse(data)))}\n`);
                } catch { socket.destroy(); }
            });
        });
        server.maxConnections = 4;
        server.unref();
        const listening = await new Promise<boolean>(resolveListening => {
            const timer = setTimeout(() => resolveListening(false), 500);
            timer.unref();
            server!.once("error", () => { clearTimeout(timer); resolveListening(false); });
            server!.listen({ host: "127.0.0.1", port: 0, exclusive: true }, () => {
                clearTimeout(timer); resolveListening(true);
            });
        });
        const address = server.address();
        if (!listening || !address || typeof address === "string") { close(); return undefined; }
        server.on("error", close);
        const endpoint = { version: 1, registration_id: id, instance_id: instance, port: address.port };
        const temporary = join(directory, `endpoint-${instance}.tmp`);
        writeFileSync(temporary, JSON.stringify(endpoint), { flag: "wx", mode: 0o600 });
        renameSync(temporary, join(directory, "endpoint.json"));
        return { close };
    } catch { close(); return undefined; }
}
