# Native acceptance

`tests/native_acceptance.py` exercises the assembled add-on against an actual
installed oh-my-pi CLI. The model endpoint, delegated task server and host
wrapper are local fixtures. They provide deterministic responses while the
native CLI performs its ordinary model transport, tool dispatch, extension
loading, RPC handling and shutdown. This gate is separate from the portable
SDK conformance and scripted parser/extension tests.

The harness currently requires **Linux x86-64**, Python 3.11 or later, the
official OMP package, Bun, the assembled add-on, its `omp_rpc_tests` driver and
the SDK host simulator. The inspected native contract is documented in
[native_contract.md](native_contract.md); the implementation does not impose
the version used by this acceptance procedure as a compatibility cutoff.

## Prepare and run

Use an isolated npm prefix. The initial qualification used OMP 18.20.2 and
Bun 1.4.4. These are reproducible qualification inputs, not a recommendation to
replace an existing user installation. OMP's official package and installation
instructions are in the
[upstream repository](https://github.com/can1357/oh-my-pi/tree/7a52f1e35eacc115b067c03a9c8ec7263e114cda).

```sh
npm install --prefix /absolute/path/native-runtime --ignore-scripts \
  --no-audit --no-fund \
  @oven/bun-linux-x64@1.4.4 @oh-my-pi/pi-coding-agent@18.20.2
```

Build a frozen source candidate with the current SDK and the workstation
build queue. Enable the available compiler cache in the local configuration.
The normal package build also produces the validator, conformance runner,
host simulator and test driver:

```sh
queued-build --slots 1 -- cmake -S /absolute/path/source -B /absolute/path/build \
  -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  -DLOGONOMIC_CLI_ADDON_SDK_SOURCE=/absolute/path/logonomic_cli_addon_sdk
queued-build --slots 4 -- cmake --build /absolute/path/build --parallel 4
ctest --test-dir /absolute/path/build --output-on-failure
```

Run from that same frozen source tree. The evidence directory must be new;
the harness refuses to overwrite a previous run.

```sh
python3 /absolute/path/source/tests/native_acceptance.py \
  --runtime /absolute/path/native-runtime \
  --driver /absolute/path/build/omp_rpc_tests \
  --package /absolute/path/build/package/logonomic.cli.omp \
  --host-simulator /absolute/path/build/sdk/logonomic_cli_addon_host_simulator \
  --artifacts /absolute/path/native-evidence \
  --scenario all
```

The example simulator path applies to a local SDK override. Use the actual
generated path when FetchContent or an embedding build supplies the SDK.
Run one named scenario when a change affects only that native boundary.

## What the gate establishes

Every process proposal starts with the actual adapter's discovery and
preparation operations through the SDK host simulator. The harness copies
only the host baseline and the capability's declared ambient names into the
frozen launch environment. It writes the adapter's private artifacts beneath
the supplied root and runs the packaged launch bridge. Native stdout is fed
incrementally through the exact C++ inspector; the final result is also
checked through the actual adapter handler.

| Scenario | Native operation and required evidence |
|---|---|
| `structured` | RPC readiness, protocol negotiation, one local model response, correlated prompt completion, settlement, stdin closure and successful process exit. The final SDK answer must equal the emitted assistant text. |
| `profile` | Only the selected native profile contains the model configuration. `OMP_PROFILE`, its precedence over `PI_PROFILE`, and `PI_CONFIG_DIR` must survive the actual capability environment filter. |
| `delegated` | The generated native plugin starts the exact host relay and descriptor/server pair. A real MCP initialization and `tools/list` precede a real `tools/call`. Default native `tools.xdev` remains enabled; the local model discovers and writes the mounted `xd://` route. |
| `background` | A real asynchronous native Bash job outlives the initial assistant response. The initial prompt is unsettled; the completion wakes another native turn and emits `session_settled`. The final answer differs from the earlier pending-work text. |
| `persistent` | A real 30-second asynchronous Bash job remains active at the initial prompt result. A deliberately short host deadline cancels the session. Inspection must report `omp_timed_out`, and native disposal must exit successfully. |
| `lifecycle` | Actual launch preparation injects the packaged extension. Native root start/turn/end/shutdown and optional samples pass through the packaged helper and actual classifier. The gate requires root model/provider, occupancy and cwd envelopes as well as hidden host-context delivery and immediate RPC shutdown. |
| `cwd` | The native `--cwd` argument selects a different directory from the host process working directory. Execution-context observations must report the actual native directory. |
| `initial-prompt` | The actual prepared invocation runs on a PTY, with the ordinary interactive mode. A separate passive public-event observer captures the first native user message; its exact text must equal the host request. One model turn follows, and native Ctrl+D owns shutdown. |
| `idle-model` | The same extension-loaded native process changes model and thinking through actual public RPC commands while idle. Native state and host-classified model observations must agree, with no additional model request. |
| `compaction` | Two real local turns precede a native compact command, using the documented soft method and a small retained-tail setting. A real installed compaction entry must match the public event and the classifier's context compaction ID. |
| `child`, `nested-child` | The local model calls the actual Task tool; children use a distinct configured model. Native starts, stops, returned results, direct-child serving model and child-state parent/outcome records are required. The nested case additionally requires a real grandchild and its observed parent/depth. A task-label model call cannot satisfy child execution. |
| `helper-custody` | The real helper starts a deliberately stalled wrapper. Closing helper stdin and, separately, allowing its own deadline to expire must both reap the wrapper before the helper exits. |
| `fallback-selected` | The local primary model returns HTTP 429 while native user configuration permits a fallback. An explicit task model selection must prevent every fallback request and return the original native refusal. |
| `fallback-preserved` | With task model selection omitted, the same native fallback configuration must remain effective: the native transition is observed, a fallback model request occurs, and the task succeeds. |

Each run creates new home, agent, native-cache, plugin and workspace
directories. The child environment is constructed explicitly and contains no
inherited account credentials. Model requests go to a loopback HTTP endpoint
with authentication disabled. The task relay and host wrapper are local
fixture processes. The harness makes no paid model requests and performs no
global OMP configuration changes.

`metadata.json` records the native versions and hashes of the harness,
driver, simulator, native entry point, Bun and every assembled package file.
`summary.json` records completed scenarios. Per-scenario evidence includes
SDK host requests and transcripts, the prepared process/artifacts, raw native
RPC frames, incremental inspection results and model requests. Delegation
adds the MCP relay journal; lifecycle and helper custody add wrapper journals.
All prompts, answers and identities in these files belong to the fixture.
The child and initial-prompt scenarios also attach a passive diagnostic native
extension that records public events. It never synthesizes callbacks, starts
work or changes native state. This separates native delivery from the package's
admission decisions. The initial-prompt gate compares the native user message,
because the provider transport may combine native date/context notices with
that message when forming its API request.
The PTY fixture disables the ordinary first-run setup wizard, startup update
check and splash through native `startup` settings. Its local provider is already
configured. This avoids waiting at the provider-setup screen; production
preparation does not alter these settings or fabricate an authenticated account.

## Qualification boundaries

The native CLI executes real production paths; the external model and host
services remain fixtures. Passing this gate does not qualify authenticated
provider accounts, a full Logonomic graphical host, Windows or macOS, every
native attention/subagent event, or native session persistence. Those require
their own platform and service acceptance evidence. Portable C++/SDK and
extension fixtures cover additional contract cases without claiming native
execution for them.

The initial Linux environment forbade the abstract Unix datagram socket used
by the official native file lock: `FileLock.tryAcquire` returned `Operation
not permitted (os error 1)`. Persisted native sessions consequently failed.
The acceptance harness uses the supported `--no-session` mode; it does not
patch, replace or bypass the lock. The native lock implementation is in
[linux.rs](https://github.com/can1357/oh-my-pi/blob/7a52f1e35eacc115b067c03a9c8ec7263e114cda/crates/pi-natives/src/file_lock/linux.rs).
Normal persistence and resume therefore remain outside this gate's evidence.

The native Task path is also affected: even with an in-memory root, OMP leases
a temporary artifact directory and opens a persisted child session. In this
workspace that child lock fails before its `started` event. The public bus
correctly reports preflight/failed records, which the add-on must not promote
to a running child. The extra small-model call is native task-label generation,
not a child turn. `child` and `nested-child` remain strict failing acceptance
requirements here; they are not skipped or reported as passed. Their full
success gate must run on a host that permits the official native lock. This
follows `task/structured-subagent.ts` artifact leasing and `task/executor.ts`
child-session creation in the recorded upstream revision.

The initial workspace also lacked source-package metadata for its dynamic
GCC runtime, which the SDK's Linux redistribution step requires. Its local
validation builds added
`-DCMAKE_EXE_LINKER_FLAGS='-static-libgcc -static-libstdc++'`. The production
build recipe is unchanged. That local configuration qualifies the package
contract and native add-on behavior; it does not validate the production
dynamic-runtime provenance path on a supported packaging workstation.

The asynchronous scenarios establish finite unsuppressed work and host
cancellation of persistent unsuppressed work. They do not independently
exercise OMP's suppressed, acknowledged or watched-job delivery exclusions.
The broader activation, cancellation and degraded-observation matrix also has
extension and adapter contract fixtures described in the native contract. Each
scenario's success is recorded separately; the presence of a scenario in this
file is not evidence that it passed on a particular host.
