import { spawn } from "node:child_process";
import { randomBytes } from "node:crypto";
import { isAbsolute, join } from "node:path";
import type { ExtensionAPI, ExtensionContext } from "@oh-my-pi/pi-coding-agent";
import { nestedTaskSnapshots } from "./native_children.ts";
import { sampleNativeCatalog } from "./native_model_catalog.ts";
import { descriptionToolName, registerDescriptionTool } from "./native_description.ts";
import type { DescriptionReply } from "./native_description.ts";
import { nativeWakeState, startWakeBridge } from "./native_wake.ts";
import type { WakeBridge } from "./native_wake.ts";

type Fact = string | boolean | number | Fact[] | { [key: string]: Fact };
type Facts = Record<string, Fact>;
type Request = { operation: "event" | "context" | "description"; name: string; body: Facts };
type Reply = DescriptionReply & { instruction?: string; child_state_version?: number;
    model_catalog_version?: number; cooperative_description_version?: number };
type Transport = (request: Request, signal: AbortSignal) => Promise<Reply | undefined>;
type Activation = {
    sessionId: string;
    nonce: string;
    turnId: string;
    sequence: number;
    lost: boolean;
    lossReported: boolean;
    pendingContext: string;
    modelSnapshot?: string;
    pendingRootModel?: string;
    childState: boolean;
    childLoss?: string;
    childLossReported: boolean;
    interrupted: boolean;
    catalogEnabled: boolean;
    catalogPending: boolean;
    catalogSnapshot?: string;
    descriptionEnabled: boolean;
};
type Child = { owner: Activation; sessionFile: unknown; parentToolCallId: unknown; active: boolean;
    modelSnapshot?: string; pendingModel?: string };
type NestedChild = { anchor: Child; parentId: string; depth: number; state: string; signature: string };

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
        let completed = false;
        // Closing this pipe asks the helper to cancel and reap its wrapper.
        // Killing only the helper would strand the authenticated wrapper.
        const cancel = () => { child.stdin.end(); };
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
            if (code !== 0) { resolve(undefined); return; }
            try {
                // The helper writes its committed response before releasing
                // the host journal. Canceling that later release cannot revoke
                // admission already proved by this response. We still await
                // helper exit so its SDK-owned wrapper has been reaped.
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
    let queuedLifecycle = 0;
    let queuedOptional = 0;
    let sampleRevision = 0;
    const latestSamples = new Map<string, number>();
    let shuttingDown = false;
    let shutdownDrainExpired = false;
    let running: AbortController | undefined;
    let contextRequest: AbortController | undefined;
    let descriptionRequest: AbortController | undefined;
    let queuedDescriptions = 0;
    let stopModelObserver: (() => void) | undefined;
    let wakeBridge: WakeBridge | undefined;
    let wakeOwner: Activation | undefined;
    let wakeContext: ExtensionContext | undefined;
    const children = new Map<string, Child>();
    const nestedChildren = new Map<string, NestedChild>();
    const attention = new Map<string, Activation>();

    const isRoot = (ctx: ExtensionContext) => ctx.agent?.kind === "main";
    const current = (ctx: ExtensionContext) => isRoot(ctx) &&
        activation?.sessionId === ctx.sessionManager.getSessionId() ? activation : undefined;
    const facts = (owner: Activation, extra: Facts = {}): Facts => ({
        session_id: owner.sessionId, activation_nonce: owner.nonce,
        event_id: freshId(), sequence: String(++owner.sequence),
        ...(owner.childLoss && !owner.childLossReported ? { child_loss: { reason_code: owner.childLoss,
            source: "omp:native_child_observer" } } : {}),
        ...(owner.turnId ? { turn_id: owner.turnId } : {}), ...extra,
    });

    const send = (owner: Activation, name: string, extra: Facts = {}, lifecycle = true,
        sourceIsCurrent: () => boolean = () => true): Promise<boolean> => {
        if (!lifecycle && owner.lost) return Promise.resolve(false);
        const body = facts(owner, extra); // Capture before any asynchronous work.
        // Replaceable samples have their own bounded budget. Dropping or
        // coalescing them cannot consume root lifecycle custody or claim loss.
        // A successful compaction is a distinct event, never coalesced.
        const replaceable = ["message_end", "model_observation", "retry_fallback_applied",
            "retry_fallback_succeeded", "task:subagent:progress", "model_catalog"].includes(name);
        const sampleKey = !lifecycle && replaceable ? `${name}:${extra.agent_id ?? ""}` : "";
        const revision = ++sampleRevision;
        if (sampleKey) latestSamples.set(sampleKey, revision);
        if (!lifecycle && queuedOptional >= 16 && name !== "task:subagent:loss") return Promise.resolve(false);
        if (lifecycle) {
            if (++queuedLifecycle > 64) owner.lost = true;
        } else ++queuedOptional;
        const work = pending.then(async () => {
            if (lifecycle) --queuedLifecycle;
            else --queuedOptional;
            if (owner !== activation || owner.lossReported || !sourceIsCurrent() ||
                (sampleKey && latestSamples.get(sampleKey) !== revision) ||
                (!lifecycle && owner.lost) || (shutdownDrainExpired && name !== "session_shutdown")) return false;
            const controller = new AbortController();
            running = controller;
            const sentLoss = owner.lost;
            try {
                const reply = await transport({ operation: "event",
                    name: sentLoss ? "logonomic_observation_lost" : name, body }, controller.signal);
                if (!reply?.committed || !reply.accepted) {
                    if (lifecycle) owner.lost = true;
                } else if (sentLoss) owner.lossReported = true;
                else {
                    if (extra.model && !extra.agent_id) owner.modelSnapshot = JSON.stringify(extra.model);
                    if (reply.child_state_version === 1) owner.childState = true;
                    if (reply.model_catalog_version === 1) owner.catalogEnabled = true;
                    if (reply.cooperative_description_version === 1) owner.descriptionEnabled = true;
                    if (body.child_loss) owner.childLossReported = true;
                }
                return reply?.committed === true && reply.accepted === true;
            } catch { if (lifecycle) owner.lost = true; return false; }
            finally { if (running === controller) running = undefined; }
        });
        pending = work.then(() => {}, () => {});
        return work;
    };

    const descriptionInstalled = registerDescriptionTool(pi, ctx => {
        const owner = current(ctx);
        return owner?.descriptionEnabled && !owner.lost && !shuttingDown ? owner : undefined;
    }, (binding, requestId, payload, ctx, signal) => {
        if (queuedDescriptions >= 4) return Promise.resolve({ committed: true, accepted: false, reason: "description_busy" });
        ++queuedDescriptions;
        const work = pending.then(async (): Promise<Reply | undefined> => {
            --queuedDescriptions;
            const owner = current(ctx);
            if (!owner || binding !== owner || owner.lost || !owner.descriptionEnabled || shuttingDown || signal?.aborted) {
                return { committed: true, accepted: false, reason: "submission_cancelled" };
            }
            const controller = new AbortController();
            const cancel = () => controller.abort();
            signal?.addEventListener("abort", cancel, { once: true });
            descriptionRequest = controller;
            running = controller;
            try {
                return await transport({ operation: "description", name: "model_description", body: {
                    session_id: binding.sessionId, activation_nonce: binding.nonce, request_id: requestId,
                    description: { kind: payload.kind, ...(payload.title !== undefined ? { title: payload.title } : {}),
                        ...(payload.summary !== undefined ? { summary: payload.summary } : {}) },
                } }, controller.signal);
            } finally {
                signal?.removeEventListener("abort", cancel);
                if (descriptionRequest === controller) descriptionRequest = undefined;
                if (running === controller) running = undefined;
            }
        });
        pending = work.then(() => {}, () => {});
        return work;
    });

    const modelFacts = (provider: unknown, model: unknown, effort: unknown): Facts =>
        identity(provider, 512) && identity(model, 512) ? {
            state: "confirmed", provider, model,
            ...(identity(effort, 128) ? { reasoning_effort: effort } : {}),
        } : { state: "unknown", provider: "", model: "" };

    const childModelFacts = (nativeModel: unknown, effort: unknown): Facts => {
        const separator = typeof nativeModel === "string" ? nativeModel.indexOf("/") : -1;
        return separator > 0 && typeof nativeModel === "string" ?
            modelFacts(nativeModel.slice(0, separator), nativeModel.slice(separator + 1), effort) :
            modelFacts(undefined, undefined, undefined);
    };

    const childFacts = (state: string, parent: string, depth: number, description?: unknown,
        tool?: unknown, percent?: unknown): Facts => ({
        state, parent_agent_id: parent, depth,
        ...(identity(description, 512) ? { description } : {}),
        ...(identity(tool, 128) ? { current_tool: tool } : {}),
        ...(typeof percent === "number" && Number.isFinite(percent) && percent >= 0 && percent <= 100 ?
            { progress: percent / 100 } : {}),
    });

    const childState = (status: unknown): string => status === "started" || status === "running" ? "running" :
        status === "completed" ? "succeeded" : status === "failed" ? "failed" : status === "aborted" ? "interrupted" : "unknown";

    const loseChildren = (owner: Activation, reason: string): void => {
        if (owner !== activation || !owner.childState || owner.childLoss) return;
        owner.childLoss = reason;
        children.clear();
        nestedChildren.clear();
        // This capability-owned loss can also ride the next root event if
        // transport fails. It never becomes whole-root observation loss.
        if (!shuttingDown) void send(owner, "task:subagent:loss", {}, false);
    };

    const observeNested = (id: string, anchor: Child, progress: unknown): void => {
        const owner = anchor.owner;
        if (!owner.childState || owner.childLoss) return;
        const observed = nestedTaskSnapshots(id, progress);
        if (observed.loss) { loseChildren(owner, observed.loss); return; }
        const rows: Facts[] = [];
        for (const item of observed.snapshots) {
            const previous = nestedChildren.get(item.id);
            if (children.has(item.id) || (previous && (previous.anchor !== anchor ||
                previous.parentId !== item.parentId || previous.depth !== item.depth))) {
                loseChildren(owner, "conflicting_nested_identity"); return;
            }
            const state = item.state;
            // Settled snapshots remain in native tool details; replay cannot
            // reopen their old child or roll it back to an earlier phase.
            if (previous && previous.state !== "running") continue;
            const extra: Facts = { agent_id: item.id,
                ...(identity(item.agent, 160) ? { agent_label: item.agent } : {}),
                child: childFacts(state, item.parentId, item.depth, item.description,
                    item.currentTool, item.completionPercent),
                model: childModelFacts(item.modelIdentity, item.thinkingLevel) };
            const signature = JSON.stringify(extra);
            if (previous?.signature === signature) continue;
            if (!previous && children.size + nestedChildren.size >= 128) {
                loseChildren(owner, "child_observation_limit"); return;
            }
            const transition = previous ? (state === "running" ? "none" : "stop") :
                (state === "running" ? "start" : "start_stop");
            nestedChildren.set(item.id, { anchor, parentId: item.parentId, depth: item.depth, state, signature });
            const startSequence = transition === "start_stop" ? String(++owner.sequence) : undefined;
            rows.push({ ...extra, transition, execution_observed: true,
                ...(startSequence ? { start_sequence: startSequence } : {}), sequence: String(++owner.sequence) });
        }
        // At most eight rows / sixteen start-stop facts fit the ordinary
        // helper-ingress@2 contract. No new helper version is required.
        for (let start = 0; start < rows.length; start += 8) {
            void send(owner, "task:subagent:snapshot", { children: rows.slice(start, start + 8) }, false,
                () => !owner.childLoss && children.get(id) === anchor).then(accepted => {
                    if (!accepted && owner === activation && children.get(id) === anchor)
                        loseChildren(owner, "child_delivery_lost");
                });
        }
    };

    const rootModelFacts = (ctx: ExtensionContext): Facts => {
        try {
            let effort: unknown;
            try { effort = pi.getThinkingLevel?.(); } catch { /* Unknown reasoning does not erase a known model. */ }
            return modelFacts(ctx.model?.provider, ctx.model?.id, effort);
        } catch { return modelFacts(undefined, undefined, undefined); }
    };

    const observeCatalog = (owner: Activation, ctx: ExtensionContext): void => {
        if (!owner.catalogEnabled || owner.catalogPending || owner !== current(ctx) || owner.lost || shuttingDown) return;
        owner.catalogPending = true;
        // Public background refresh can outlive this activation. It does not
        // hold a native lifecycle callback or acquire a replacement's identity.
        void sampleNativeCatalog(ctx).then(async catalog => {
            if (owner !== current(ctx) || owner.lost || shuttingDown) return;
            const signature = JSON.stringify(catalog);
            if (signature === owner.catalogSnapshot) return;
            if (await send(owner, "model_catalog", { catalog }, false, () => owner === current(ctx)))
                owner.catalogSnapshot = signature;
        }).catch(() => { /* A retired or unavailable optional query cannot fail native lifecycle work. */ })
            .finally(() => { owner.catalogPending = false; });
    };

    const snapshot = (ctx: ExtensionContext): Facts => {
        let context: Facts = { usage_unavailable: true };
        try {
            const usage = ctx.getContextUsage();
            if (usage && Number.isSafeInteger(usage.tokens) && usage.tokens >= 0 &&
                Number.isSafeInteger(usage.contextWindow) && usage.contextWindow > 0) {
                // This native query is current occupancy, including native
                // estimates between provider anchors; billing totals are not.
                context = { used_tokens: usage.tokens, context_window: usage.contextWindow };
            }
        } catch { /* An unavailable optional query never blocks native work. */ }
        const cwd = ctx.cwd;
        return { context, model: rootModelFacts(ctx), execution: identity(cwd, 4096) && isAbsolute(cwd) ?
            { state: "confirmed", cwd } : { state: "unknown", cwd: "" } };
    };

    const observeIdleModel = (owner: Activation, ctx: ExtensionContext): void => {
        if (typeof ctx.setInterval !== "function" || typeof ctx.clearTimer !== "function") return;
        // OMP owns this unref'd timer and also retires it on shutdown. The
        // getter is live for the bound session; this samples no history and
        // never turns isIdle() into completion evidence.
        let catalogTicks = 0;
        const timer = ctx.setInterval(() => {
            if (shuttingDown || owner !== current(ctx) || owner.lost || !ctx.isIdle()) return;
            if (++catalogTicks >= 15) { catalogTicks = 0; observeCatalog(owner, ctx); }
            const model = rootModelFacts(ctx);
            const signature = JSON.stringify(model);
            if (signature === (owner.pendingRootModel ?? owner.modelSnapshot)) return;
            owner.pendingRootModel = signature;
            void send(owner, "model_observation", { model }, false).finally(() => {
                if (owner.pendingRootModel === signature) owner.pendingRootModel = undefined;
            });
        }, 1000);
        stopModelObserver = () => ctx.clearTimer(timer);
    };

    const activate = async (name: string, source: string, ctx: ExtensionContext): Promise<void> => {
        if (!isRoot(ctx) || shuttingDown) return;
        const sessionId = ctx.sessionManager.getSessionId();
        if (!identity(sessionId)) return;
        const previous = activation?.nonce;
        descriptionRequest?.abort();
        stopModelObserver?.();
        stopModelObserver = undefined;
        const owner: Activation = { sessionId, nonce: freshId(), turnId: "", sequence: 0,
            lost: false, lossReported: false, pendingContext: "", childState: false, childLossReported: false,
            interrupted: false, catalogEnabled: false, catalogPending: false, descriptionEnabled: false };
        activation = owner;
        wakeOwner = undefined;
        wakeContext = undefined;
        latestSamples.clear();
        children.clear();
        nestedChildren.clear();
        attention.clear();
        const sessionFile = ctx.sessionManager.getSessionFile();
        await send(owner, name, { ...snapshot(ctx), source, ...(previous ? { previous_nonce: previous } : {}),
            ...(identity(sessionFile, 8192) && isAbsolute(sessionFile) ? { session_file: sessionFile } : {}) });
        if (owner === activation && !owner.lost && !shuttingDown) {
            observeCatalog(owner, ctx);
            observeIdleModel(owner, ctx);
            wakeOwner = owner;
            wakeContext = ctx;
            if (!wakeBridge) wakeBridge = await startWakeBridge(pi, () => {
                if (shuttingDown || !wakeOwner || wakeOwner !== activation || !wakeContext) return undefined;
                return nativeWakeState(wakeContext, { ...wakeOwner,
                    attention: [...attention.values()].some(item => item === wakeOwner) });
            });
            if (shuttingDown) { wakeBridge?.close(); wakeBridge = undefined; }
        }
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
            contextRequest = controller;
            try {
                const reply = await transport({ operation: "context", name: "before_agent_start",
                    body: { session_id: owner.sessionId, activation_nonce: owner.nonce } }, controller.signal);
                if (owner === activation && reply?.committed && reply.accepted && reply.instruction) {
                    owner.pendingContext = reply.instruction;
                }
            } catch { /* A context refusal never vetoes the native prompt. */ }
            finally { if (contextRequest === controller) contextRequest = undefined; }
        }
        if (owner === activation && !shuttingDown && owner.pendingContext) {
            // Repeated native preparation attempts retain accepted context
            // until agent_start proves the prepared prompt entered the loop.
            const descriptionRoute = descriptionInstalled && owner.descriptionEnabled &&
                pi.getActiveTools?.().includes(descriptionToolName)
                ? `\nFor the Logonomic title and working-summary requests above, use ${descriptionToolName}; its result reports the host's disposition.`
                : "";
            return { message: { customType: "logonomic", content: owner.pendingContext + descriptionRoute, display: false } };
        }
    });

    pi.on("agent_start", (_event, ctx) => {
        const owner = current(ctx);
        if (!owner) return;
        owner.pendingContext = "";
        owner.interrupted = false;
        // Automatic continuations remain one busy root turn.
        if (!owner.turnId) owner.turnId = freshId();
        // Capture and queue observations synchronously. Holding the native
        // extension delivery turn on helper I/O can hide its fire-and-forget
        // agent_end behind a concurrent session_shutdown callback.
        void send(owner, "agent_start", snapshot(ctx));
        observeCatalog(owner, ctx);
    });
    pi.on("agent_end", (event, ctx) => {
        const owner = current(ctx);
        if (!owner?.turnId) return;
        if (!Object.hasOwn(event, "willContinue")) {
            // The verified native emitter includes this field even when its
            // value is undefined. Missing semantic evidence is not success.
            owner.lost = true;
            void send(owner, "logonomic_observation_lost");
            return;
        }
        if (event.willContinue === true ||
            (event.willContinue !== undefined && event.willContinue !== false)) return;
        // The native contract treats undefined/false willContinue as terminal.
        // ctx.isIdle() can still be false while the owning prompt unwinds;
        // this notification is not repeated when that counter reaches zero.
        // session_stop is a veto gate and is deliberately never observed here.
        const last = [...event.messages].reverse().find(message => message.role === "assistant");
        const reason = last && "stopReason" in last && typeof last.stopReason === "string" ? last.stopReason : "";
        owner.interrupted = reason === "aborted";
        void send(owner, "agent_end", { ...snapshot(ctx), terminal: true, stop_reason: reason });
        owner.turnId = "";
        observeCatalog(owner, ctx);
    });

    pi.on("message_end", (event, ctx) => {
        const owner = current(ctx);
        if (owner && event.message.role === "assistant")
            void send(owner, "message_end", snapshot(ctx), false);
    });
    pi.on("session_compact", (event, ctx) => {
        const owner = current(ctx);
        if (!owner) return;
        const extra = snapshot(ctx);
        // Native emits this only after appending the entry and installing its
        // rebuilt live context. The durable entry id deduplicates redelivery;
        // a start/attempt/auto_compaction_end is not a second success signal.
        (extra.context as Facts).compaction_id = identity(event.compactionEntry?.id, 240) ?
            event.compactionEntry.id : "";
        void send(owner, "session_compact", extra, false);
    });
    for (const name of ["retry_fallback_applied", "retry_fallback_succeeded"] as const) {
        pi.on(name, (_event, ctx) => {
            const owner = current(ctx);
            if (owner) {
                void send(owner, name, snapshot(ctx), false);
                observeCatalog(owner, ctx);
            }
        });
    }

    const requestAttention = (name: string, id: string, ctx: ExtensionContext) => {
        const owner = current(ctx);
        if (!owner || !identity(id)) return;
        attention.set(id, owner);
        void send(owner, name, { request_id: id });
    };
    const resolveAttention = (name: string, id: string, ctx: ExtensionContext) => {
        const owner = attention.get(id);
        attention.delete(id);
        if (owner && owner === current(ctx)) void send(owner, name, { request_id: id });
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

    // Public lifecycle reports direct Task/eval children. Nested Task snapshots
    // are separately projected only after positive native execution evidence;
    // a parent stopping or textual mention never implies descendant completion.
    const unsubscribeLifecycle = pi.events.on("task:subagent:lifecycle", (value: unknown) => {
        if (typeof value !== "object" || value === null) return;
        const event = value as { id?: unknown; agent?: unknown; status?: unknown; sessionFile?: unknown;
            parentToolCallId?: unknown; description?: unknown };
        if (!identity(event.id) || typeof event.status !== "string") return;
        if (event.status === "started") {
            if (!activation || activation.childLoss || shuttingDown) return;
            const previous = children.get(event.id);
            if (activation.childState && previous &&
                (previous.sessionFile !== event.sessionFile || previous.parentToolCallId !== event.parentToolCallId) &&
                [...nestedChildren.values()].some(nested => nested.anchor === previous)) {
                loseChildren(activation, "replaced_child_binding"); return;
            }
            if (activation.childState && !children.has(event.id) && children.size + nestedChildren.size >= 128) {
                loseChildren(activation, "child_observation_limit"); return;
            }
            children.set(event.id, { owner: activation, sessionFile: event.sessionFile,
                parentToolCallId: event.parentToolCallId, active: true });
        }
        const child = children.get(event.id);
        if (!child || child.sessionFile !== event.sessionFile || child.parentToolCallId !== event.parentToolCallId ||
            !["started", "completed", "failed", "aborted"].includes(event.status)) return;
        if (event.status !== "started") {
            child.active = false;
            if (!child.owner.childState) children.delete(event.id);
        }
        void send(child.owner, "task:subagent:lifecycle", { agent_id: event.id, status: event.status,
            ...(identity(event.agent, 160) ? { agent_label: event.agent } : {}),
            ...(child.owner.childState ? { child: childFacts(childState(event.status), "", 1, event.description) } : {}) },
            !child.owner.childState).then(accepted => {
                if (!accepted && children.get(event.id as string) === child) loseChildren(child.owner, "child_delivery_lost");
            });
    });
    const unsubscribeProgress = pi.events.on("task:subagent:progress", (value: unknown) => {
        if (typeof value !== "object" || value === null) return;
        const event = value as { progress?: { id?: unknown; resolvedModelIdentity?: unknown;
            resolvedThinkingLevel?: unknown; status?: unknown; description?: unknown;
            currentTool?: unknown; completionPercent?: unknown }; sessionFile?: unknown; parentToolCallId?: unknown };
        const progress = event.progress;
        if (!progress || !identity(progress.id)) return;
        const child = children.get(progress.id);
        if (!child || !child.active || child.owner !== activation || child.sessionFile !== event.sessionFile ||
            child.parentToolCallId !== event.parentToolCallId || shuttingDown) return;
        observeNested(progress.id, child, progress);
        if (child.owner.childLoss) return;
        const extra: Facts = { agent_id: progress.id,
            model: childModelFacts(progress.resolvedModelIdentity, progress.resolvedThinkingLevel),
            ...(child.owner.childState ? { child: childFacts(childState(progress.status), "", 1,
                progress.description, progress.currentTool, progress.completionPercent) } : {}) };
        const signature = JSON.stringify(extra);
        if (signature === (child.pendingModel ?? child.modelSnapshot)) return;
        child.pendingModel = signature;
        const id = progress.id;
        // A captured old child's queued sample cannot acquire authority over a
        // replacement which reused its display id before helper dispatch.
        void send(child.owner, "task:subagent:progress", extra, false,
            () => children.get(id) === child && child.active).then(accepted => {
                if (accepted) child.modelSnapshot = signature;
                if (child.pendingModel === signature) child.pendingModel = undefined;
            });
    });

    pi.on("session_shutdown", async (_event, ctx) => {
        if (!isRoot(ctx)) return;
        const owner = current(ctx);
        shuttingDown = true;
        wakeOwner = undefined;
        wakeContext = undefined;
        wakeBridge?.close();
        wakeBridge = undefined;
        stopModelObserver?.();
        stopModelObserver = undefined;
        unsubscribeLifecycle();
        unsubscribeProgress();
        contextRequest?.abort();
        descriptionRequest?.abort();
        // Native shutdown awaits handlers for at most 2000 ms. Give already
        // captured facts one helper-sized drain window, then reserve time for
        // wrapper retirement and the final shutdown/degradation delivery.
        // This bounds teardown; it never supplies terminal-event evidence.
        const deadline = setTimeout(() => {
            shutdownDrainExpired = true;
            // Queued lifecycle facts will be skipped after expiry. A running
            // send can already have committed; its actual response decides loss.
            if (owner && queuedLifecycle > 0) owner.lost = true;
            running?.abort();
        }, 750);
        try {
            for (;;) {
                const tail = pending;
                await tail;
                if (pending === tail) break;
            }
        } finally { clearTimeout(deadline); }
        if (owner) await send(owner, "session_shutdown");
        activation = undefined;
        latestSamples.clear();
        children.clear();
        nestedChildren.clear();
        attention.clear();
    });
}

export default function logonomicExtension(pi: ExtensionAPI): void {
    if (process.env.LOGONOMIC_PROVIDER_HELPERS_DIR) installExtension(pi);
}
