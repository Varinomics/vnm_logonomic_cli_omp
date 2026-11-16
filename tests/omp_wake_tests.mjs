import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { chmodSync, copyFileSync, existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, statSync,
    symlinkSync, writeFileSync } from "node:fs";
import { createServer } from "node:net";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { nativeWakeState, startWakeBridge } from "../package/assets/native_wake.ts";

const driver = process.argv[2];
assert(driver, "The SDK-linked wake contract driver is required");
const root = mkdtempSync(join(tmpdir(), "omp-wake-contract-"));
const program = join(root, process.platform === "win32" ? "omp.exe" : "omp");
copyFileSync(driver, program);
chmodSync(program, 0o700);
const profile = join(root, "profile");
mkdirSync(profile, { mode: 0o700 });
const platform = process.platform === "win32" ? "windows-x86_64" :
    process.platform === "darwin" ? "macos-arm64" : "linux-x86_64";
const prepare = { launcher: { script_body: program, runner_key: "direct", environment_text: "" },
    launcher_argv: [program], working_directory: root, title_instruction: "",
    declared_assets: ["assets/logonomic.ts", "assets/native_children.ts", "assets/native_wake.ts"],
    launch_environment: { platform, effective_environment: { PATH: dirname(program) } },
    profile_storage: { version: 1, root: profile }, session_wake: { version: 1 } };

async function invoke(operation, payload, cancelAfter) {
    return await new Promise((resolve, reject) => {
        const process = spawn(driver, [], { stdio: ["pipe", "pipe", "pipe"], windowsHide: true });
        const timer = setTimeout(() => { process.kill(); reject(new Error("Wake fixture exceeded its bounded deadline")); }, 4000);
        let output = "";
        let error = "";
        process.stdout.on("data", bytes => { output += bytes; assert(output.length <= 65536); });
        process.stderr.on("data", bytes => { error += bytes; });
        process.on("error", reject);
        process.on("close", code => {
            clearTimeout(timer);
            if (code !== 0) reject(new Error(`Wake fixture exited ${code}: ${error}`));
            else {
                try { resolve(JSON.parse(output)); } catch (exception) { reject(exception); }
            }
        });
        process.stdin.end(JSON.stringify({ operation, payload, ...(cancelAfter !== undefined ? { cancel_after_ms: cancelAfter } : {}) }));
    });
}

let bridge;
let fakeServer;
try {
    const noOffer = structuredClone(prepare);
    delete noOffer.session_wake;
    const ordinary = (await invoke("prepare", noOffer)).payload;
    assert.equal(ordinary.session_wake, undefined);
    assert.equal(ordinary.disposition.prepared, true);
    const noProfile = structuredClone(prepare);
    delete noProfile.profile_storage;
    const unavailable = (await invoke("prepare", noProfile)).payload;
    assert.equal(unavailable.disposition.prepared, true, "Missing optional wake storage cannot disable ordinary launch");
    assert.equal(unavailable.session_wake.available, false);

    const prepared = await invoke("prepare", prepare);
    assert.equal(prepared.status, "success");
    const offer = prepared.payload.session_wake;
    assert.equal(offer.available, true);
    const directory = offer.binding.registration_directory;
    const secret = JSON.parse(readFileSync(join(directory, "registration.json"), "utf8"));
    assert(!JSON.stringify(prepared.payload).includes(secret.token), "Preparation retains credentials in private storage");
    if (process.platform !== "win32") assert.equal(statSync(directory).mode & 0o077, 0);
    const session = { provider_session_id: "exact-native-session", working_directory: root,
        launcher_argv: [program], launch_environment: prepare.launch_environment };
    const target = { session, correlation: { version: 1, transition_nonce: "a".repeat(32) }, binding: offer.binding };
    const notification = { ...target, notification_id: "notification-1", instruction: "Logonomic: read your newly committed mailbox." };
    assert.equal((await invoke("inspect", target)).payload.available, false, "Prepared alone is not a live native endpoint");
    const calls = [];
    let state = { providerSessionId: session.provider_session_id, transitionNonce: target.correlation.transition_nonce };
    const pi = { sendMessage: (message, options) => { calls.push({ message, options }); } };
    bridge = await startWakeBridge(pi, () => state, directory);
    assert(bridge, "The production native bridge binds the prepared private registration");
    assert.equal(await startWakeBridge(pi, () => state, directory), undefined,
        "An inherited or replacement process cannot claim another process's live registration");
    assert.equal((await invoke("inspect", target)).payload.available, true);
    assert.equal(calls.length, 0, "Inspection is read-only");

    const stale = structuredClone(target);
    stale.correlation.transition_nonce = "b".repeat(32);
    assert.equal((await invoke("inspect", stale)).payload.reason, "unbound_session");
    assert.equal((await invoke("notify", { ...notification, ...stale })).payload.disposition, "unavailable");
    for (const reason of ["native_busy", "native_attention_pending", "native_paused", "native_interrupted"]) {
        state.deferred = reason;
        assert.deepEqual((await invoke("notify", notification)).payload, { disposition: "deferred", reason });
        assert.equal(calls.length, 0, "A deferred notification makes no native submission");
    }
    delete state.deferred;
    const result = (await invoke("notify", notification)).payload;
    assert.deepEqual(result, { disposition: "uncertain", reason: "native_admission_unacknowledged" });
    assert.deepEqual(calls, [{ message: { customType: "logonomic-mailbox", content: notification.instruction,
        display: false, attribution: "agent" }, options: { deliverAs: "aside" } }]);
    state.deferred = "native_busy";
    assert.deepEqual((await invoke("notify", notification)).payload, result,
        "A duplicate already dispatched notification cannot become retriable because native work is now busy");
    assert.equal(calls.length, 1);
    delete state.deferred;
    assert.equal((await invoke("notify", { ...notification, notification_id: "cancel-before" }, 0)).payload.disposition, "unavailable");
    assert.equal(calls.length, 1, "Cancellation before dispatch causes no native call");

    state.transitionNonce = "b".repeat(32);
    assert.equal((await invoke("notify", notification)).payload.disposition, "unavailable",
        "A same-ID native clear invalidates the old activation binding");
    const newTarget = { ...target, correlation: { version: 1, transition_nonce: state.transitionNonce } };
    await invoke("notify", { ...notification, ...newTarget });
    assert.equal(calls.length, 2);
    if (process.platform !== "win32") {
        chmodSync(directory, 0o755);
        assert.equal((await invoke("inspect", newTarget)).payload.available, false,
            "A registry that lost owner-only protection cannot supply native authority");
        chmodSync(directory, 0o700);
        const alias = join(root, "alias");
        mkdirSync(alias);
        symlinkSync(directory, join(alias, `wake-${offer.binding.registration_id}`), "dir");
        const linked = { ...newTarget, binding: { ...offer.binding,
            registration_directory: join(alias, `wake-${offer.binding.registration_id}`) } };
        assert.equal((await invoke("inspect", linked)).payload.available, false, "Linked registries cannot redirect a launch binding");
    }
    bridge.close();
    bridge = undefined;
    assert.equal(existsSync(directory), false);
    assert.equal((await invoke("inspect", newTarget)).payload.available, false, "Native shutdown revokes the endpoint");

    const second = (await invoke("prepare", prepare)).payload.session_wake;
    let received = 0;
    const peers = new Set();
    fakeServer = createServer(socket => {
        peers.add(socket);
        socket.on("error", () => {});
        socket.on("close", () => peers.delete(socket));
        socket.once("data", () => { received++; }); // Deliberately withhold the response after dispatch.
    });
    await new Promise(resolve => fakeServer.listen(0, "127.0.0.1", resolve));
    writeFileSync(join(second.binding.registration_directory, "endpoint.json"), JSON.stringify({ version: 1,
        registration_id: second.binding.registration_id, instance_id: "c".repeat(32), port: fakeServer.address().port }), { mode: 0o600 });
    const cancelled = await invoke("notify", { ...notification, binding: second.binding }, 100);
    assert.equal(received, 1);
    assert.equal(cancelled.payload.disposition, "uncertain", "Cancellation after socket dispatch cannot promise non-admission");
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(peers.size, 0, "Canceled native transport closes its socket under bounded custody");
    await new Promise(resolve => fakeServer.close(resolve));
    fakeServer = undefined;

    const native = { sessionId: "native", nonce: "d".repeat(32), lost: false, turnId: "", interrupted: false, attention: false };
    const modes = { plan: { status: "inactive" }, goal: { status: "inactive" }, loop: { status: "inactive" } };
    const ctx = { hasUI: true, sessionManager: { getSessionId: () => native.sessionId }, isIdle: () => true,
        sessionModes: { getState: () => modes } };
    assert.equal(nativeWakeState(ctx, native).deferred, undefined);
    modes.plan.status = "active";
    assert.equal(nativeWakeState(ctx, native).deferred, "native_paused");
    modes.plan.status = "inactive";
    modes.goal.status = "paused";
    assert.equal(nativeWakeState(ctx, native).deferred, "native_paused");
    modes.goal.status = "inactive";
    native.interrupted = true;
    assert.equal(nativeWakeState(ctx, native).deferred, "native_interrupted");
    native.interrupted = false;
    native.attention = true;
    assert.equal(nativeWakeState(ctx, native).deferred, "native_attention_pending");
    native.attention = false;
    assert.equal(nativeWakeState({ ...ctx, isIdle: () => false }, native).deferred, "native_busy");
    assert.equal(nativeWakeState({ ...ctx, sessionModes: undefined }, native).unavailable, "native_mode_query_unavailable");
    console.log("PASS SDK wake preparation, exact native binding, policy, deduplication and cancellation contracts");
} finally {
    bridge?.close();
    fakeServer?.close();
    rmSync(root, { recursive: true, force: true });
}
