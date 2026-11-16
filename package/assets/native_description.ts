import type { AgentToolResult, ExtensionAPI, ExtensionContext } from "@oh-my-pi/pi-coding-agent";

export const descriptionToolName = "logonomic_describe_session";
export type DescriptionBinding = { sessionId: string; nonce: string };
export type DescriptionPayload = { kind: "title" | "summary" | "summary_unchanged"; title?: string; summary?: string };
export type DescriptionReply = { committed: boolean; accepted: boolean; reason?: string;
    description?: { version: number; request_id: string; accepted: boolean; reason: string } };
type Details = { accepted: boolean; disposition: "accepted" | "refused" | "uncertain"; reason: string };
type Submit = (binding: DescriptionBinding, requestId: string, payload: DescriptionPayload,
    ctx: ExtensionContext, signal?: AbortSignal) => Promise<DescriptionReply | undefined>;

const text = (value: unknown, maximum: number, multiline = false): value is string =>
    typeof value === "string" && value.length > 0 && Buffer.byteLength(value) <= maximum &&
    Buffer.from(value).toString("utf8") === value &&
    !(multiline ? /[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]/u : /[\x00-\x1f\x7f]/u).test(value);
const reason = (value: unknown): value is string => typeof value === "string" &&
    /^[a-z][a-z0-9_]{0,63}$/u.test(value);

function result(disposition: Details["disposition"], code: string): AgentToolResult<Details> {
    const message = disposition === "accepted" ? "Logonomic recorded the session description." :
        disposition === "uncertain" ? "The host may have recorded this submission. Do not retry it automatically." :
        `Logonomic did not record this submission: ${code}.`;
    return { content: [{ type: "text", text: message }],
        details: { accepted: disposition === "accepted", disposition, reason: code } };
}

/** Public native tool execution supplies its own context, approval policy and abort signal. */
export function registerDescriptionTool(pi: ExtensionAPI,
    bindingFor: (ctx: ExtensionContext) => DescriptionBinding | undefined, submit: Submit): boolean {
    if (typeof pi.registerTool !== "function" || typeof pi.zod?.object !== "function") return false;
    const z = pi.zod;
    const receipts = new WeakMap<DescriptionBinding, Map<string, { signature: string; result: Promise<AgentToolResult<Details>> }>>();
    pi.registerTool({
        name: descriptionToolName,
        label: "Describe Logonomic session",
        description: "Record a Logonomic session title or concise Markdown working summary when host instructions request it. " +
            "Use summary_unchanged only after reconsidering an existing summary. This does not report task completion.",
        approval: "write",
        loadMode: "essential",
        parameters: z.object({
            kind: z.enum(["title", "summary", "summary_unchanged"]),
            title: z.string().max(1024).optional().describe("Title suggestion, or an optional revised title with a summary"),
            summary: z.string().max(16384).optional().describe("Concise Markdown working-state summary for kind summary"),
        }).strict(),
        async execute(requestId, payload, signal, _onUpdate, ctx) {
            const binding = bindingFor(ctx); // Capture before the first await.
            if (!binding || ctx.agent?.kind !== "main") return result("refused", "description_unavailable");
            if (signal?.aborted) return result("refused", "cancelled_before_submission");
            if (!text(requestId, 256) || !payload || typeof payload !== "object" ||
                Object.keys(payload).some(key => !["kind", "title", "summary"].includes(key)) ||
                (payload.title !== undefined && !text(payload.title, 1024)) ||
                (payload.summary !== undefined && !text(payload.summary, 16384, true)) ||
                (payload.kind === "title" ? payload.title === undefined || payload.summary !== undefined :
                    payload.kind === "summary" ? payload.summary === undefined :
                        payload.kind !== "summary_unchanged" || payload.summary !== undefined)) {
                return result("refused", "description_invalid");
            }
            const captured = { kind: payload.kind, ...(payload.title !== undefined ? { title: payload.title } : {}),
                ...(payload.summary !== undefined ? { summary: payload.summary } : {}) } as DescriptionPayload;
            const signature = JSON.stringify(captured);
            if (Buffer.byteLength(JSON.stringify({ provider_session_id: binding.sessionId,
                transition_nonce: binding.nonce, request_id: requestId, ...captured })) > 24 * 1024) {
                return result("refused", "description_invalid");
            }
            let retained = receipts.get(binding);
            if (!retained) { retained = new Map(); receipts.set(binding, retained); }
            const previous = retained.get(requestId);
            if (previous) return previous.signature === signature ? previous.result : result("refused", "request_conflict");
            // Never evict an unknown outcome and thereby create a fresh retry.
            if (retained.size >= 128) return result("refused", "description_limit");
            const work = (async () => {
                let reply: DescriptionReply | undefined;
                try { reply = await submit(binding, requestId, captured, ctx, signal); }
                catch { return result("uncertain", "transport_unavailable"); }
                if (bindingFor(ctx) !== binding) return result("uncertain", "stale_activation");
                if (!reply?.committed) return result("uncertain", "transport_unavailable");
                if (!reply.accepted) return result("refused", reason(reply.reason) ? reply.reason : "description_rejected");
                const receipt = reply.description;
                if (!receipt || receipt.version !== 1 || receipt.request_id !== requestId ||
                    typeof receipt.accepted !== "boolean" || typeof receipt.reason !== "string" ||
                    (receipt.reason === "" ? !receipt.accepted : !reason(receipt.reason))) {
                    return result("uncertain", "description_result_unavailable");
                }
                return result(receipt.accepted ? "accepted" : "refused", receipt.reason);
            })();
            retained.set(requestId, { signature, result: work });
            return work;
        },
    });
    return true;
}
