/** Native tool-call boundary for one host-owned, read-only RPC task. */

type Tool = {
    name?: unknown;
    sourceInfo?: { source?: unknown; path?: unknown };
    mcpServerName?: unknown;
    mcpToolName?: unknown;
};

type ExpectedTool = { server: string; action: string };
type ReviewPolicy = { version: 1; scope: string; tools: ExpectedTool[] };
type Context = { agent?: { kind?: string }; sessionManager: { getSessionId(): string } };
type ToolCall = { toolName?: unknown; input?: unknown };
type NativeApi = {
    on(name: string, handler: (...args: any[]) => unknown): void;
    getAllTools(): Tool[];
    getActiveTools(): string[];
    setActiveTools(names: string[]): Promise<void>;
    appendEntry(name: string, data: unknown): void;
};

const FILE_TOOLS = new Set(["read", "grep", "glob"]);
const DENIED = { block: true, reason: "This Logonomic review permits native file reads and its exact host task-control actions only." };

function object(value: unknown): value is Record<string, unknown> {
    return value !== null && typeof value === "object" && !Array.isArray(value);
}

export function parseReviewPolicy(text: string): ReviewPolicy {
    if (Buffer.byteLength(text) > 32768) throw new Error("Read-only policy exceeds its bound.");
    const value: unknown = JSON.parse(text);
    if (!object(value) || Object.keys(value).sort().join(",") !== "scope,tools,version" ||
        value.version !== 1 || typeof value.scope !== "string" || !/^[0-9a-f]{32}$/.test(value.scope) ||
        !Array.isArray(value.tools) || value.tools.length > 512) {
        throw new Error("Malformed read-only task policy.");
    }
    const seen = new Set<string>();
    for (const tool of value.tools) {
        if (!object(tool) || Object.keys(tool).sort().join(",") !== "action,server" ||
            typeof tool.server !== "string" || !tool.server || Buffer.byteLength(tool.server) > 128 ||
            typeof tool.action !== "string" || !tool.action || Buffer.byteLength(tool.action) > 64 ||
            /[\u0000-\u001f]/.test(tool.server + tool.action)) {
            throw new Error("Malformed read-only task-control identity.");
        }
        const key = JSON.stringify([tool.server, tool.action]);
        if (seen.has(key)) throw new Error("Duplicate read-only task-control identity.");
        seen.add(key);
    }
    return value as ReviewPolicy;
}

function nativeBuiltin(tool: Tool, name: string): boolean {
    // Native getAllTools() retains a filesystem origin when an extension
    // replaces a built-in name. A name or a readOnlyHint alone is insufficient.
    return tool.name === name && tool.sourceInfo?.source === "builtin" &&
        tool.sourceInfo.path === `<builtin:${name}>` &&
        tool.mcpServerName === undefined && tool.mcpToolName === undefined;
}

function hostTool(tool: Tool, policy: ReviewPolicy): boolean {
    return typeof tool.name === "string" && tool.sourceInfo?.source === "mcp" &&
        tool.sourceInfo.path === `<mcp:${tool.name}>` &&
        policy.tools.some(expected => expected.server === tool.mcpServerName && expected.action === tool.mcpToolName);
}

function exactTool(tools: Tool[], name: string): Tool | undefined {
    const matches = tools.filter(tool => tool.name === name);
    return matches.length === 1 ? matches[0] : undefined;
}

function filePaths(input: Record<string, unknown>, name: string): string[] | undefined {
    const value = input.path ?? (name === "grep" ? input.paths : undefined);
    if (value === undefined) return name === "read" ? undefined : [];
    const paths = Array.isArray(value) ? value : [value];
    return paths.every(path => typeof path === "string" && path.length > 0 && !path.includes("\0"))
        ? paths as string[] : undefined;
}

export function permitsReviewCall(policy: ReviewPolicy, tools: Tool[], event: ToolCall): boolean {
    if (typeof event.toolName !== "string" || !object(event.input)) return false;
    const tool = exactTool(tools, event.toolName);
    if (!tool) return false;
    if (hostTool(tool, policy)) return true;
    if (FILE_TOOLS.has(event.toolName) && nativeBuiltin(tool, event.toolName)) {
        const paths = filePaths(event.input, event.toolName);
        if (!paths) return false;
        return paths.every(path => {
            // Internal URL handlers can execute code (for example ssh://).
            // The xd listing and exact host tool documentation are metadata.
            if (event.toolName === "read" && path === "xd://") return true;
            if (event.toolName === "read" && /^xd:\/\/[a-z0-9_]+$/.test(path)) {
                const target = exactTool(tools, path.slice(5));
                return target !== undefined && hostTool(target, policy);
            }
            return !/[a-z][a-z0-9+.-]*:\/\//i.test(path) && !/^www\./i.test(path);
        });
    }
    if (event.toolName === "write" && nativeBuiltin(tool, "write") &&
        typeof event.input.path === "string" && /^xd:\/\/[a-z0-9_]+$/.test(event.input.path) &&
        typeof event.input.content === "string") {
        const target = exactTool(tools, event.input.path.slice(5));
        if (!target || !hostTool(target, policy)) return false;
        try {
            return object(JSON.parse(event.input.content));
        } catch {
            return false;
        }
    }
    return false;
}

export function installReviewGuard(pi: NativeApi, policy: ReviewPolicy): void {
    let activeSession: string | undefined;
    let retired = false;
    const current = (ctx: Context) => !retired && ctx.agent?.kind === "main" &&
        activeSession !== undefined && ctx.sessionManager.getSessionId() === activeSession;

    pi.on("tool_call", (event: ToolCall, ctx: Context) =>
        current(ctx) && permitsReviewCall(policy, pi.getAllTools(), event) ? undefined : DENIED);
    // The RPC launcher refuses command-shaped task text too. This native gate
    // covers later inputs, before built-in/custom slash-command dispatch.
    pi.on("input", (event: { text?: unknown }, ctx: Context) =>
        !current(ctx) || typeof event.text !== "string" || event.text.trimStart().startsWith("/")
            ? { handled: true } : undefined);
    for (const name of ["session_switch", "session_clear", "session_branch", "session_tree", "session_shutdown"]) {
        pi.on(name, () => { retired = true; activeSession = undefined; });
    }
    pi.on("session_start", async (_event: unknown, ctx: Context) => {
        if (retired || activeSession !== undefined || ctx.agent?.kind !== "main") return;
        const tools = pi.getAllTools();
        if (![...FILE_TOOLS].every(name => {
            const tool = exactTool(tools, name);
            return tool !== undefined && nativeBuiltin(tool, name);
        })) return;
        const enabled = new Set(pi.getActiveTools());
        const admitted = tools.filter(tool => typeof tool.name === "string" && enabled.has(tool.name) &&
            ((FILE_TOOLS.has(tool.name) && nativeBuiltin(tool, tool.name)) || hostTool(tool, policy) ||
                (policy.tools.length > 0 && nativeBuiltin(tool, "write")))).map(tool => tool.name as string);
        const session = ctx.sessionManager.getSessionId();
        if (!session) return;
        await pi.setActiveTools(admitted);
        if (retired || ctx.sessionManager.getSessionId() !== session) return;
        activeSession = session;
        pi.appendEntry("logonomic-read-only", { version: 1, scope: policy.scope, session_id: session });
    });
}

export default function reviewExtension(pi: NativeApi): void {
    installReviewGuard(pi, parseReviewPolicy(process.env.LOGONOMIC_OMP_REVIEW_POLICY ?? ""));
}
