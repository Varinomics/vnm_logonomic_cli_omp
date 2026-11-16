# Native source profiles and accounts

`accounts@2` exposes OMP's filesystem source profiles. These are the native
default profile, named profiles, and an explicitly selected agent directory.
They are source identities for launch and historical-data custody. They do not
identify the provider principal who signed in, prove authentication, or report
remaining quota.

The adapter discovers at most 64 profiles and includes the exact selected
source even when it lies outside the default profile inventory. Existing named
profile directories are read from the native config and applicable XDG roots.
An overflow or inaccessible inventory fails explicitly; it never returns a
truncated list as though it were complete. Resolution selects the same stable
key that discovery supplies.

The key is a non-secret digest of the canonical native source path. The SDK's
`account_display_name(profile_path)` supplies the folder-based display name.
An existing directory is available, a non-directory source is unavailable, and
a native selectable directory that has not been created is unknown. These
statuses describe the filesystem source, independently of sign-in status.

## Native selection rules

The resolver reads only the SDK's frozen environment and supported literal
launcher. It applies these source-established rules:

- A real `--profile` argument overrides environment selection; the last one
  wins. A profile-looking scalar operand, such as the value of
  `--system-prompt`, remains data. Unsupported launcher grammars fail profile
  inspection without executing a shell.
- `OMP_PROFILE` takes precedence over `PI_PROFILE`, including an explicitly
  empty `OMP_PROFILE`. Empty or `default` selects the default profile. Native
  name syntax, whitespace normalization, and reserved device-name rejection
  are preserved.
- Named profiles use `<home>/<config>/profiles/<name>/agent` and ignore an
  unrelated `PI_CODING_AGENT_DIR`. Switching back to default restores a real
  agent-directory override, but does not turn an inherited profile-derived
  agent path into a new default override.
- `PI_CONFIG_DIR` changes the config root relative to the native platform home.
  Relative agent overrides use the frozen process directory. No path comes
  from the adapter process's home or ambient environment.
- On Linux/macOS, XDG data and state routing requires the native app directory
  to exist. A named profile requires its own XDG profile directory; an existing
  base `omp` directory alone cannot move it. An explicit nondefault agent
  directory does not inherit XDG routing.
- `--session-dir` overrides `PI_CODING_AGENT_SESSION_DIR`. Both are retained by
  the resolver for native history consumers, without changing account identity.

Native Bun/OMP dotenv loading can also change home, profile, agent, session or
XDG paths after the host freezes its environment. The adapter does not implement
a second dotenv interpreter. It inspects the relevant native dotenv locations
and refuses `native_profile_environment_unsupported` when a directory selector
is present, or its bounded inspection cannot establish the source. Ordinary
credential-only dotenv files remain allowed; their values are neither imported
into the adapter environment nor returned. Windows drive-relative paths are
also unavailable because the request does not contain native per-drive state.

## Selected-account acknowledgments

Interactive, structured and delegated preparation verify that the selected
account key belongs to the exact resolved native source. Version 2 additionally
checks the exact canonical profile path. A mismatch fails with
`account_context_mismatch` instead of echoing a different account. Successful
results preserve the requested account-context version and all fields.

This does not rewrite native credential storage, force a sign-in, or change the
launcher's model, approval policy or profile arguments. An operation without a
selected account does not require profile inspection merely to retain ordinary
launcher behavior. Conversation export continues to use its host-captured exact
session locator and account provenance.

The structured/delegated environment declaration remains within the SDK's
32-name bound. It now includes the documented native session-directory
selector. `BUN_INSTALL` is outside those two ambient declarations; an explicitly
authored launcher assignment can still use the SDK's admitted environment route.
Interactive preparation and discovery retain their existing `BUN_INSTALL` name.

## Evidence

Source behavior is pinned to upstream OMP commit
`7a52f1e35eacc115b067c03a9c8ec7263e114cda`:

- [`utils/src/dirs.ts`](https://github.com/can1357/oh-my-pi/blob/7a52f1e35eacc115b067c03a9c8ec7263e114cda/packages/utils/src/dirs.ts):
  `normalizeProfileName`, `resolveProfileEnv`, `resolvePreProfileAgentDir`,
  `setProfile`, `DirResolver`, and category-specific directories.
- [`utils/src/env.ts`](https://github.com/can1357/oh-my-pi/blob/7a52f1e35eacc115b067c03a9c8ec7263e114cda/packages/utils/src/env.ts):
  project/agent/config/home dotenv precedence and `refreshDirsFromEnv`.
- [`cli/profile-bootstrap.ts`](https://github.com/can1357/oh-my-pi/blob/7a52f1e35eacc115b067c03a9c8ec7263e114cda/packages/coding-agent/src/cli/profile-bootstrap.ts)
  and `cli/flag-tables.ts`: profile extraction and native operand boundaries.
- [`cli/args.ts`](https://github.com/can1357/oh-my-pi/blob/7a52f1e35eacc115b067c03a9c8ec7263e114cda/packages/coding-agent/src/cli/args.ts):
  the native ambient session-directory selector.

Portable tests exercise precedence, external directories, XDG existence checks,
exact versioned acknowledgments, invalid names, dotenv refusal, cancellation,
and inventory capacity. The separate native `profile` acceptance scenario also
invokes actual package account discovery and resolution, prepares the exact
version-2 source account, and completes a real local-model turn whose model
configuration exists only inside that selected native profile. It passed with
official OMP 18.20.2/Bun 1.4.4 on Linux; this does not qualify other platforms or
authenticated provider accounts.
