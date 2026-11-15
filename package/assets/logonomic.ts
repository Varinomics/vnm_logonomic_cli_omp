import { spawn } from "node:child_process";
import { randomBytes } from "node:crypto";
import { isAbsolute, join } from "node:path";
import type { ExtensionAPI, ExtensionContext } from "@oh-my-pi/pi-coding-agent";

type Facts = Record<string, string | boolean>;
type Request = { operation: "event" | "context"; name: string; body: Facts };
type Reply = { committed: boolean; accepted: boolean; instruction?: string };
type Transport = (request: Request, signal: AbortSignal) => Promise<Reply | undefined>;
type Activation = {
    sessionId: string;
    nonce: string;
    turnId: string;
    sequence: number;
    lost: boolean;
    lossReported: boolean;
    pendingContext: string;
};

const freshId = () => randomBytes(16).toString("hex");
const identity = (value: unknown, limit = 256): value is string =>
    typeof value === "string" && value.length > 0 && Buffer.byteLength(value) <= limit && !/[\x00-\x1f\x7f]/u.test(value);

/** One direct child process; SDK Helper_client owns and reaps the vnmctl child. */
async function invokeHelper(request: Request, signal: AbortSignal): Promise<Reply | undefined> {
    const directory = process.env.LOGONOMIC_PROVIDER_HELPERS_DIR;
    if (!directory || !isAbsolute(directory) || signal.aborted) return undefined;
    return await new Promise(resolve => {
        const child = spawn(join(directory, process.platform === "win32" ? "omp_helper.exe" : "omp_helper"), [], {
            stdio: ["pipe", "pipe", "ignore"], windowsHide: true,
        });
        const chunks: Buffer[] = [];
        let size = 0;
        let cancelled = false;
        let completed = false;
        // Closing this pipe asks the helper to cancel and reap its wrapper.
        // Killing only the helper would strand the authenticated wrapper.
        const cancel = () => { cancelled = true; child.stdin.end(); };
        const timer = setTimeout(cancel, 1000);
        signal.addEventListener("abort", cancel, { once: true });
        child.stdin.on("error", () => {});
        child.stdout.on("data", (chunk: Buffer) => {
            size += chunk.length;
            if (size > 40 * 1024) cancel();
            else chunks.push(chunk);
        });
        const finish = (code: number | null) => {
            if (completed) return;
            completed = true;
            clearTimeout(timer);
            signal.removeEventListener("abort", cancel);
            if (cancelled || code !== 0) { resolve(undefined); return; }
            try {
                const reply: unknown = JSON.parse(Buffer.concat(chunks).toString("utf8"));
                if (typeof reply !== "object" || reply === null ||
                    typeof (reply as Reply).accepted !== "boolean" || typeof (reply as Reply).committed !== "boolean" ||
                    ((reply as Reply).instruction !== undefined && typeof (reply as Reply).instruction !== "string")) {
                    resolve(undefined);
                } else resolve(reply as Reply);
            } catch { resolve(undefined); }
        };
        child.on("error", () => finish(null));
        child.on("close", finish);
        child.stdin.write(`${JSON.stringify(request)}\n`);
    });
}

/** Public-event adapter, exported for contract fixtures without an installed CLI. */
export function installExtension(pi: ExtensionAPI, transport: Transport = invokeHelper): void {
    let activation: Activation | undefined;
    let pending = Promise.resolve();
    let queued = 0;
    let shuttingDown = false;
    let running: AbortController | undefined;
    const children = new Map<string, { owner: Activation; sessionFile: unknown }>();
    const attention = new Map<string, Activation>();

    const isRoot = (ctx: ExtensionContext) => ctx.agent?.kind === "main";
    const current = (ctx: ExtensionContext) => isRoot(ctx) &&
        activation?.sessionId === ctx.sessionManager.getSessionId() ? activation : undefined;
    const facts = (owner: Activation, extra: Facts = {}): Facts => ({
        session_id: owner.sessionId, activation_nonce: owner.nonce,
        event_id: freshId(), sequence: String(++owner.sequence),
        ...(owner.turnId ? { turn_id: owner.turnId } : {}), ...extra,
    });

    const send = (owner: Activation, name: string, extra: Facts = {}): Promise<void> => {
        const body = facts(owner, extra); // Capture before any asynchronous work.
        if (++queued > 64) owner.lost = true;
        const work = pending.then(async () => {
            --queued;
            if (owner !== activation || owner.lossReported || (shuttingDown && name !== "session_shutdown")) return;
            const controller = new AbortController();
            running = controller;
            try {
                const reply = await transport({ operation: "event",
                    name: owner.lost ? "logonomic_observation_lost" : name, body }, controller.signal);
                if (!reply?.committed || !reply.accepted) owner.lost = true;
                else if (owner.lost) owner.lossReported = true;
            } catch { owner.lost = true; }
            finally { if (running === controller) running = undefined; }
        });
        pending = work.catch(() => {});
        return pending;
    };

    const activate = async (name: string, source: string, ctx: ExtensionContext): Promise<void> => {
        if (!isRoot(ctx) || shuttingDown) return;
        const sessionId = ctx.sessionManager.getSessionId();
        if (!identity(sessionId)) return;
        const previous = activation?.nonce;
        const owner: Activation = { sessionId, nonce: freshId(), turnId: "", sequence: 0,
            lost: false, lossReported: false, pendingContext: "" };
        activation = owner;
        children.clear();
        attention.clear();
        const sessionFile = ctx.sessionManager.getSessionFile();
        await send(owner, name, { source, ...(previous ? { previous_nonce: previous } : {}),
            ...(identity(sessionFile, 8192) && isAbsolute(sessionFile) ? { session_file: sessionFile } : {}) });
    };

    pi.on("session_start", (_event, ctx) => activate("session_start", "load", ctx));
    pi.on("session_switch", (event, ctx) => activate("session_switch", event.reason, ctx));
    pi.on("session_clear", (_event, ctx) => activate("session_clear", "clear", ctx));
    pi.on("session_branch", (event, ctx) => activate("session_branch", event.reason, ctx));
    pi.on("session_tree", (_event, ctx) => activate("session_tree", "tree", ctx));

    pi.on("before_agent_start", async (event, ctx) => {
        const owner = current(ctx);
        if (!owner || owner.lost || event.reason === "revive") return;
        await pending;
        if (owner !== activation || owner.lost || shuttingDown) return;
        if (!owner.pendingContext) {
            const controller = new AbortController();
            running = controller;
            try {
                const reply = await transport({ operation: "context", name: "before_agent_start",
                    body: { session_id: owner.sessionId, activation_nonce: owner.nonce } }, controller.signal);
                if (owner === activation && reply?.committed && reply.accepted && reply.instruction) {
                    owner.pendingContext = reply.instruction;
                }
            } catch { /* A context refusal never vetoes the native prompt. */ }
            finally { if (running === controller) running = undefined; }
        }
        if (owner === activation && !shuttingDown && owner.pendingContext) {
            // Repeated native preparation attempts retain accepted context
            // until agent_start proves the prepared prompt entered the loop.
            return { message: { customType: "logonomic", content: owner.pendingContext, display: false } };
        }
    });

    pi.on("agent_start", async (_event, ctx) => {
        const owner = current(ctx);
        if (!owner) return;
        owner.pendingContext = "";
        // Automatic continuations remain one busy root turn.
        if (!owner.turnId) owner.turnId = freshId();
        await send(owner, "agent_start");
    });
    pi.on("agent_end", async (event, ctx) => {
        const owner = current(ctx);
        if (!owner?.turnId) return;
        if (!Object.hasOwn(event, "willContinue")) {
            // The verified native emitter includes this field even when its
            // value is undefined. Missing semantic evidence is not success.
            owner.lost = true;
            await send(owner, "logonomic_observation_lost");
            return;
        }
        if (!ctx.isIdle() || event.willContinue === true ||
            (event.willContinue !== undefined && event.willContinue !== false)) return;
        // The native contract treats undefined/false willContinue as terminal.
        // session_stop is a veto gate and is deliberately never observed here.
        const last = [...event.messages].reverse().find(message => message.role === "assistant");
        const reason = last && "stopReason" in last && typeof last.stopReason === "string" ? last.stopReason : "";
        await send(owner, "agent_end", { terminal: true, stop_reason: reason });
        owner.turnId = "";
    });

    const requestAttention = async (name: string, id: string, ctx: ExtensionContext) => {
        const owner = current(ctx);
        if (!owner || !identity(id)) return;
        attention.set(id, owner);
        await send(owner, name, { request_id: id });
    };
    const resolveAttention = async (name: string, id: string, ctx: ExtensionContext) => {
        const owner = attention.get(id);
        attention.delete(id);
        if (owner && owner === current(ctx)) await send(owner, name, { request_id: id });
    };
    pi.on("tool_approval_requested", (event, ctx) => {
        if (event.sessionId === ctx.sessionManager.getSessionId())
            return requestAttention("tool_approval_requested", `permission:${event.toolCallId}`, ctx);
    });
    pi.on("tool_approval_resolved", (event, ctx) => {
        if (event.sessionId === ctx.sessionManager.getSessionId())
            return resolveAttention("tool_approval_resolved", `permission:${event.toolCallId}`, ctx);
    });
    pi.on("plan_review_requested", (event, ctx) => {
        if (event.sessionId === ctx.sessionManager.getSessionId())
            return requestAttention("plan_review_requested", `plan:${event.reviewId}`, ctx);
    });
    pi.on("plan_review_resolved", (event, ctx) => {
        if (event.sessionId === ctx.sessionManager.getSessionId())
            return resolveAttention("plan_review_resolved", `plan:${event.reviewId}`, ctx);
    });
    pi.on("tool_execution_start", (event, ctx) => {
        if (event.toolName === "ask" && ctx.hasUI)
            return requestAttention("ask_execution_start", `ask:${event.toolCallId}`, ctx);
    });
    pi.on("tool_execution_end", (event, ctx) => {
        if (event.toolName === "ask") return resolveAttention("ask_execution_end", `ask:${event.toolCallId}`, ctx);
    });

    // Public session EventBus reports immediate task/eval children. It does
    // not expose the private whole-tree bus; /tan and nested coverage are not
    // inferred from a parent tool completing or an assistant mentioning them.
    const unsubscribe = pi.events.on("task:subagent:lifecycle", (value: unknown) => {
        if (typeof value !== "object" || value === null) return;
        const event = value as { id?: unknown; agent?: unknown; status?: unknown; sessionFile?: unknown };
        if (!identity(event.id) || typeof event.status !== "string") return;
        if (event.status === "started") {
            if (!activation || shuttingDown) return;
            children.set(event.id, { owner: activation, sessionFile: event.sessionFile });
        }
        const child = children.get(event.id);
        if (!child || child.sessionFile !== event.sessionFile ||
            !["started", "completed", "failed", "aborted"].includes(event.status)) return;
        if (event.status !== "started") children.delete(event.id);
        void send(child.owner, "task:subagent:lifecycle", { agent_id: event.id, status: event.status,
            ...(identity(event.agent, 160) ? { agent_label: event.agent } : {}) });
    });

    pi.on("session_shutdown", async (_event, ctx) => {
        if (!isRoot(ctx)) return;
        const owner = current(ctx);
        shuttingDown = true;
        unsubscribe();
        running?.abort();
        await pending;
        if (owner) await send(owner, "session_shutdown");
        activation = undefined;
        children.clear();
        attention.clear();
    });
}

export default function logonomicExtension(pi: ExtensionAPI): void {
    if (process.env.LOGONOMIC_PROVIDER_HELPERS_DIR) installExtension(pi);
}
