import assert from "node:assert/strict";
import { installExtension } from "../package/assets/logonomic.ts";

// Fixtures follow shared-events.ts, extensions/types.ts, and the public
// task:subagent:lifecycle contract at the recorded upstream source revision.
function harness(transport) {
    const handlers = new Map();
    const channels = new Map();
    const delivered = [];
    const pi = {
        on(name, handler) { handlers.set(name, handler); },
        events: { on(name, handler) { channels.set(name, handler); return () => channels.delete(name); } },
    };
    installExtension(pi, async (request, signal) => {
        delivered.push(structuredClone(request));
        return transport ? await transport(request, signal) : { committed: true, accepted: true };
    });
    const state = { id: "session-A", file: "/sessions/A.jsonl", idle: true,
        cwd: "/work/project/nested dir", usage: { tokens: 1200, contextWindow: 16000, percent: 7.5 } };
    const ctx = { agent: { kind: "main", id: "Main", name: "main", depth: 0 }, hasUI: true,
        sessionManager: { getSessionId: () => state.id, getSessionFile: () => state.file },
        get cwd() { return state.cwd; }, getContextUsage: () => state.usage,
        isIdle: () => state.idle };
    return { handlers, channels, delivered, state, ctx,
        emit: async (name, event = {}, context = ctx) => {
            const result = await handlers.get(name)?.({ type: name, ...event }, context);
            await new Promise(resolve => setImmediate(resolve));
            return result;
        },
        child: event => channels.get("task:subagent:lifecycle")?.(event),
        drain: () => new Promise(resolve => setImmediate(resolve)) };
}

const h = harness();
assert(!h.handlers.has("session_stop"), "the native stop veto is not completion evidence");
await h.emit("session_start");
const firstNonce = h.delivered[0].body.activation_nonce;
assert.match(firstNonce, /^[0-9a-f]{32}$/u);
const sub = { ...h.ctx, agent: { kind: "sub", id: "0-task", name: "task", depth: 0 } };
await h.emit("session_start", {}, sub);
await h.emit("agent_start", {}, sub);
assert.equal(h.delivered.length, 1, "even depth-zero child bindings cannot create root transitions");

await h.emit("agent_start");
const turn = h.delivered.at(-1).body.turn_id;
h.state.idle = false; // Actual callbacks precede the owning prompt's unwind.
await h.emit("agent_end", { willContinue: true, messages: [{ role: "assistant", stopReason: "stop" }] });
assert.equal(h.delivered.at(-1).name, "agent_start", "continuations retain busy state");
await h.emit("agent_start");
assert.equal(h.delivered.at(-1).body.turn_id, turn);
await h.emit("agent_end", { willContinue: undefined, messages: [{ role: "assistant", stopReason: "stop" }] });
assert.equal(h.delivered.at(-1).name, "agent_end", "the single native terminal notification survives prompt unwinding");
assert.equal(h.delivered.at(-1).body.turn_id, turn);
assert.equal(h.delivered.at(-1).body.stop_reason, "stop");
assert.equal(h.delivered.filter(request => request.name === "agent_end").length, 1);
await h.emit("agent_start");
const nextTurn = h.delivered.at(-1).body.turn_id;
assert.notEqual(nextTurn, turn, "a completed turn id cannot bleed into the next user prompt");
await h.emit("agent_end", { willContinue: false, messages: [{ role: "assistant", stopReason: "aborted" }] });
assert.equal(h.delivered.at(-1).body.stop_reason, "aborted");
assert.equal(h.delivered.at(-1).body.turn_id, nextTurn);
assert.equal(h.delivered.at(-1).body.terminal, true, "false native willContinue retains terminal semantics while unwinding");

await h.emit("tool_approval_requested", { sessionId: "session-A", toolCallId: "approve-1", toolName: "bash" });
await h.emit("tool_approval_resolved", { sessionId: "session-A", toolCallId: "approve-1", approved: false });
assert.equal(h.delivered.at(-2).body.request_id, h.delivered.at(-1).body.request_id);
await h.emit("plan_review_requested", { sessionId: "session-A", reviewId: "review-1" });
await h.emit("plan_review_resolved", { sessionId: "session-A", reviewId: "review-1", choice: undefined, by: "local" });
assert.equal(h.delivered.at(-1).name, "plan_review_resolved", "dismissal also ends the observed wait");
await h.emit("tool_execution_start", { toolName: "ask", toolCallId: "question-1", args: { questions: ["private"] } });
await h.emit("tool_execution_end", { toolName: "ask", toolCallId: "question-1", result: "private", isError: false });
assert.equal(h.delivered.at(-1).name, "ask_execution_end");
assert(!JSON.stringify(h.delivered).includes("private"), "prompts, tool arguments and answers never enter telemetry");

h.child({ id: "0-Explore", agent: "explore", status: "started", sessionFile: "/sessions/child-A.jsonl" });
await h.drain();
assert.equal(h.delivered.at(-1).body.agent_id, "0-Explore");
await h.emit("session_clear");
const cleared = h.delivered.at(-1);
assert.equal(cleared.name, "session_clear");
assert.equal(cleared.body.session_id, "session-A");
assert.equal(cleared.body.previous_nonce, firstNonce);
assert.notEqual(cleared.body.activation_nonce, firstNonce);
const count = h.delivered.length;
h.child({ id: "0-Explore", agent: "explore", status: "completed", sessionFile: "/sessions/child-A.jsonl" });
await h.drain();
assert.equal(h.delivered.length, count, "a child from the cleared epoch cannot finish a replacement child");
h.child({ id: "0-Explore", agent: "explore", status: "started", sessionFile: "/sessions/child-B.jsonl" });
await h.drain();
h.child({ id: "0-Explore", agent: "explore", status: "completed", sessionFile: "/sessions/child-A.jsonl" });
await h.drain();
assert.equal(h.delivered.at(-1).body.status, "started", "reused native ids also retain their exact child file");
h.child({ id: "0-Explore", agent: "explore", status: "failed", sessionFile: "/sessions/child-B.jsonl" });
await h.drain();
assert.equal(h.delivered.at(-1).body.status, "failed");
await h.emit("session_shutdown");
assert.equal(h.delivered.at(-1).name, "session_shutdown");
assert.equal(h.channels.size, 0);

let contextCalls = 0;
const context = harness(request => {
    if (request.operation === "context") contextCalls++;
    return { committed: true, accepted: true, instruction: request.operation === "context" ? "host-owned context" : "" };
});
await context.emit("session_start");
const first = await context.emit("before_agent_start", { reason: "prompt", prompt: "private prompt" });
assert.deepEqual(first, { message: { customType: "logonomic", content: "host-owned context", display: false } });
assert.deepEqual(await context.emit("before_agent_start", { reason: "prompt" }), first);
assert.equal(contextCalls, 1, "native prompt preparation retries retain the accepted one-shot context");
await context.emit("agent_start");
await context.emit("before_agent_start", { reason: "prompt" });
assert.equal(contextCalls, 2, "the next accepted prompt obtains fresh per-turn context");
const contextRequest = context.delivered.find(request => request.operation === "context");
assert.equal(contextRequest.body.activation_nonce, context.delivered[0].body.activation_nonce);
assert.equal(contextRequest.name, "before_agent_start");

let resolveContext;
const deferred = harness(request => request.operation === "context" ?
    new Promise(resolve => { resolveContext = resolve; }) : { committed: true, accepted: true });
await deferred.emit("session_start");
const inFlight = deferred.emit("before_agent_start", { reason: "prompt" });
await deferred.drain();
deferred.state.id = "session-B";
deferred.state.file = "/sessions/B.jsonl";
await deferred.emit("session_switch", { reason: "resume" });
resolveContext({ committed: true, accepted: true, instruction: "old activation context" });
assert.equal(await inFlight, undefined, "asynchronous context from a prior activation is suppressed");

const failed = harness(request => request.name === "agent_start" ? undefined : { committed: true, accepted: true });
await failed.emit("session_start");
await failed.emit("agent_start");
await failed.emit("agent_end", { willContinue: undefined, messages: [{ role: "assistant", stopReason: "stop" }] });
assert.equal(failed.delivered.at(-1).name, "logonomic_observation_lost");
const losses = failed.delivered.length;
await failed.emit("agent_start");
assert.equal(failed.delivered.length, losses, "after admitted delivery loss the same activation remains quarantined");

const unknownSemantics = harness();
await unknownSemantics.emit("session_start");
await unknownSemantics.emit("agent_start");
await unknownSemantics.emit("agent_end", { messages: [{ role: "assistant", stopReason: "stop" }] });
assert.equal(unknownSemantics.delivered.at(-1).name, "logonomic_observation_lost",
    "an older event shape without native continuation evidence must never imply successful completion");

let releaseStart;
let startAborted = false;
const closing = harness((request, signal) => request.name === "agent_start" ?
    new Promise(resolve => {
        releaseStart = resolve;
        signal.addEventListener("abort", () => { startAborted = true; resolve(undefined); }, { once: true });
    }) : { committed: true, accepted: true });
await closing.emit("session_start");
await closing.emit("agent_start");
closing.state.idle = false;
await closing.emit("agent_end", { willContinue: undefined, messages: [{ role: "assistant", stopReason: "stop" }] });
const shutdown = closing.emit("session_shutdown");
await closing.drain();
assert.equal(startAborted, false, "normal shutdown must not cancel an already captured lifecycle delivery");
releaseStart({ committed: true, accepted: true });
await shutdown;
assert.deepEqual(closing.delivered.map(request => request.name),
    ["session_start", "agent_start", "agent_end", "session_shutdown"],
    "fire-and-forget native terminal notification drains before session shutdown under immediate EOF");
assert.equal(closing.delivered[1].body.turn_id, closing.delivered[2].body.turn_id);

const telemetry = harness();
await telemetry.emit("session_start");
const initialSnapshot = telemetry.delivered[0];
assert.deepEqual(initialSnapshot.body.context, { used_tokens: 1200, context_window: 16000 });
assert.deepEqual(initialSnapshot.body.execution, { state: "confirmed", cwd: "/work/project/nested dir" });
telemetry.state.usage = { tokens: 17000, contextWindow: 16000, percent: 106.25 };
telemetry.state.cwd = "/work/moved/nested dir";
await telemetry.emit("message_end", { message: { role: "assistant", usage: { totalTokens: 987654321 } } });
assert.deepEqual(telemetry.delivered.at(-1).body.context, { used_tokens: 17000, context_window: 16000 },
    "native current occupancy can exceed capacity; cumulative message billing is never substituted");
assert.equal(telemetry.delivered.at(-1).body.execution.cwd, "/work/moved/nested dir");
assert.equal(telemetry.delivered.at(-1).body.activation_nonce, initialSnapshot.body.activation_nonce);
assert(Number(telemetry.delivered.at(-1).body.sequence) > Number(initialSnapshot.body.sequence));
telemetry.state.usage = undefined;
telemetry.state.cwd = "relative-dir";
await telemetry.emit("agent_start");
assert.deepEqual(telemetry.delivered.at(-1).body.context, { usage_unavailable: true });
assert.deepEqual(telemetry.delivered.at(-1).body.execution, { state: "unknown", cwd: "" });
const beforeCompaction = telemetry.delivered.length;
await telemetry.emit("auto_compaction_start", {});
await telemetry.emit("auto_compaction_end", { aborted: false, result: { tokensBefore: 20000 } });
assert.equal(telemetry.delivered.length, beforeCompaction, "attempt/end notifications do not double-count installed compaction");
await telemetry.emit("session_compact", { compactionEntry: { id: "compact-42", summary: "private summary" } });
assert.deepEqual(telemetry.delivered.at(-1).body.context, { usage_unavailable: true, compaction_id: "compact-42" });
assert(!JSON.stringify(telemetry.delivered).includes("private summary"));
const priorChildTelemetry = telemetry.delivered.length;
await telemetry.emit("message_end", { message: { role: "assistant" } }, sub);
await telemetry.emit("session_compact", { compactionEntry: { id: "child-compaction" } }, sub);
assert.equal(telemetry.delivered.length, priorChildTelemetry, "a child query never mutates root context occupancy");

const optionalRefusal = harness(request => request.name === "message_end" ? undefined :
    { committed: true, accepted: true });
await optionalRefusal.emit("session_start");
await optionalRefusal.emit("agent_start");
await optionalRefusal.emit("message_end", { message: { role: "assistant" } });
await optionalRefusal.emit("agent_end", { willContinue: false, messages: [{ role: "assistant", stopReason: "stop" }] });
assert.equal(optionalRefusal.delivered.at(-1).name, "agent_end", "optional telemetry refusal cannot quarantine root lifecycle");

let cancelledStall = false;
const stalled = harness((request, signal) => request.name === "agent_start" ?
    new Promise(resolve => signal.addEventListener("abort", () => {
        cancelledStall = true;
        resolve(undefined);
    }, { once: true })) : { committed: true, accepted: true });
await stalled.emit("session_start");
await stalled.emit("agent_start");
await stalled.emit("agent_end", { willContinue: false, messages: [{ role: "assistant", stopReason: "stop" }] });
await stalled.emit("session_shutdown");
assert.equal(cancelledStall, true, "the bounded shutdown drain cancels a helper that does not settle");
assert.deepEqual(stalled.delivered.map(request => request.name),
    ["session_start", "agent_start", "logonomic_observation_lost"],
    "shutdown budget exhaustion reports observation loss without fabricating completion");

console.log("PASS native extension event, activation, attention, child, context and delivery-loss fixtures");
