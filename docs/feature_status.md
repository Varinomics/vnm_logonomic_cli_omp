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
| Direct task/eval children | Implemented | Native child start/stop IDs, labels and serving provider/model/reasoning; optional `child-state@1` adds native descriptions, outcomes, current tool and progress. The host must adopt each optional contract. |
| Native child runtime acceptance | Blocked in this workspace | The real Task tool reaches native child creation, which fails on the native temporary session lock with Linux `EPERM` before a child starts. Label generation is not counted as child execution. A strict normal-host acceptance scenario remains required. |
| Nested Task children | Implemented with bounded coverage | Negotiated `child-state@1` projects nested Task snapshots carried by admitted direct-child progress, requiring real execution evidence and preserving parent/depth. Missing descendants are not inferred stopped. This is not a complete native tree subscription. |
| `/tan`, advisors and child-local dialogs | Not covered | These sources have no adopted native lifecycle/parent identity route in this package; Task snapshots cannot establish their coverage. |
| Delegated model and reasoning choice | Implemented | Exact requested native selection is acknowledged and checked; silent fallback of an explicit choice fails. |
| Active root/child model and reasoning reporting | Implemented | Optional `model-state@1` reports sampled root selection, idle changes, and admitted direct-child serving identity. Unknown values remain unknown. The host must support the new optional contract. |
| Available-model catalog | Implemented for live roots | Optional `model-catalog@1` samples the running profile through public `ctx.models.list()`, reports exact totals and explicit partiality, and reaches the existing host Session details list. This configured registry set differs from the native RPC picker filter and does not prove model access. |
| Host instructions to native model | Implemented | Hidden native message, fenced to the captured activation. This is instruction delivery, not model identity reporting. |
| Context occupancy and compactions | Implemented | `context-state@2` uses native occupancy measurements and successful installed compaction events; missing measurements remain unknown. Real root and installed-compaction acceptance checks passed separately. |
| Native working directory reporting | Implemented | `execution-context@1` samples native `ctx.cwd` at public activation, turn, message and compaction events. The native directory-override check passed; no immediate idle-change notification is promised. |
| Host wake | Implemented | `session-wake@1` uses an exact live activation binding and native hidden aside admission. Known busy/paused/attention states defer. The void native API returns an uncertain submission result; a real PTY gate verified a second turn in the same process/session without editor input. |
| Interactive startup prompt adaptation | Implemented | `interactive-initial-prompt@1` carries one exact host-authored first user message after native `--` on fresh supported launches. Existing launcher prompts/attachments, session selectors, goals and text subject to native command/model-mention expansion are explicitly unavailable. |
| Structured/delegated task execution | Implemented | Native RPC v2 prompt result plus settled evidence; host retains process cancellation and budget authority. |
| Read-only structured/delegated work | Implemented | Negotiated `read_only_review` uses a trusted native execution guard, file-tool provenance and task-scoped startup acknowledgment. Only exact host task-control actions may execute through MCP. See [read-only reviews](read_only_reviews.md) for the native scope and qualification. |
| Exact-record interactive resume | Not declared | Native path resume can follow continuations or fork. A guarded conditional recipe remains unimplemented and unqualified; the native persistence probe cannot acquire its file lock in this workspace. This does not establish that an exact guarded integration is impossible. |
| Conversation export | Implemented | `conversation-export@1` reads the exact captured local version 3 JSONL file and exports its active branch. In-memory, gzip/database, unknown-version, missing-locator and foreign-ID sources fail explicitly. No native resume is needed. |
| Usage accounting | Implemented with explicit partial history | `usage-scan@3` retains native JSONL event evidence with fork/discard deduplication, auxiliary calls, exact host-selected cache custody and atomic `usage-cross-source-event@1` proofs. Task aggregates, missing history and unsupported storage are not guessed. See [usage accounting](usage_accounting.md). |
| Native source-profile accounts | Implemented | `accounts@2` discovers and resolves native filesystem profiles, including explicit external agent directories, and verifies versioned selected-account preparation. This identifies a source profile, not a historical provider principal. |
| Authentication operations | Not implemented | Native RPC exposes login/logout operations, but the current SDK has no adopted host operation/UI contract for them. OAuth-provider credential-source presence alone does not prove authentication for all configured API-key/custom providers. |
| Quota reporting | Not implemented | No native account-scoped quota observation is translated into a host contract. |
| Cooperative descriptions | Implemented with host negotiation | Public native `logonomic_describe_session` returns explicit model-authored titles and working summaries through `cooperative-description@1`. The host applies its existing policies under an exact native activation guard; carrier acceptance alone is not a successful description receipt. See [cooperative descriptions](cooperative_descriptions.md). |
| Host-blockable completion | Undeclared | Native `session_stop` supports an awaited veto or continuation, but the current authenticated host contract supplies no completion-policy decision. Its title-specific `instruction_required` flag cannot authorize a veto. |
| Host turn-origin correlation | Undeclared | Native turn IDs and exact wake targets do not establish a confirmed host origin for a subsequent turn. The current lifecycle contract has no authoritative host turn-origin record. |

The SDK capability files are the adoption authority. A row describing a
native possibility is not a declaration that Logonomic can use it yet.
