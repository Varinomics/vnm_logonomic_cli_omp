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

## Interactive observations

| Native evidence | Package observation | Scope and qualification |
|---|---|---|
| `session_start` | Session started, source `load` | Initial load can already be a native resumed conversation. The package reports its actual loaded id. |
| `session_switch` | New activation with the native reason | `new`, `resume`, and `fork` are emitted after the switch succeeds. |
| `session_clear` | New activation, source `clear` | Native `/clear` keeps the session UUID and file. A new nonce separates its live context from the previous activation. |
| `session_branch`, `session_tree` | New activation | Context rewinds cannot reuse the previous activation's model context or child state. |
| `session_shutdown` | Session ended | Only for the root binding; never claims successful work. |
| `agent_start` | Confirmed working phase | Creates a provider turn id. Automatic continuation retains that id. |
| Terminal `agent_end` | Confirmed awaiting-input phase | Requires the native emitter's own `willContinue` field with value undefined/false and the native context currently idle. |
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
disables streamed deltas, inspects native state, and submits the exact prompt
only after negotiation. Completion requires native prompt-result and settled
state evidence; partial assistant text and a bare process exit do not prove
success. The SDK's launch bridge retains attached process custody and the
selected native executable. Saved environment, model, profile and approval
choices remain intact. Delegated model selection uses exact provider/model
keys from the native catalog; selected unsupported reasoning levels fail
instead of silently substituting a different level. See `omp_rpc_tests` and
the public
[`docs/rpc.md`](https://github.com/can1357/oh-my-pi/blob/7a52f1e35eacc115b067c03a9c8ec7263e114cda/docs/rpc.md)
for readiness, terminal, error and model-catalog fixture contracts.
