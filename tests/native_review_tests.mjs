import assert from "node:assert/strict";
import { installReviewGuard, parseReviewPolicy, permitsReviewCall } from "../package/assets/native_review.ts";

const policy = parseReviewPolicy(JSON.stringify({ version: 1, scope: "a".repeat(32),
    tools: [{ server: "host:s0", action: "report_result" }] }));
const builtin = name => ({ name, sourceInfo: { source: "builtin", path: `<builtin:${name}>` } });
const taskTool = { name: "mcp_host_report_result", sourceInfo: { source: "mcp", path: "<mcp:mcp_host_report_result>" },
    mcpServerName: "host:s0", mcpToolName: "report_result" };
const tools = [...["read", "grep", "glob", "write", "bash", "task"].map(builtin), taskTool,
    { name: "mcp_other_mutate", sourceInfo: { source: "mcp", path: "<mcp:mcp_other_mutate>" },
        mcpServerName: "other", mcpToolName: "mutate", mcpReadOnlyHint: true }];
const permits = (name, input, inventory = tools) => permitsReviewCall(policy, inventory, { toolName: name, input });

assert.equal(permits("read", { path: "source.cpp:1-90" }), true);
assert.equal(permits("read", { path: "C:\\repo\\source.cpp" }), true);
assert.equal(permits("grep", { pattern: "review", path: ["src", "tests"] }), true);
assert.equal(permits("glob", { path: "**/*.cpp" }), true);
assert.equal(permits("grep", { pattern: "default working directory" }), true);
assert.equal(permits("read", { path: "xd://" }), true);
assert.equal(permits("read", { path: "xd://mcp_host_report_result" }), true);
assert.equal(permits("write", { path: "xd://mcp_host_report_result", content: "{\"message\":\"review result\"}" }), true);
assert.equal(permits("mcp_host_report_result", { message: "review result" }), true);

for (const path of [
    "review.txt", "./www.example/notes.txt", "/tmp/review.txt", "~/repo/review.txt",
    '"folder with spaces/review.txt"', "src;tests", "src,tests", "{src,tests}/**/*.cpp",
    "source.cpp:1-90", "Makefile:raw:1-20,30+5", "README:conflicts", "README:-20",
    String.raw`C:\repo\source.cpp`, "C:/repo/source.cpp", String.raw`C:\Program Files\source.cpp:12`,
    String.raw`\\?\C:\repo\source.cpp`, String.raw`\\server\share\source.cpp`,
]) for (const name of ["read", "grep", "glob"]) {
    assert.equal(permits(name, { path }), true, `${name} keeps filesystem spelling: ${path}`);
}

// read/grep/glob share normalization and list recovery. In particular, native
// read tries opaque MCP resource URIs before falling back to a missing file;
// the enclosing built-in name does not authorize that separate resource call.
for (const path of [
    "reviewprobe:mutate", "urn:example:document", "custom:raw:mutate",
    "http:/host/mutate", "https:/host/mutate", "HTTP:/host/mutate",
    "local:/notes", String.raw`local:\notes`, "/repo/local://notes", String.raw`C:\repo\local://notes`,
    "@local://notes", "file:///tmp/review.txt", "source.cpp:conflict://1",
    "review.txt;reviewprobe:mutate", "reviewprobe:mutate;review.txt", "review.txt,reviewprobe:mutate",
    "review.txt reviewprobe:mutate", '"reviewprobe:mutate"', 'review.txt;"reviewprobe:mutate"',
    "{review.txt,reviewprobe:mutate}", "review.txt;https:/host/mutate",
    "review.txt;www.host/mutate", 'review.txt;"www.host/mutate"',
    // The *whole* input may be opaque even when all delimiter fragments look
    // local; do not authorize it by independently accepting `Makefile:12`.
    "Makefile:12 other", "Makefile:12,other", "Makefile:raw other", "Makefile:12;other",
    "custom:raw ", "custom:12 ", "custom:raw\t", "custom:conflicts\u00a0",
    "C://repo/file", String.raw`C:\\repo\file`, String.raw`C:/\repo/file`,
    "xd:/mcp_host_report_result", "xd://mcp_host_report_result:raw",
    "xd://mcp_host_report_result?query=1", "xd://mcp_host_report_result;review.txt",
    "review.txt;xd://mcp_host_report_result", "xd://;review.txt",
]) for (const name of ["read", "grep", "glob"]) {
    assert.equal(permits(name, { path }), false, `${name} refuses routed/ambiguous spelling: ${path}`);
}
assert.equal(permits("grep", { paths: ["src", "reviewprobe:mutate"] }), false);
assert.equal(permits("glob", { path: ["src", "local:/notes"] }), false);
assert.equal(permits("read", { path: "xd://mcp_host_report_result" }, tools), true,
    "The exact authorized device documentation remains available");
assert.equal(permits("write", { path: "xd://mcp_host_report_result", content: "{}" }), true,
    "The exact authorized host action remains available");

for (const [name, input] of [
    ["bash", { command: "touch forbidden" }], ["task", { task: "modify a file" }],
    ["write", { path: "source.cpp", content: "replacement" }],
    ["write", { path: "local://scratch", content: "replacement" }],
    ["write", { path: "xd://mcp_other_mutate", content: "{}" }],
    ["write", { path: "xd://mcp_host_report_result/other", content: "{}" }],
    ["write", { path: "xd://mcp_host_report_result", content: "[]" }],
    ["write", { path: "xd://mcp_host_report_result", content: "invalid" }],
    ["read", { path: "ssh://host/command" }], ["read", { path: "src;ssh://host/command" }],
    ["read", { path: "https://host/mutate" }], ["read", { path: "www.host/mutate" }],
    ["read", { path: "xd://mcp_other_mutate" }], ["read", {}],
    ["grep", { pattern: "x", paths: ["src", "ssh://host"] }],
    ["glob", { path: ["src", null] }], ["mcp_other_mutate", {}], ["invented", {}],
]) assert.equal(permits(name, input), false, `${name} must not pass: ${JSON.stringify(input)}`);

const shadowed = tools.map(tool => tool.name === "read"
    ? { ...tool, sourceInfo: { source: "builtin", path: "/project/.omp/tools/read.ts" } } : tool);
assert.equal(permits("read", { path: "file" }, shadowed), false);
assert.equal(permits("read", { path: "file" }, [...tools, builtin("read")]), false);
const unrelated = tools.map(tool => tool === taskTool ? { ...tool, mcpServerName: "wrong" } : tool);
assert.equal(permits("mcp_host_report_result", {}, unrelated), false);
const spoofed = tools.map(tool => tool === taskTool
    ? { ...tool, sourceInfo: { source: "mcp", path: "/project/.omp/tools/spoof.ts" } } : tool);
assert.equal(permits("mcp_host_report_result", {}, spoofed), false);
for (const value of ["", "null", "{}", JSON.stringify({ ...policy, version: 2 }),
    JSON.stringify({ ...policy, extra: true }), JSON.stringify({ ...policy, tools: [...policy.tools, ...policy.tools] }),
    JSON.stringify({ ...policy, tools: [{ server: "host", action: "bad\nvalue" }] }), " ".repeat(32769)]) {
    assert.throws(() => parseReviewPolicy(value));
}

function fixture(inventory = tools, activation = async () => {}) {
    const callbacks = new Map();
    const entries = [];
    let active = inventory.map(tool => tool.name);
    const ctx = { agent: { kind: "main" }, sessionManager: { getSessionId: () => "session-A" } };
    const api = {
        on: (name, callback) => callbacks.set(name, callback),
        getAllTools: () => inventory, getActiveTools: () => active,
        setActiveTools: async names => { await activation(); active = names; },
        appendEntry: (name, data) => entries.push({ name, data }),
    };
    installReviewGuard(api, policy);
    return { ctx, callbacks, entries, active: () => active };
}

const live = fixture();
assert.equal(live.callbacks.get("tool_call")({ toolName: "read", input: { path: "file" } }, live.ctx).block, true);
await live.callbacks.get("session_start")({}, live.ctx);
assert.deepEqual(live.entries, [{ name: "logonomic-read-only", data: {
    version: 1, scope: policy.scope, session_id: "session-A" } }]);
assert.deepEqual(live.active(), ["read", "grep", "glob", "write", "mcp_host_report_result"]);
assert.equal(live.callbacks.get("tool_call")({ toolName: "read", input: { path: "file" } }, live.ctx), undefined);
assert.equal(live.callbacks.get("tool_call")({ toolName: "read", input: { path: "file" } },
    { ...live.ctx, agent: { kind: "subagent" } }).block, true);
assert.deepEqual(live.callbacks.get("input")({ text: " /config change" }, live.ctx), { handled: true });
assert.equal(live.callbacks.get("input")({ text: "Review source" }, live.ctx), undefined);
live.callbacks.get("session_clear")();
assert.equal(live.callbacks.get("tool_call")({ toolName: "read", input: { path: "file" } }, live.ctx).block, true);
await live.callbacks.get("session_start")({}, live.ctx);
assert.equal(live.entries.length, 1, "A retired task cannot rebind to another native activation");

const replaced = fixture(shadowed);
await replaced.callbacks.get("session_start")({}, replaced.ctx);
assert.deepEqual(replaced.entries, [], "A replaced read implementation cannot acknowledge the native read boundary");
let release;
const pending = fixture(tools, () => new Promise(resolve => { release = resolve; }));
const starting = pending.callbacks.get("session_start")({}, pending.ctx);
pending.callbacks.get("session_switch")();
release();
await starting;
assert.deepEqual(pending.entries, [], "A transition during activation must not acknowledge the retired session");
console.log("Native read-only execution guard fixtures passed.");
