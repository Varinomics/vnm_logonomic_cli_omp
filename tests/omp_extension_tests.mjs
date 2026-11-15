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
    const state = { id: "session-A", file: "/sessions/A.jsonl", idle: true };
    const ctx = { agent: { kind: "main", id: "Main", name: "main", depth: 0 }, hasUI: true,
        sessionManager: { getSessionId: () => state.id, getSessionFile: () => state.file },
        isIdle: () => state.idle };
    return { handlers, channels, delivered, state, ctx,
        emit: async (name, event = {}, context = ctx) => await handlers.get(name)?.({ type: name, ...event }, context),
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
await h.emit("agent_end", { willContinue: true, messages: [{ role: "assistant", stopReason: "stop" }] });
assert.equal(h.delivered.at(-1).name, "agent_start", "continuations retain busy state");
await h.emit("agent_start");
assert.equal(h.delivered.at(-1).body.turn_id, turn);
h.state.idle = false;
await h.emit("agent_end", { willContinue: undefined, messages: [{ role: "assistant", stopReason: "stop" }] });
assert.equal(h.delivered.at(-1).name, "agent_start", "a late settle cannot finish a newer streaming run");
h.state.idle = true;
await h.emit("agent_end", { willContinue: undefined, messages: [{ role: "assistant", stopReason: "aborted" }] });
assert.equal(h.delivered.at(-1).body.stop_reason, "aborted");
assert.equal(h.delivered.at(-1).body.terminal, true, "undefined native willContinue has documented terminal semantics");

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

console.log("PASS native extension event, activation, attention, child, context and delivery-loss fixtures");
