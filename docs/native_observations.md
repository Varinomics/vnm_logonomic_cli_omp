# Native observations

The extension captures optional observations together with the native session
identity, activation nonce, and source sequence before queueing helper delivery.
The classifier admits those facts only for the matching activation, or jointly
with an accepted native session transition. It emits an optional SDK envelope
only when the host requests that capability. Invalid optional data leaves a valid
adjacent lifecycle fact usable; refusal of a context-only delivery does not
quarantine root lifecycle observation.

Delivery commitment comes from the authenticated SDK response. The helper writes
that response before releasing the host's replay journal; cancellation during
that later acknowledgment does not revoke the already admitted event. The
extension still waits for helper exit and wrapper reaping. A shutdown deadline
reports loss for skipped queued lifecycle facts or an actually uncommitted send,
not merely for an in-flight post-commit journal release.

## Context occupancy and compactions

`context-state@2` samples `ctx.getContextUsage()` on native session activation,
agent start, assistant message end, terminal agent end, and successful compaction.
The native query reports the current model context, using provider anchors and
local estimates between them. Its `tokens` and `contextWindow` become
`used_tokens` and `context_window`; the host derives the bounded displayed
fraction. Cumulative usage or billing totals are never treated as occupancy.
An absent, invalid, or unavailable query explicitly clears the old measurement.
This cadence does not imply an update for each streamed token.

Only `session_compact` increments the observed successful-compaction lower bound.
The native emitter first appends the compaction entry, replaces live messages,
and rebases context statistics. Its stable entry ID identifies the observation
for SDK deduplication across redelivery. Compaction start, attempts, and
`auto_compaction_end` are not additional success evidence. The extension does not
claim that observed events cover a resumed conversation's earlier history, and
does not mix live successful events with an absolute history count. Compaction
summaries and conversation text are not included.

## Current execution directory

For hosts requesting `execution-context@1`, the adapter samples the exact
`ctx.cwd` at the same native cadence. Logonomic uses the conversation's
registered directory and leaves this capability unselected.
The extension runner reads the owning session manager's live directory; it does
not use the process-global directory, launch argument, or a tool's per-command
working directory. Native `/move` changes are therefore visible in the next
sample. There is no dedicated public directory-change event, so this is not a
promise of immediate observation while idle.

An absolute local path is preserved byte for byte, including nested directories
and spaces. A missing or invalid path produces an attributed `unknown` update.
The source nonce and monotonically increasing sequence prevent delayed facts
from an earlier activation, including a clear that preserves the native UUID,
from replacing the current directory. This capability neither changes directories
nor authorizes an interactive resume recipe.

## Active model, provider, and reasoning

`model-state@1` samples the root's native `ctx.model.provider` and `ctx.model.id`,
with `pi.getThinkingLevel()` when available, at the context sampling boundaries
above and native `retry_fallback_applied` / `retry_fallback_succeeded` events.
Missing model evidence produces `unknown`; omitted reasoning replaces an older
known level with unknown, while literal native `off` remains `off`. Requested
launch aliases and delegated selection arguments are not observations.

Native `model_changed` and `thinking_level_changed` notifications are not exposed
as extension hooks in this source revision. Where the native managed timer API
is available, the extension therefore samples the public live model/thinking
getters once per second while idle and sends only changed snapshots. The native
timer is unreferenced, tied to the root activation, and retired on clear, switch,
or shutdown. This lightweight observation never infers completion or starts a
turn. Older native bindings without managed timers retain event-bound samples.
Each SDK update names its observation boundary.

For admitted Task/eval children, the public progress snapshot supplies
`resolvedModelIdentity` and `resolvedThinkingLevel`. Native startup can seed
these fields before a session starts, so those fields alone never admit a child.
After startup, the executor updates identity from the child session's serving
model, which distinguishes an armed fallback from a model that actually served.
The extension splits only
the documented provider/identity boundary; it does not guess thinking from the
display string or use `modelOverride` as evidence. Unchanged samples are
deduplicated after accepted delivery, and failed delivery can recover on a later
native observation.

Child progress must match an admitted native child ID, its exact session file,
and its parent tool-call identity. Pending delivery also retains the captured
child object, so a queued old sample cannot overwrite a replacement that reused
the ID. Clearing or switching the root retires all these bindings. Child model
facts cannot create child lifecycle, overwrite root context, or complete the
root's turn. The host independently admits the matching active child and removes
its live model after departure.

## Structured serving model

In a structured or delegated task, hosts can negotiate
`structured-model-observation@1` and request the model on a specific RPC
inspection. The adapter reports the provider and model from the latest native
assistant `message_end` in those bytes. A fallback therefore reports the model
that served the message. The interactive model capability is independent.

## Available native models

After the host negotiates `model-catalog@1`, the extension samples the public
`ctx.models.list()` query for the running native profile. This is native
configured availability, separate from the currently serving model and from the
native model picker's enabled-model filter. The sampler waits for the native
registry's existing background refresh without holding a lifecycle callback or
starting a model request. It includes provider, model ID, and available display
name or reasoning-support metadata; it excludes authentication and pricing data.

Sampling occurs after an accepted activation, at agent and fallback boundaries,
and after fifteen managed timer ticks while idle. An unchanged snapshot is not
transported again. A pending refresh retains its original activation and cannot
publish into a replacement session or a cleared activation. Only one refresh is
pending per activation, and catalog deliveries use the bounded optional queue.
Hosts that do not negotiate the capability receive no catalog samples.

The projection sorts stable provider/model identities and bounds the payload to
128 rows and 24 KiB before lifecycle framing. `total_count` records the native
catalog size and `truncated` identifies a partial projection. An unavailable
public query produces unavailable state. This sampled catalog does not promise
that every listed model will accept a subsequent request.

## Native child details and nested Task observations

`child-state@1` adds native state, description, current tool, progress fraction,
and structural parent/depth to independently admitted children. Optional fields
remain absent when their native source does not supply them. Native completed,
failed, and aborted states become succeeded, failed, and interrupted; an aborted
settled result takes precedence over its nominal exit code. These are child
outcomes and never become the root turn result. The host retains bounded completed
child history separately from live child facts.

Direct Task/eval admission still requires the public
`task:subagent:lifecycle` started event. Progress retains the exact child session
file and parent tool-call binding. Native pending entries, a seeded model, and
pre-session failures are not starts. Direct descriptions come from the lifecycle
or progress payload; no tool arguments or output text is copied into telemetry.

After the host negotiates child-state, public direct-child progress can also
carry nested `TaskToolDetails` through `inflightTaskDetails` and
`extractedToolData.task`. The extension recursively projects those structured
snapshots. A nested row must show a positive native request or executed-tool
count before it can establish execution. A first observed settled execution
carries distinct start and stop facts in one classification; a reserved or
preflight-failed row with no execution evidence remains unadmitted. Parent and
depth come from the native structure, never from parsing child ID strings.
Snapshots do not imply that a missing descendant stopped, and a parent's stop
does not complete its descendants.

Observation is bounded to depth eight and 128 retained child identities per root
activation. Cycles, conflicting identity bindings, capacity limits, or failure
to deliver a native child lifecycle batch produce attributed child-only loss.
Loss withdraws child liveness without ending the root or claiming child success.
If its separate delivery fails, the next genuine root event can carry the loss;
shutdown can carry loss from a canceled child delivery. Source sequences and
captured child objects prevent old queued snapshots from acquiring a replacement
child's authority. Older hosts without child-state retain the original direct
child observation route and receive no expanded nested stream.

This is structured Task snapshot coverage, not a universal native agent tree.
It does not promise `/tan` or advisor coverage, every descendant before its first
execution evidence, independent child context occupancy, or child messaging and
control. No private whole-tree event bus or transcript parsing is used.

The contract fixtures cover direct and nested state projection, source binding,
loss, and SDK admission. In the current native acceptance environment, Task
creation reaches the public bus but fails its temporary child-session file lock
with `EPERM` before a started event. Root `--no-session` runs pass. Successful
end-to-end native child execution remains unqualified until that native platform
boundary is exercised in a supported environment; fixture coverage is not
reported as that acceptance result.

## Source evidence

The behavior above is traced to upstream oh-my-pi commit
`7a52f1e35eacc115b067c03a9c8ec7263e114cda`:

- `packages/coding-agent/src/extensibility/extensions/types.ts`:
  `ExtensionContext.getContextUsage` and `cwd`, public lifecycle hooks.
- `packages/coding-agent/src/session/session-stats.ts`:
  `getContextBreakdown` and `getContextUsage`, current occupancy and provider
  anchors rather than accumulated billing.
- `packages/coding-agent/src/session/session-maintenance.ts`:
  `#commitCompactionEntry` and snapcompact rescue commit, both emitting
  `session_compact` after installation.
- `packages/coding-agent/src/extensibility/extensions/runner.ts`:
  `cwd` getter and `createContext`, reading `sessionManager.getCwd()`.
- `packages/coding-agent/src/session/agent-session.ts`:
  model-change fan-out explicitly has no extension-facing hook.
- `packages/coding-agent/src/task/executor.ts`: `publishServingModel`,
  `emitProgressNow`, and native child lifecycle source identity.
- `packages/tui/src/tools/task.ts`: `AgentProgress.resolvedModelIdentity` and
  `resolvedThinkingLevel`; `resolvedModel` is a display value, not the identity.

The source revision records evidence; it is not an arbitrary minimum version.
The contract fixtures test source attribution and SDK admission. Actual native
acceptance is reported separately from fixture coverage.
