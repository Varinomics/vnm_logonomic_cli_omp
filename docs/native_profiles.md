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

The key is a non-secret digest of the canonical native source path. On Windows,
its input preserves case within case-sensitive directories. Ordinary Windows
directories retain OMP's historical ASCII-folded keys. The SDK's
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
- Native resume/session/restore and fork operands supply an exact history file
  only when they contain `/` or `\`, or end in `.jsonl`. Such files may be
  extensionless or use another suffix. Bare names, IDs and prefixes stay opaque
  selectors; the resolver does not search for same-named files. Relative paths
  use the selected startup `--cwd`. Fork selection precedes resume selection,
  and bare `--restore` preserves an already selected source. These are history
  locators; native continuation/fork behavior still prevents an exact-resume
  promise from a path alone.
- When launch cwd is the native home directory, OMP can automatically change
  to an accessible temporary directory. For relative history paths, the adapter
  inspects existing native candidates in order: `HOME/tmp`, then POSIX `/tmp`
  and `/var/tmp`. It checks directory accessibility without changing its own
  cwd or creating a directory. `--allow-home` or explicit `--cwd` suppresses
  that automatic selection. Only an unestablished permission state or the
  remaining runtime `os.tmpdir()` fallback is unavailable with
  `native_profile_working_directory_unsupported`. Absolute locators and ordinary
  source-profile inventory do not depend on this automatic selection.

Native Bun/OMP dotenv loading can also change home, profile, agent, session or
XDG paths after the host freezes its environment. The adapter does not implement
a second dotenv interpreter. It inspects the relevant native dotenv locations
and refuses `native_profile_environment_unsupported` when any directory-selector
spelling occurs in their bytes, or its bounded inspection cannot establish the
source. This deliberately includes selector names inside comments and quoted
values: native runtime grammar, including BOM prefixes, colon delimiters and
whitespace across lines, must not turn a partial parser into a wrong account
acknowledgment. OMP reads `.env` in the project, agent, config and home
directories. Bun also reads mode-specific dotenv files in its startup directory.
Accounts, usage and interactive preparation capture `NODE_ENV` to select them.
Structured and delegated sessions inspect the files for all three Bun modes
when their filtered launch environment leaves the inherited mode unknown.
Credential-only files without selector spellings remain allowed;
their values are neither imported into the adapter environment nor returned.
Windows drive-relative paths are
also unavailable because the request does not contain native per-drive state.

## Selected-account acknowledgments

Interactive, structured and delegated preparation verify that the selected
account key belongs to the exact resolved native source. Version 2 additionally
checks the retained profile path using native directory case rules. A Windows
grant with a historical key can still select the source identified by its
retained path when the containing directory's case policy can be established.
A Windows key-only grant whose key could have been shared by case-sensitive
siblings requires observable parent directories. A historical key shared by
siblings requires a new selection. A mismatched grant fails with
`account_context_mismatch`. Successful results preserve the
requested account-context version and all fields. Usage scans also verify that
the retained ledger belongs to the selected native source.

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
- [`main.ts`](https://github.com/can1357/oh-my-pi/blob/7a52f1e35eacc115b067c03a9c8ec7263e114cda/packages/coding-agent/src/main.ts)
  and `cli/startup-cwd.ts`: native session path-versus-selector routing and
  startup cwd application.

Portable tests exercise precedence, external directories, XDG existence checks,
exact versioned acknowledgments, invalid names, dotenv refusal, cancellation,
and inventory capacity. The separate native `profile` acceptance scenario also
invokes actual package account discovery and resolution, prepares the exact
version-2 source account, and completes a real local-model turn whose model
configuration exists only inside that selected native profile. It passed with
official OMP 18.20.2/Bun 1.4.4 on Linux; this does not qualify other platforms or
authenticated provider accounts.

`tests/native_profile_dotenv_acceptance.py` separately invokes the actual
packaged discovery, resolution and structured preparation paths while the
installed native `dirs.ts`/`env.ts` provide the dotenv oracle. It covers the
confirmed BOM/colon redirections, vertical whitespace and newline variants,
conservative comment/quote refusals, and preserved credential-only syntax.
It performs no model request or sign-in.

The separate exact-locator acceptance gate passed 18 packaged cases, including
seven installed native `applyStartupCwd` comparisons on Linux. It confirms HOME
relocation, CLI/ambient relative history directories, explicit `--cwd`,
`--allow-home`, inline empty `--cwd=`, and absolute-path independence. The strict
SDK direct grammar rejects a separately quoted empty argument, so the empty-cwd
fixture uses the supported inline spelling. Windows traversal and runtime temp
fallback behavior were not qualified by this Linux gate.
