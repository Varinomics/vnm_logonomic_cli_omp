# Feature coverage

This table describes implemented add-on contracts. It does not claim that
every native oh-my-pi feature is exposed by Logonomic. Native source evidence
and the separate runtime checks are described in [the native contract](native_contract.md)
and [native acceptance](native_acceptance.md).
Context and working-directory evidence is specified in
[native observations](native_observations.md).

| Feature | Current add-on coverage | Boundary or missing connection |
| --- | --- | --- |
| Launcher discovery and interactive preparation | Implemented | Literal native launcher; unsupported script shapes retain ordinary launch behavior. |
| Launcher approval controls | Implemented | Native `--yolo`, `--auto-approve`, and understood `--approval-mode` values. Removing invocation bypasses leaves saved native policy intact. |
| Root session and turn lifecycle | Implemented | Correlated native activation and terminal continuation evidence; no success inferred from process exit alone. |
| Tool permission and plan-review attention | Implemented | Native paired callbacks; `ask` tool brackets provide inferred question attention. Other extension dialogs lack an observed native pair. |
| Direct task/eval children | Implemented | Native child start/stop IDs and labels, plus exact serving provider/model/reasoning when native progress reports it. Whole-tree and detailed child-progress contracts are separate. |
| Native child runtime acceptance | Blocked in this workspace | The real Task tool reaches native child creation, which fails on the native temporary session lock with Linux `EPERM` before a child starts. Label generation is not counted as child execution. A strict normal-host acceptance scenario remains required. |
| Nested children, `/tan`, advisors | Not covered | The current extension consumes the public session's direct child bus, not private whole-tree subscription state. |
| Delegated model and reasoning choice | Implemented | Exact requested native selection is acknowledged and checked; silent fallback of an explicit choice fails. |
| Active root/child model and reasoning reporting | Implemented | Optional `model-state@1` reports sampled root selection, idle changes, and admitted direct-child serving identity. Unknown values remain unknown. The host must support the new optional contract. |
| Available-model catalog | Not implemented | Native `get_available_models` exists. The add-on makes no listing request; `delegated-model-selection@1` only selects values. |
| Host instructions to native model | Implemented | Hidden native message, fenced to the captured activation. This is instruction delivery, not model identity reporting. |
| Context occupancy and compactions | Implemented | `context-state@2` uses native occupancy measurements and successful installed compaction events; missing measurements remain unknown. A real root turn verified occupancy; installed compaction has its own acceptance check. |
| Native working directory reporting | Implemented | `execution-context@1` samples native `ctx.cwd` at public activation, turn, message and compaction events; no immediate idle-change notification is promised. |
| Host wake | Not implemented | Instruction delivery alone does not initiate a native turn. An attached, activation-scoped native delivery endpoint is required. |
| Interactive startup prompt adaptation | Not implemented | The current preparation grammar refuses positional prompts. Native prompt/attachment boundaries must be preserved when inserting the extension. |
| Structured/delegated task execution | Implemented | Native RPC v2 prompt result plus settled evidence; host retains process cancellation and budget authority. |
| Read-only structured/delegated work | Unavailable | Native approval modes and tool lists do not establish a read-only execution boundary for extensions, shell tools, or user policy. |
| Exact-record interactive resume | Unavailable | Native path resume can follow a newer continuation or fork a moved conversation; path alone is not exact native identity. |
| Conversation export | Implemented | `conversation-export@1` reads the exact captured local version 3 JSONL file and exports its active branch. In-memory, gzip/database, unknown-version, missing-locator and foreign-ID sources fail explicitly. No native resume is needed. |
| Usage accounting | Not implemented | No `usage-scan` producer maps native persisted usage and source coverage into SDK records. |
| Managed accounts/authentication | Not implemented | Existing launches preserve the chosen native profile/credentials; no managed account ownership or authentication-operation contract is declared. |
| Quota reporting | Not implemented | No native account-scoped quota observation is translated into a host contract. |
| Cooperative descriptions | Not implemented | Child definition labels are reported; no model-produced description protocol is requested or consumed. |
| Host-blockable completion | Unavailable | Native `session_stop` is a veto/continuation gate, not a completed turn. This package does not make host policy part of that native gate. |
| Host turn-origin correlation | Not implemented | The extension assigns local turn IDs; it has no confirmed host-origin ID for every native prompt. |

The SDK capability files are the adoption authority. A row describing a
native possibility is not a declaration that Logonomic can use it yet.
