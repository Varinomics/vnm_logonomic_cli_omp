# Native oh-my-pi contract

This package uses public oh-my-pi extension events for interactive terminals
and native newline-delimited RPC for structured and delegated sessions. The
inspected source is oh-my-pi **18.20.2**, commit
[`7a52f1e35eacc115b067c03a9c8ec7263e114cda`](https://github.com/can1357/oh-my-pi/tree/7a52f1e35eacc115b067c03a9c8ec7263e114cda).
The provenance record lists the owned source contracts. Source inspection,
scripted protocol fixtures, and execution of the native CLI are separate
forms of evidence; tests must not describe a fixture as a live model run.

## Interactive launch

The package recognizes literal `omp` invocations through the SDK's launcher
grammar. Preparation verifies the host's complete frozen invocation, resolves
that same executable from the frozen effective environment, and preserves its
supported native options. The inspected version is provenance, not a minimum
or maximum cutoff. Runtime identity and event evidence determine which
observations can be produced. An unsupported invocation shape leaves the
original launcher unchanged and disables the package's interactive lifecycle
and context promises for that launch.

Preparation preserves supported native options and appends
`--extension {{asset:assets/logonomic.ts}}`. When the caller already uses
`--trusted-extension`, the added module uses that same native allowlist form.
String option values are parsed before recognizing flags: a system prompt
whose literal content is `--trusted-extension` does not change trust policy.
The package does not write a global oh-my-pi configuration file. The host
materializes the declared asset and supplies `LOGONOMIC_PROVIDER_HELPERS_DIR`;
the extension resolves the packaged helper from that inherited directory.

Supported option shapes include model/provider/thinking, explicit approval
policy, profile, working directory, session selectors, extension modules,
tool/skill selection, system prompts, cost/time limits, and the supported
boolean controls enumerated in `interactive_flags`. Options are retained
byte for byte. Noninteractive modes, CLI subcommands, positional startup
prompts, unknown extension flags, incomplete arguments, conflicting extension
allowlist forms, and Windows script wrappers remain unchanged ordinary
launches. Windows native `omp.exe` and POSIX executables/shebang launchers can
be prepared. A shell launcher is flattened only when the SDK proves one
literal invocation and its assignment values are present in the frozen
environment; compound scripts are never rewritten.

The native source contracts are
[`cli/args.ts`](https://github.com/can1357/oh-my-pi/blob/7a52f1e35eacc115b067c03a9c8ec7263e114cda/packages/coding-agent/src/cli/args.ts),
[`cli/flag-tables.ts`](https://github.com/can1357/oh-my-pi/blob/7a52f1e35eacc115b067c03a9c8ec7263e114cda/packages/coding-agent/src/cli/flag-tables.ts),
and the extension-loading path in
[`main.ts`](https://github.com/can1357/oh-my-pi/blob/7a52f1e35eacc115b067c03a9c8ec7263e114cda/packages/coding-agent/src/main.ts).

### Host initial prompt

`interactive-initial-prompt@1` adds a host-authored first user message to a fresh
supported invocation. The existing launcher must contain only understood
interactive options, with no positional prompt, `@file`, resume/session/restore,
continue, fork or goal selector. Preparation appends the package extension,
then native `--`, then the complete requested text as one argument. Native
`parseArgs` treats that final argument literally, and the interactive startup
loop in `main.ts` submits each positional independently to `session.prompt`.
The result acknowledges SHA-256 of the exact requested bytes and is checked
against the SDK's full replacement-argument bounds. An uncarriable request
returns `initial_prompt_unavailable` instead of an unchanged launch.

Native `AgentSession` expands leading slash commands/templates and `^` model
mentions before creating a user message. Leading slash text and any caret are
therefore unavailable for this exact-text capability; unclassified caret forms
are conservatively refused. Flag- and attachment-shaped text after `--`, quotes,
Unicode and edge spaces remain literal. The referenced brief is not read or
modified by preparation. The native `read` tool accepts absolute paths, subject
to the user's existing tool/approval choices; this acknowledgment does not
promise that the model will read the brief.

These boundaries follow `cli/initial-message.ts`, `main.ts`,
`AgentSession.#dispatchPrompt` and `session/model-mentions.ts` in the inspected
revision. Existing user configuration and extensions retain native behavior;
the package does not replace that policy to manufacture a fresh-message promise.

## Interactive observations

| Native evidence | Package observation | Scope and qualification |
|---|---|---|
| `session_start` | Session started, source `load` | Initial load can already be a native resumed conversation. The package reports its actual loaded id. |
| `session_switch` | New activation with the native reason | `new`, `resume`, and `fork` are emitted after the switch succeeds. |
| `session_clear` | New activation, source `clear` | Native `/clear` keeps the session UUID and file. A new nonce separates its live context from the previous activation. |
| `session_branch`, `session_tree` | New activation | Context rewinds cannot reuse the previous activation's model context or child state. |
| `session_shutdown` | Session ended | Only for the root binding; never claims successful work. |
| `agent_start` | Confirmed working phase | Creates a provider turn id. Automatic continuation retains that id. |
| Terminal `agent_end` | Confirmed awaiting-input phase | Requires the native emitter's own `willContinue` field with value undefined/false. The callback can precede prompt unwinding. |
| Terminal assistant `stopReason: stop` | Confirmed success | `error` means failed; `aborted` means interrupted. Length/tool-use/unknown endings do not establish successful work. |
| `session_stop` | No observation | This is an awaited veto/continuation gate. Returning from the gate is not completion. |
| `tool_approval_requested` and `tool_approval_resolved` | Paired confirmed permission attention | Exact native session id and tool-call id. A rejected approval still resolves the wait. |
| `plan_review_requested` and `plan_review_resolved` | Paired confirmed interactive-question attention | Exact review id, including dismissal without a selected choice. |
| UI `ask` tool execution start/end | Paired inferred question attention | Execution brackets the built-in tool; it does not prove that a dialog was visible. |
| Public `task:subagent:lifecycle` | Direct child start/stop | Exact native registry id and child session file; a child stop never completes the root. |

Extension factories are rebound to subagent sessions. `ctx.agent.kind`, not
depth, separates a main session from a child: native `/tan` children can have
depth zero. Child factories cannot create host root activations. Direct child
observations come from the root's public session EventBus; started and stopped
frames retain the activation captured at start and the native child file.
Events from an old activation are discarded after clear/switch/branch/tree.

The public EventBus exposes immediate task-tool and eval `agent()` children.
The private whole-tree observability bus is a separate object. The add-on
does **not** claim complete nested descendant, `/tan`, or advisor coverage,
and does not infer missing child stops from root tool completion. The SDK's
single `subagents` feature flag has no depth field; the supported flag means
the direct child observations described here. Generic extension UI dialogs
also lack public paired observation events and are not included in attention
coverage. Child-local permission/question dialogs are not promoted to root
attention.

The verified native emitter includes the `willContinue` property even when
its value is undefined. A runtime event that omits the property has not
established those continuation semantics; the extension reports SDK lifecycle
degradation instead of inferring a successful finish. An extension context
without `agent.kind` cannot establish a root activation. These checks avoid
rejecting a compatible release merely because it is older than the inspected
source version.

Native `ctx.isIdle()` is not a terminal-event condition. Its underlying
`isStreaming` getter includes the owning prompt's in-flight counter, which
can remain nonzero during the single terminal extension notification. Native
execution confirmed `willContinue: undefined` together with `isIdle(): false`
on that callback. Completion follows the native continuation decision without
waiting for a second notification or a timer. The next user prompt receives
a new provider turn id.

These mappings follow
[`shared-events.ts`](https://github.com/can1357/oh-my-pi/blob/7a52f1e35eacc115b067c03a9c8ec7263e114cda/packages/coding-agent/src/extensibility/shared-events.ts),
[`extensions/types.ts`](https://github.com/can1357/oh-my-pi/blob/7a52f1e35eacc115b067c03a9c8ec7263e114cda/packages/coding-agent/src/extensibility/extensions/types.ts),
[`agent-session.ts`](https://github.com/can1357/oh-my-pi/blob/7a52f1e35eacc115b067c03a9c8ec7263e114cda/packages/coding-agent/src/session/agent-session.ts),
[`task/executor.ts`](https://github.com/can1357/oh-my-pi/blob/7a52f1e35eacc115b067c03a9c8ec7263e114cda/packages/coding-agent/src/task/executor.ts),
and
[`session-observer-registry.ts`](https://github.com/can1357/oh-my-pi/blob/7a52f1e35eacc115b067c03a9c8ec7263e114cda/packages/tui/src/overlays/session-observer-registry.ts).

## Correlation, context and helper lifetime

Each root activation has a random 128-bit nonce. Native facts capture its
session id and nonce before any asynchronous delivery. The adapter admits
ordinary facts only when both match the host-owned helper ledger. A switch
also names the previous nonce, so a delayed switch cannot replace a different
activation. Event ids and sequence numbers support host deduplication; the
adapter holds no cross-invocation ledger.

`before_agent_start` consumes host-rendered model context through
`model-context-delivery@2`, fenced by the captured session id and nonce.
Accepted context is returned as a hidden native custom message. Repeated
native preparation attempts retain that context until `agent_start` proves
the prepared prompt entered the loop. A response arriving after an activation
change is suppressed; a refusal never vetoes the user's native prompt or
retries with an unfenced version-1 request. Startup instructions and ordinary
user-prompt context are supported. Wake-only/synthetic turns that do not fire
this native hook do not receive an invented context-delivery event.

The extension spawns one attached package helper at a time. The helper uses
the SDK's `Helper_client` and inherited host authority to run `vnmctl`; it
does not open host sockets or serialize credentials. A transport retry keeps
the exact idempotency key and activation nonce. The caller keeps helper stdin
open after one bounded JSON line. Disposal closes the pipe; the helper's
nonblocking cancellation predicate makes the SDK kill and reap its wrapper.
Each helper invocation has a 750 ms deadline, below the native two-second
shutdown-handler budget. There are no detached processes or PID-only helper
kills. Telemetry contains bounded identities and status fields, not prompts,
tool arguments, plan text, answers, credentials or assistant reasoning.

Lifecycle notification handlers capture and enqueue their facts synchronously;
they do not hold the native extension delivery queue while helper I/O runs.
This matters because the native terminal callback is fire-and-forget and its
shutdown callback bypasses that delivery queue. On shutdown, already captured
facts drain in order for up to 750 ms, reserving the rest of the native
two-second handler budget for cancellation/reaping and final delivery. An
expired drain cancels the active helper through stdin EOF and reports
observation loss. It never infers terminal completion from elapsed time.

A delivery failure or queue overflow prevents later observations from
pretending that the activation was fully observed. The next delivery reports
SDK lifecycle degradation, and further lifecycle facts from that activation
are suppressed after the degradation is accepted. A new native activation
can establish fresh correlation. Context delivery failure alone is optional
context loss and does not fabricate lifecycle failure or completion.

## Exact resume boundary

Native `--resume`, `--session`, `--restore`, `--continue`, and `--fork` options
remain user-controlled. The add-on records the actual admitted session id
and file. It does not advertise the SDK's exact-record interactive-resume
capability. In the inspected native implementation, even an absolute resume
path first calls `findSessionContinuation`, may select a newer sibling file,
and then calls `forkIfMovedAway`. Session ownership conflicts can also create
a new continuation identity. There is no public CLI flag that disables these
policies. A prior read-only file-id check cannot make that subsequent launch
an exact-id operation. The relevant code is `createSessionManager` in
[`main.ts`](https://github.com/can1357/oh-my-pi/blob/7a52f1e35eacc115b067c03a9c8ec7263e114cda/packages/coding-agent/src/main.ts)
and the continuation/ownership handling in
[`session-manager.ts`](https://github.com/can1357/oh-my-pi/blob/7a52f1e35eacc115b067c03a9c8ec7263e114cda/packages/coding-agent/src/session/session-manager.ts).

## Structured and delegated RPC

The RPC implementation negotiates protocol v2 after native readiness,
selects delta message updates, inspects native state, and submits the exact prompt
only after negotiation. Completion requires native prompt-result and settled
state evidence; partial assistant text and a bare process exit do not prove
success. The SDK's launch bridge retains attached process custody and the
selected native executable. Saved environment, model, profile and approval
choices remain intact. Delegated model selection accepts native provider/model
keys and verifies the resolved model; selected unsupported reasoning levels fail
instead of silently substituting a different level. See `omp_rpc_tests` and
the public
[`docs/rpc.md`](https://github.com/can1357/oh-my-pi/blob/7a52f1e35eacc115b067c03a9c8ec7263e114cda/docs/rpc.md)
for readiness, terminal, error and model-selection contracts. The add-on does
not call `get_available_models` or provide a model catalog. The inspected
`get_state.model` and `thinkingLevel` fields validate explicit task choices;
they are not live interactive model telemetry.

## Launcher approval transformations

`launcher-approval@1` uses the SDK's literal launcher editor. OMP's
`cli/args.ts` treats `--yolo` and `--auto-approve` as `autoApprove: true`.
`cli/flag-tables.ts` accepts `--approval-mode` values `yolo`, `always-ask`,
`always-ask-cached` and `write`; `tools/approval.ts` makes `autoApprove`
override the configured mode. Yolofy replaces these known mode arguments and
bypass aliases with one `--yolo`. Unyolofy removes bypasses, preserves other
explicit modes and leaves native configuration unchanged. In particular,
OMP's schema default is `tools.approvalMode: yolo`, so removing a CLI override
does not establish that permission dialogs will appear.

The editor retains profile, model, configuration, prompt and tool-selection
operands. It rejects compound scripts, unknown option arities, management
commands and unsupported mode values. This includes `help` and OMP's reserved
top-level plugin words (`extensions`, `list`, `remove`, `uninstall`,
`marketplace`, `discover`, `upgrade`, `enable`, `disable`) as the first
positional operand: `cli-commands.ts` dispatches or rejects those using the
raw first argument, so moving a bypass before or away from them can change
a command into prompt text or the reverse. Explicit `launch` or `--` keeps
such words in a stable prompt role. The extension-shadowable `--plan`
option is deliberately outside this bounded editing grammar. Explicit
`--extension`, `--hook`, and `--trusted-extension` operands are also left to
the ordinary launcher: OMP can join split Windows module paths, which is
not a scalar option grammar the SDK editor promises to reinterpret. The contract
changes native tool-approval arguments only; it does not change plan workflow
or native tool-specific restrictions.

## Delegated native routing

Delegation uses OMP's supported `.claude-plugin/plugin.json` and `.mcp.json`
layout through `--plugin-dir`. This native plugin format accepts the host's
absolute relay executable; the portable Agent Plugins `mcp.json` format
restricts commands to bare names or contained relative paths and cannot
represent that relay. The host writes the files beneath its supplied task
root. Every server launches the host relay with the exact descriptor path
and original server id. Original server executables and credentials are not
copied. The contracts are
[`claude-plugins.ts`](https://github.com/can1357/oh-my-pi/blob/7a52f1e35eacc115b067c03a9c8ec7263e114cda/packages/coding-agent/src/discovery/claude-plugins.ts),
[`substitute-plugin-root.ts`](https://github.com/can1357/oh-my-pi/blob/7a52f1e35eacc115b067c03a9c8ec7263e114cda/packages/coding-agent/src/discovery/substitute-plugin-root.ts),
and the distinct restrictions in
[`agent-plugin-format.ts`](https://github.com/can1357/oh-my-pi/blob/7a52f1e35eacc115b067c03a9c8ec7263e114cda/packages/coding-agent/src/discovery/agent-plugin-format.ts).

OMP enables `tools.xdev` by default, so many callable MCP tools are mounted
under `xd://` and absent from `get_state.dumpTools`. A task-local extension
records enabled MCP metadata at native `session_start`: `getActiveTools()`
provides the union of top-level and mounted tool names, while `getAllTools()`
provides the original MCP server and action identities. The extension uses
`appendEntry`, which adds no model prompt. RPC `get_entries` must return one
inventory record for this task's scope containing every requested server and
action before the task prompt is sent. Unrelated or duplicate inventories
cannot establish readiness. This preserves the user's tool exposure and
approval settings. The native bindings are in
[`runtime-init.ts`](https://github.com/can1357/oh-my-pi/blob/7a52f1e35eacc115b067c03a9c8ec7263e114cda/packages/coding-agent/src/modes/runtime-init.ts),
[`session-tools.ts`](https://github.com/can1357/oh-my-pi/blob/7a52f1e35eacc115b067c03a9c8ec7263e114cda/packages/coding-agent/src/session/session-tools.ts),
and
[`extensions/types.ts`](https://github.com/can1357/oh-my-pi/blob/7a52f1e35eacc115b067c03a9c8ec7263e114cda/packages/coding-agent/src/extensibility/extensions/types.ts).

An explicit delegated model or reasoning selection adds a task-local
`--config` overlay disabling `retry.modelFallback` and
`contextPromotion.enabled`. Same-model retry settings remain unchanged.
Startup identity, assistant-message provider/model identity, fallback events,
and thinking-level changes are checked against the selected values. Omitted
selection adds no such overlay and preserves native recovery defaults. These
native replacement paths are defined by
[`turn-recovery.ts`](https://github.com/can1357/oh-my-pi/blob/7a52f1e35eacc115b067c03a9c8ec7263e114cda/packages/coding-agent/src/session/turn-recovery.ts)
and
[`context-settings.ts`](https://github.com/can1357/oh-my-pi/blob/7a52f1e35eacc115b067c03a9c8ec7263e114cda/packages/coding-agent/src/session/context-settings.ts).

The finite ambient declarations preserve `OMP_PROFILE`, `PI_PROFILE`,
`PI_CONFIG_DIR`, `PI_CODING_AGENT_DIR`, and the XDG state/cache selectors;
the SDK already supplies its home/PATH/XDG data/config baseline. Empty values
are retained, including an empty `OMP_PROFILE` that intentionally overrides
`PI_PROFILE`. The session declarations use the SDK's 32-name allowance for
these selectors, common provider credentials and endpoint selectors, and
HTTP proxy settings. Additional native provider variables must be selected
explicitly in the saved launcher's environment; they then pass through the
SDK's frozen effective environment. Native saved authentication and profile
files continue to use the selected native directories. See
[`dirs.ts`](https://github.com/can1357/oh-my-pi/blob/7a52f1e35eacc115b067c03a9c8ec7263e114cda/packages/utils/src/dirs.ts)
and the package capability sidecars for the precise names.

Settlement waits for owner-scoped background work whose delivery can still
wake the session. OMP excludes suppressed, acknowledged, or watched job
deliveries from that predicate. An unsuppressed persistent server can remain
unsettled until the original host deadline or cancellation; the add-on does
not infer success from silence or suppress required work. Stdin closure is
followed by final output and exit validation. The native predicate is in
[`session-settle.ts`](https://github.com/can1357/oh-my-pi/blob/7a52f1e35eacc115b067c03a9c8ec7263e114cda/packages/coding-agent/src/session/session-settle.ts)
and `hasPendingAsyncWork()` in `agent-session.ts`.

Windows structured sessions support the native `omp.exe` installed by OMP's
default official
[`install.ps1`](https://github.com/can1357/oh-my-pi/blob/7a52f1e35eacc115b067c03a9c8ec7263e114cda/scripts/install.ps1).
The SDK's native launch bridge owns Unicode path/argument transport and
attached process lifetime. Arbitrary npm `.cmd` and PowerShell `.ps1`
wrappers remain unavailable for structured replacement; the add-on does not
guess a Bun executable or reinterpret those scripts. Source review and
portable contract fixtures do not establish execution on Windows; native
acceptance evidence must identify the platform on which it ran.
