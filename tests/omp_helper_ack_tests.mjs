import assert from "node:assert/strict";
import { chmodSync, existsSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { setTimeout as delay } from "node:timers/promises";
import { installExtension } from "../package/assets/logonomic.ts";

// POSIX shebang fixture; the SDK's actual helper owns and reaps the stalled
// acknowledgment process. No host IPC or simulated native completion is used.
const helpers = process.argv[2];
assert(helpers && process.platform !== "win32");
const directory = mkdtempSync(join(tmpdir(), "omp-ack-fixture-"));
const wrapper = join(directory, "wrapper.mjs");
const journal = join(directory, "journal.jsonl");
const prior = { ...process.env };
try {
    writeFileSync(wrapper, `#!${process.execPath}
import { appendFileSync, readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";
const root = ${JSON.stringify(directory)};
const args = process.argv.slice(2);
const body = args.includes("--args-stdin") ? JSON.parse(readFileSync(0, "utf8")) : undefined;
const name = body?.event?.name;
if (name) writeFileSync(join(root, body.idempotency_key), name);
appendFileSync(join(root, "journal.jsonl"), JSON.stringify({args, name, pid:process.pid}) + "\\n");
if (args[1] === "acknowledge" && readFileSync(join(root, args[2]), "utf8") === "agent_end") {
    writeFileSync(join(root, "stalled-ack-pid"), String(process.pid));
    setInterval(() => {}, 1000);
} else {
    const result = args[0] === "request-title-instruction" ?
        {accepted:true,instruction_required:false,instruction:""} : {accepted:true,epoch:1};
    process.stdout.write(JSON.stringify(result));
}
`);
    chmodSync(wrapper, 0o700);
    Object.assign(process.env, { LOGONOMIC_PROVIDER_HELPERS_DIR: helpers, LOGONOMIC_VNMCTL_PATH: wrapper,
        LOGONOMIC_SESSION_CAPABILITY: "a".repeat(64), LOGONOMIC_PROVIDER_PACKAGE_ID: "logonomic.cli.omp",
        LOGONOMIC_PROVIDER_GENERATION_DIGEST: "b".repeat(64), LOGONOMIC_PROVIDER_GENERATION_NONCE: "c".repeat(32) });
    delete process.env.VNM_OMP_WAKE_REGISTRATION;
    const handlers = new Map();
    const pi = { on(name, callback) { handlers.set(name, callback); }, events: { on() { return () => {}; } } };
    const ctx = { agent: { kind: "main" }, hasUI: true, cwd: directory,
        sessionManager: { getSessionId: () => "native-ack-fixture", getSessionFile: () => undefined },
        model: { provider: "fixture", id: "model" }, getContextUsage: () => undefined, isIdle: () => false };
    installExtension(pi);
    await handlers.get("session_start")({}, ctx);
    handlers.get("agent_start")({}, ctx);
    // The real SDK context call waits for earlier queued lifecycle delivery.
    await handlers.get("before_agent_start")({ reason: "prompt" }, ctx);
    handlers.get("agent_end")({ willContinue: false, messages: [{ role: "assistant", stopReason: "stop" }] }, ctx);
    await handlers.get("session_shutdown")({}, ctx);
    const entries = readFileSync(journal, "utf8").trim().split("\n").map(JSON.parse);
    assert.deepEqual(entries.filter(entry => entry.name).map(entry => entry.name),
        ["session_start", "agent_start", "agent_end", "session_shutdown"],
        "Canceling post-commit journal release cannot discard the real terminal event or replace shutdown with loss");
    const pidFile = join(directory, "stalled-ack-pid");
    assert(existsSync(pidFile), "The production helper reached journal acknowledgment after committing agent_end");
    const pid = Number(readFileSync(pidFile, "utf8"));
    let alive = true;
    for (let attempt = 0; attempt < 30 && alive; ++attempt) {
        try { process.kill(pid, 0); await delay(10); } catch (error) { assert.equal(error.code, "ESRCH"); alive = false; }
    }
    assert.equal(alive, false, "The SDK helper reaps the canceled acknowledgment child before the extension completes shutdown");
    console.log("PASS committed terminal delivery survives bounded acknowledgment cancellation and reaps its wrapper");
} finally {
    for (const key of Object.keys(process.env)) if (!(key in prior)) delete process.env[key];
    Object.assign(process.env, prior);
    rmSync(directory, { recursive: true, force: true });
}
