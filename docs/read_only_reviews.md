# Read-only structured and delegated reviews

The package declares `read_only_review` for `structured-session@3` and
`delegated-session@3`. A review uses the selected native executable, source
profile, model and account through the same frozen launch context as an
implementation task. The host must also adopt the managed
`assets/native_review.ts` module; without it, preparation returns
`read_only_unavailable`.

## Native execution boundary

The host materializes the guard from the immutable package and substitutes
its path in `OMP_LOGONOMIC_REVIEW_ASSET`. Process argv is literal in the SDK,
so the attached `omp_review_bridge` supplies that path to native
`--trusted-extension`. The bridge retains the existing standard streams,
working directory, process custody and exit status. It requests native
`--mode rpc --no-ui`, `--tools read,grep,glob`, and an in-memory session with
`--no-session`. LSP, judge, spotlight and PTY helpers are disabled for this
bounded review route.

Tool selection controls what the model sees. The public native `tool_call`
handler enforces what can execute:

- `read`, `grep` and `glob` require the live native tool's built-in provenance
  and synthetic built-in source path. An extension replacing a built-in name
  is refused.
- Ordinary writes, shell execution, task spawning and other tools are
  refused. File-read arguments cannot select arbitrary URL handlers, including
  `ssh://` and web URLs. The native `xd://` inventory and the exact authorized
  task tools' documentation remain readable.
- A delegated review may invoke only the exact `(MCP server, action)` pairs
  supplied by the host's task-control connection. Its native tool provenance
  must also match. A server's `readOnlyHint`, an action with a similar name,
  or another action advertised by the same server grants no authority.
- Native `write xd://<tool>` is allowed only for those exact host actions,
  with an object JSON payload. Native nested dispatch passes through
  `tool_call` again. Ordinary filesystem writes through `write` remain blocked.

The guard checks current tool metadata on every call. A switch, clear,
branch, tree change or shutdown retires the task's activation. A child
extension context cannot use the root's review authority.

The native mechanisms are documented by the inspected OMP 18.20.2 source:
[`ToolInfo` and extension event results](https://github.com/can1357/oh-my-pi/blob/7a52f1e35eacc115b067c03a9c8ec7263e114cda/packages/coding-agent/src/extensibility/extensions/types.ts),
[`getAllToolInfos`](https://github.com/can1357/oh-my-pi/blob/7a52f1e35eacc115b067c03a9c8ec7263e114cda/packages/coding-agent/src/session/session-tools.ts),
the fail-closed [`emitToolCall` handler](https://github.com/can1357/oh-my-pi/blob/7a52f1e35eacc115b067c03a9c8ec7263e114cda/packages/coding-agent/src/extensibility/extensions/runner.ts),
and the [execution wrapper](https://github.com/can1357/oh-my-pi/blob/7a52f1e35eacc115b067c03a9c8ec7263e114cda/packages/coding-agent/src/extensibility/extensions/wrapper.ts).
Native [xd dispatch](https://github.com/can1357/oh-my-pi/blob/7a52f1e35eacc115b067c03a9c8ec7263e114cda/packages/coding-agent/src/internal-urls/xd-protocol.ts)
explains why a tool list alone is insufficient.

## Admission and preserved configuration

Preparation rejects a task beginning with a native slash command, because
the RPC input path can execute commands before entering the model/tool loop.
The native input handler also consumes later command-shaped input. Saved
launchers explicitly selecting their own extension, hook, tool list, session
directory, plan, prewalk or advisor policy are refused for this purpose.
Their normal implementation and interactive routes retain their existing
behavior.

The guard loads through OMP's exact trusted-module path. It records a
`logonomic-read-only` entry only after checking the built-in tools, installing
the active tool set, and retaining the same main session. The adapter first
negotiates RPC v2 and verifies native idle state. It then requests
`get_entries` and requires one acknowledgment with this preparation's random
scope and the exact `get_state.sessionId`. Missing, duplicate, stale or
foreign-session acknowledgments fail before the prompt is sent. Delegation
also retains its independent task-control inventory check.

The guard adds no credential and grants no tool approval. Existing native
approval policy can still refuse an otherwise admitted read or host action.
Exact delegated model selection and native completion/settlement checks are
unchanged. The finite task-control policy must fit the SDK environment-value
bound; an oversized exact policy fails explicitly.

This boundary governs model tool execution. OMP still loads the selected
profile and initializes native configuration, installed custom-tool factories
and MCP servers; their startup behavior and native caches remain under the
user's installed runtime. An OS filesystem sandbox is outside this capability.
The native guard does not reinterpret a configured tool's annotation as proof
that executing it is safe.

## Qualification

The portable fixtures exercise tool provenance, changed metadata, URI and
write refusal, exact task-action admission, input handling, activation
retirement and startup acknowledgment. `omp_rpc_tests` exercises the actual
preparation and inspection code. Package validation and SDK conformance run
against the assembled package.

`tests/native_review_acceptance.py` is an opt-in native gate. It uses the
official installed OMP 18.20.2 CLI with Bun 1.4.4, the actual packaged adapter,
bridge and guard, and an isolated loopback model fixture. The SDK typed host
driver obtains the real preparation result; the harness applies its documented
managed-asset substitution and owns the process. It does not run the desktop
custodian or use a real provider account.

The Linux native gate observed:

| Scenario | Actual native result |
|---|---|
| Structured review | A file read succeeded. Ordinary write, shell, remote read and Task attempts returned tool errors. The workspace file remained unchanged, no requested new file appeared, and the process exited 0 after six local model requests. |
| Delegated review with default xd routing | The same file read succeeded. The native task-control relay received exactly one `report_result` call. An unrequested `send_message` action from that same server was refused, along with the mutation attempts. The workspace remained unchanged and the process exited 0 after nine local model requests. |

Some excluded tools are absent from the active native registry; attempted
calls receive native “tool not found” errors. The installed `read` and
delegated `write` paths also exercised the guard's own refusal result. The
test does not count an absent tool as evidence that its execute callback ran.

The gate writes `metadata.json` with the package, driver, harness, native CLI
and Bun hashes, plus native frames, model requests, SDK inspection results and
relay requests. Run it against an already built package and an existing
official runtime installation:

```sh
python3 tests/native_review_acceptance.py \
  --runtime /absolute/npm-prefix \
  --package /absolute/build/package/logonomic.cli.omp \
  --driver /absolute/build/omp_rpc_tests \
  --host-driver /absolute/build/omp_native_host_driver \
  --artifacts /absolute/new-evidence-directory
```

This qualification covers Linux native execution. The Windows attached
process implementation follows the SDK launch-bridge handle and job-custody
pattern; this workspace did not execute a Windows or macOS native gate.
