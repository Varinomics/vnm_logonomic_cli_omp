# Logonomic oh-my-pi add-on

Native Logonomic CLI add-on for [oh-my-pi](https://github.com/can1357/oh-my-pi).
It identifies the installed `omp` command and uses the public C++20
[Logonomic CLI add-on SDK](https://github.com/Varinomics/logonomic/tree/master/sdk/logonomic_cli_addon_sdk).
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
| Root session lifecycle | Native session load, successful switch, clear, branch/tree change and shutdown. Each activation has separate correlation even when `/clear` keeps the native UUID. |
| Root working and completion | Native agent-loop events and continuation decisions, including the terminal callback before prompt unwinding. Successful, failed and interrupted outcomes remain distinct. |
| Context and working directory | Native occupancy, installed compaction events and current directory observations, correlated to the active conversation. |
| Permission and question attention | Confirmed native tool approval and plan-review pairs; inferred UI `ask` execution pairs. |
| Direct subagents | Native task/eval child registry ids and start/stop events, kept separate from root completion. |
| Startup and turn context | Host-owned hidden custom-message content through the native `before_agent_start` hook, fenced to the captured activation. |
| Structured work | Native RPC v2 readiness/negotiation, bounded incremental inspection, exact prompt delivery and verified terminal evidence. |
| Delegated work and model selection | Exact requested model and supported reasoning-level selection, with native state and response validation through the same RPC path. |

Each package declares these capabilities in `package/capabilities`;
Logonomic adopts only the intersection supported by its host and the running
adapter. The inspected upstream version is recorded as provenance, with no
version ceiling or arbitrary tested-version minimum. Native runtime evidence
guards completion; a missing continuation field degrades observation instead
of manufacturing success.

Model selection applies to delegated tasks. This version does not list available
models or report the active interactive model, provider, or reasoning level.
Yolofy adds native `--yolo`; Unyolofy removes recognized invocation bypasses.
Unyolofy preserves saved native policy, which may itself permit tools without
asking. Neither action edits the native configuration or imposes a new policy.

The direct-subagent feature covers immediate task-tool and eval `agent()`
children on the public session bus. It does not promise the private whole
descendant tree, `/tan` clones, advisor activity, or child-local dialogs.
Generic dialogs from other extensions have no paired native observation
surface and are not covered. The package never uses the native `session_stop`
veto gate as a completed-turn signal.

Native resume/continue/fork flags are preserved. The SDK's exact-record
interactive-resume capability is intentionally absent: native path-based
resume can follow a newer continuation or fork a moved session, so an absolute
path alone cannot guarantee the requested native identity. The extension
reports whichever native identity actually loaded. See
[the native contract](docs/native_contract.md) for the evidence and complete
coverage boundaries, and [feature coverage](docs/feature_status.md) for the
implemented and missing host integrations.
The [native observation contract](docs/native_observations.md) describes context
measurement, compaction and directory sampling boundaries.

Noninteractive modes, compound scripts, positional startup prompts, unknown
extension-owned launch flags, and Windows `.cmd`/`.ps1` wrappers retain their
ordinary launcher behavior without interactive lifecycle promises. Supported
Windows preparation uses a native `omp.exe`; POSIX executable/shebang
launchers are supported. Credentials, native account management, quota,
usage accounting, and conversation export remain outside this package's
declared capabilities.

## Building

Use CMake 3.25 or later, a C++20 compiler, and the workstation build queue.
The SDK dependency follows the owned Logonomic `master` branch. To use a
current local SDK source tree:

```sh
queued-build --slots 1 -- cmake -S . -B ../build-omp -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DLOGONOMIC_CLI_ADDON_SDK_SOURCE=/path/to/logonomic/sdk/logonomic_cli_addon_sdk
queued-build --slots 4 -- cmake --build ../build-omp --parallel 4
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

