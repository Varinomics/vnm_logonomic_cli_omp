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

The native CLI keeps its own models, credentials and settings. Each package
declares its supported features in `package/capabilities`; Logonomic adopts
only capabilities also supported by its host and the running adapter.

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

Package validation and conformance exercise the SDK boundary. Native provider
behavior requires oh-my-pi itself. The inspected upstream contract is recorded
in `package/provenance/upstream_contract.json`; its version is evidence, not a
maximum supported version.

The add-on builds for Windows and Linux x86-64 and macOS ARM64. A Logonomic
build can also add this project with `LOGONOMIC_CLI_ADDON_EMBEDDED` after adding
the SDK; the project then registers its package without installing files or
adding standalone tests to the host build.

