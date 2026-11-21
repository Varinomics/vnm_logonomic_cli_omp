# Logonomic oh-my-pi add-on

Native Logonomic CLI add-on for [oh-my-pi](https://github.com/can1357/oh-my-pi).
It identifies the installed `omp` command and uses the C++20
[Logonomic CLI add-on SDK](https://github.com/Varinomics/vnm_logonomic_cli_addon/tree/master/sdk/logonomic_cli_addon_sdk).
Install oh-my-pi separately using its upstream instructions; this package does
not contain the CLI or change its installation.

## Using the package

Place the assembled `logonomic.cli.omp` directory in Logonomic's CLI add-on
directory, or select its parent directory as the add-on development root.
Restart Logonomic after installing or replacing the package, then rescan
launchers to discover `omp` from the launch environment's `PATH`.
Existing literal `omp` launchers are recognized, including Windows executable
and npm command wrappers. Compound shell scripts remain ordinary launchers.

For a supported interactive launcher, Logonomic materializes the package's
native extension and adds it to that launch. There is no separate global
oh-my-pi configuration edit. The native CLI keeps its own models, credentials,
settings and approval choices. An existing `--trusted-extension` allowlist
remains an allowlist when the package module is added.

## Implemented features

| Feature | Implementation |
|---|---|
| Launcher discovery and preparation | Frozen-environment `omp` discovery, literal launcher recognition, preserved native arguments and a custodied extension asset. |
| Launcher approval controls | Yolofy and Unyolofy edit native tool-approval flags in understood literal launchers while retaining other arguments and saved configuration. |
| Native source profiles | `accounts@2` discovers and resolves filesystem profiles and checks exact selected-account acknowledgments before preparation. |
| Root session lifecycle | Native session load, successful switch, clear, branch/tree change and shutdown. Each activation has separate correlation even when `/clear` keeps the native UUID. |
| Root working and completion | Native agent-loop events and continuation decisions, including the terminal callback before prompt unwinding. Successful, failed and interrupted outcomes remain distinct. |
| Context and working directory | Native occupancy, installed compaction events and current directory observations, correlated to the active conversation. |
| Active model observations | Root provider/model/reasoning samples, including idle selection changes, and serving-model facts for admitted direct children through optional `model-state@1`. |
| Available model catalog | Optional live configured-model inventory from the running native profile, with explicit partial counts and a Session details list. |
| Permission and question attention | Confirmed native tool approval and plan-review pairs; inferred UI `ask` execution pairs. |
| Task/eval children | Native child identities and start/stop events, plus optional descriptions, progress, outcomes and bounded nested Task snapshots through `child-state@1`. |
| Startup and turn context | Host-owned hidden custom-message content through the native `before_agent_start` hook, fenced to the captured activation. |
| Cooperative session descriptions | A public native tool returns requested model-authored titles and working summaries through the host's existing policies and exact activation-bound `cooperative-description@1` receipts. |
| Host-authored initial prompt | One exact first user message for a fresh supported interactive invocation, acknowledged through `interactive-initial-prompt@1`. |
| Mailbox wake | A private binding targets the exact live interactive activation; native aside delivery can start an idle turn without editor input. |
| Structured work | Native RPC v2 readiness/negotiation, bounded incremental inspection, exact prompt delivery and verified terminal evidence. |
| Read-only structured and delegated reviews | Trusted native tool-call guard, exact startup acknowledgment, native file reads and only host-authorized task-control actions. See [read-only reviews](docs/read_only_reviews.md). |
| Delegated work and model selection | Exact requested model and supported reasoning-level selection, with native state and response validation through the same RPC path. |
| Conversation export | Exact captured native JSONL file, including custom filenames; selected branch; full or compact text, Markdown, and highlighted conversation formats. |
| Historical usage | Native event ledger with fork/discard deduplication, auxiliary model calls, sealed host-selected cache retention and explicit incomplete-history coverage. |

Each package declares these capabilities in `package/capabilities`;
Logonomic adopts only the intersection supported by its host and the running
adapter. The inspected upstream version is recorded as provenance, with no
version ceiling or arbitrary tested-version minimum. Native runtime evidence
guards completion; a missing continuation field degrades observation instead
of manufacturing success.

Model selection applies to delegated tasks. Interactive model observations use
the optional `model-state@1` host contract; older hosts can still use the other
features. Optional `model-catalog@1` supplies the running profile's configured
model set; it is separate from current serving-model facts and verified model
access. See [the model catalog contract](docs/model_catalog.md) for sampling,
bounds and the difference from the native model picker.
Yolofy adds native `--yolo`; Unyolofy removes recognized invocation bypasses.
Unyolofy preserves saved native policy, which may itself permit tools without
asking. Neither action edits the native configuration or imposes a new policy.

The child feature covers immediate task-tool and eval `agent()` children on the
public session bus. With `child-state@1`, admitted direct-child progress can
also carry bounded nested Task snapshots with real execution evidence and
parent/depth relationships. This does not promise a complete descendant tree,
`/tan` clones, advisor activity, or child-local dialogs. Native Linux x86-64
acceptance exercised one direct Task child and a second Task with a grandchild
using OMP 18.22.2 and a local model on ext4.
Generic dialogs from other extensions have no paired native observation
surface and are not covered. The package never uses the native `session_stop`
veto gate as a completed-turn signal.

Native resume/continue/fork flags are preserved. The SDK's exact-record
interactive-resume capability remains undeclared: native path-based
resume can follow a newer continuation or fork a moved session, so an absolute
path alone cannot guarantee the requested native identity. The extension
reports whichever native identity actually loaded. See
[the native contract](docs/native_contract.md) for the evidence and complete
coverage boundaries, and [feature coverage](docs/feature_status.md) for the
implemented and missing host integrations.
The [native observation contract](docs/native_observations.md) describes context,
model, compaction and directory sampling boundaries. [Conversation export](docs/conversation_export.md)
documents exact source selection and the persisted formats it can read.
[Native mailbox wake](docs/native_wake.md) documents activation binding,
native pause/attention policy, and the uncertain return after void native
admission. The separate [native acceptance](docs/native_acceptance.md) records
the successful same-process idle-wake check.

Noninteractive modes, compound scripts, positional startup prompts, unknown
extension-owned launch flags, and Windows `.cmd`/`.ps1` wrappers retain their
ordinary launcher behavior without interactive lifecycle promises. Supported
Windows preparation uses a native `omp.exe`; POSIX executable/shebang
launchers are supported. [Native source profiles](docs/native_profiles.md) are
filesystem identities. [Historical usage](docs/usage_accounting.md) counts
retained native source events; authentication operations, provider-principal
identity and remaining quota are separate and remain undeclared.

A host initial prompt is supported on a fresh launcher without an existing
prompt, attachment or resume/continue/fork/goal selector. It is passed as one
literal argument after `--`. Slash-command and caret model-mention text is
refused because native processing can rewrite it. A refused host initial prompt
fails preparation explicitly; it does not fall back to a launch without the
requested message.

## Building

The [contributor guide](https://github.com/Varinomics/vnm_logonomic_cli_addon/blob/master/CONTRIBUTING.md) covers local package checks.
Send provider pull requests to [the omp repository](https://github.com/Varinomics/vnm_logonomic_cli_omp/pulls).

Use CMake 3.25 or later and a C++20 compiler. The example builds in
`../build-omp`. To use local SDK sources, set `LOGONOMIC_CLI_ADDON_SDK_SOURCE`
to the SDK's `sdk/logonomic_cli_addon_sdk` directory at configure time.

```sh
cmake -S . -B ../build-omp -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build ../build-omp --parallel 4
ctest --test-dir ../build-omp --output-on-failure
```

The `omp_package` target assembles the indexed package below
`../build-omp/package/logonomic.cli.omp` (`package/<config>/logonomic.cli.omp`
with a multi-configuration generator). Linux package assembly also requires
Python 3.11 or later and `patchelf` for the SDK's runtime deployment.
`cmake --install ../build-omp --prefix /fresh/export/directory` exports the
package; the destination package directory must not already exist.

The local tests check launcher preparation, the SDK lifecycle feature gate,
native event/activation/context fixtures, RPC negotiation and terminal/error
classification, package integrity, and SDK conformance. Node.js with TypeScript
stripping support enables the native extension fixture test; run it directly
with `node --experimental-strip-types tests/omp_extension_tests.mjs`.
Package conformance and scripted fixtures do not establish authenticated model
behavior; native CLI acceptance evidence is documented separately.
The inspected upstream contract is recorded in
`package/provenance/upstream_contract.json`.

The package configuration targets Windows and Linux x86-64 and macOS ARM64;
each target requires its own native build and acceptance validation. A Logonomic
build can also add this project with `LOGONOMIC_CLI_ADDON_EMBEDDED` after adding
the SDK; the project then registers its package without installing files or
adding standalone tests to the host build.

