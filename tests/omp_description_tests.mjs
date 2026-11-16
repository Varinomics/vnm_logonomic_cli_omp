import assert from "node:assert/strict";
import { descriptionToolName, registerDescriptionTool } from "../package/assets/native_description.ts";
import { installExtension } from "../package/assets/logonomic.ts";

// Native schema construction is exercised by the real-runtime acceptance
// gate; these fixtures exercise invocation, cancellation and source authority.
const field = () => ({ max() { return this; }, optional() { return this; }, describe() { return this; } });
const zod = { string: field, enum: field, object: () => ({ strict: () => ({}) }) };
const rootContext = { agent: { kind: "main" } };
const active = () => ({ sessionId: "native-root", nonce: "a".repeat(32) });
let binding = active();
let captured;
const calls = [];
let answer = (id) => ({ committed: true, accepted: true,
    description: { version: 1, request_id: id, accepted: true, reason: "" } });
assert(registerDescriptionTool({ zod, registerTool: tool => { captured = tool; } }, () => binding,
    async (owner, id, payload, ctx, signal) => {
        calls.push({ owner, id, payload, ctx, signal });
        return answer(id, signal);
    }));
assert.equal(captured.name, descriptionToolName);
assert.equal(captured.approval, "write", "The native approval tier remains a real mutation policy");
assert.equal(calls.length, 0, "Registering a public tool cannot invent a model submission");
const execute = (id, payload, signal, ctx = rootContext) => captured.execute(id, payload, signal, undefined, ctx);
const title = { kind: "title", title: "Model-authored subject" };
const accepted = await execute("call-1", title);
assert.equal(accepted.details.disposition, "accepted");
assert.equal(calls[0].owner, binding);
assert.deepEqual(calls[0].payload, title);
assert.deepEqual(await execute("call-1", title), accepted);
assert.equal(calls.length, 1, "A repeated native invocation returns its receipt without another submission");
assert.equal((await execute("call-1", { ...title, title: "Changed" })).details.reason, "request_conflict");
assert.equal(calls.length, 1);

answer = id => ({ committed: true, accepted: true,
    description: { version: 1, request_id: id, accepted: false, reason: "already_assigned" } });
assert.equal((await execute("refused", title)).details.disposition, "refused");
answer = () => ({ committed: true, accepted: true });
const uncertain = await execute("unknown", { kind: "summary_unchanged" });
assert.equal(uncertain.details.disposition, "uncertain", "Carrier acceptance cannot stand in for a host policy receipt");
assert.match(uncertain.content[0].text, /Do not retry/u);
const delivered = calls.length;
assert.deepEqual(await execute("unknown", { kind: "summary_unchanged" }), uncertain);
assert.equal(calls.length, delivered, "An unknown disposition cannot become an automatic fresh-key retry");

for (const payload of [{ ...title, title: "line\nline" }, { ...title, nonce: "model-controlled" },
    { kind: "summary" }, { kind: "summary_unchanged", summary: "replacement" },
    { kind: "summary", summary: "\n".repeat(16384) }]) {
    assert.equal((await execute(`invalid-${calls.length}`, payload)).details.reason, "description_invalid");
}
assert.equal(calls.length, delivered);
assert.equal((await execute("child", title, undefined, { agent: { kind: "sub" } })).details.reason, "description_unavailable");
const cancelled = new AbortController();
cancelled.abort();
assert.equal((await execute("cancelled", title, cancelled.signal)).details.reason, "cancelled_before_submission");
assert.equal(calls.length, delivered);

const lateAbort = new AbortController();
answer = id => {
    lateAbort.abort();
    return { committed: true, accepted: true,
        description: { version: 1, request_id: id, accepted: true, reason: "" } };
};
assert.equal((await execute("committed-before-cancel", title, lateAbort.signal)).details.disposition, "accepted",
    "Cancellation cannot revoke an already committed host policy receipt");

let release;
answer = id => new Promise(resolve => { release = () => resolve({ committed: true, accepted: true,
    description: { version: 1, request_id: id, accepted: true, reason: "" } }); });
const old = binding;
const retired = execute("old-activation", title);
binding = { sessionId: old.sessionId, nonce: "b".repeat(32) };
release();
assert.equal((await retired).details.reason, "stale_activation");
assert.equal(calls.at(-1).owner, old, "Pending publication retains its captured native activation object");

const handlers = new Map();
const nativeTools = new Map();
const state = { id: "native-root" };
const ctx = { agent: { kind: "main" }, cwd: "/work", sessionManager: {
    getSessionId: () => state.id, getSessionFile: () => undefined } };
const pi = { zod, on: (name, handler) => handlers.set(name, handler),
    events: { on: () => () => {} }, registerTool: tool => nativeTools.set(tool.name, tool),
    getActiveTools: () => [...nativeTools.keys()] };
const observed = [];
let pauseDescription = false;
let pauseMessage = false;
let releaseMessage;
installExtension(pi, async (request, signal) => {
    observed.push(structuredClone(request));
    if (pauseMessage && request.name === "message_end") return await new Promise(resolve => {
        releaseMessage = () => resolve({ committed: true, accepted: true });
    });
    if (request.operation === "context") return { committed: true, accepted: true, instruction: "Host title guidance." };
    if (request.operation === "description") {
        if (pauseDescription) return await new Promise(resolve => signal.addEventListener("abort", () => resolve(undefined), { once: true }));
        return { committed: true, accepted: true, description: {
            version: 1, request_id: request.body.request_id, accepted: true, reason: "" } };
    }
    return { committed: true, accepted: true, cooperative_description_version: 1 };
});
const emit = async (name, event = {}) => {
    const returned = await handlers.get(name)?.({ type: name, ...event }, ctx);
    await new Promise(resolve => setImmediate(resolve));
    return returned;
};
const invoke = (id, value, signal) => nativeTools.get(descriptionToolName).execute(id, value, signal, undefined, ctx);
assert.equal((await invoke("before", title)).details.reason, "description_unavailable");
await emit("session_start");
const activationNonce = observed[0].body.activation_nonce;
const context = await emit("before_agent_start", { reason: "prompt" });
assert.match(context.message.content, /Host title guidance/u);
assert.match(context.message.content, /logonomic_describe_session/u);
await emit("agent_start");
assert.equal((await invoke("actual-native-call", { kind: "summary", summary: "## Work\n- Ready." })).details.accepted, true);
const submission = observed.find(request => request.operation === "description");
assert.equal(submission.body.session_id, state.id);
assert.equal(submission.body.activation_nonce, activationNonce);
assert.equal(submission.body.request_id, "actual-native-call");
assert.equal(submission.name, "model_description");

pauseDescription = true;
const abort = new AbortController();
const interrupted = invoke("cancelled-in-flight", title, abort.signal);
await new Promise(resolve => setImmediate(resolve));
abort.abort();
assert.equal((await interrupted).details.disposition, "uncertain");
await emit("agent_end", { willContinue: false, messages: [{ role: "assistant", stopReason: "aborted" }] });

pauseMessage = true;
await emit("message_end", { message: { role: "assistant" } });
const queued = invoke("queued-before-clear", title);
const clear = emit("session_clear");
releaseMessage();
await clear;
assert.equal((await queued).details.reason, "stale_activation");
assert(!observed.some(request => request.operation === "description" && request.body.request_id === "queued-before-clear"),
    "A queued submission is discarded before dispatch when a clear retires its captured activation");
await emit("session_shutdown");
assert(observed.some(request => request.name === "agent_end"));
assert.equal(observed.at(-1).name, "session_shutdown");
assert(!observed.some(request => request.name === "logonomic_observation_lost"),
    "A cancelled optional description does not withdraw native terminal evidence");

console.log("PASS public native description tool, exact activation, receipt, replay and cancellation fixtures");
