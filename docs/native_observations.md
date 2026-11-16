# Native observations

The extension captures optional observations together with the native session
identity, activation nonce, and source sequence before queueing helper delivery.
The classifier admits those facts only for the matching activation, or jointly
with an accepted native session transition. It emits an optional SDK envelope
only when the host requests that capability. Invalid optional data leaves a valid
adjacent lifecycle fact usable; refusal of a context-only delivery does not
quarantine root lifecycle observation.

## Context occupancy and compactions

`context-state@2` samples `ctx.getContextUsage()` on native session activation,
agent start, assistant message end, terminal agent end, and successful compaction.
The native query reports the current model context, using provider anchors and
local estimates between them. Its `tokens` and `contextWindow` become
`used_tokens` and `context_window`; the host derives the bounded displayed
fraction. Cumulative usage or billing totals are never treated as occupancy.
An absent, invalid, or unavailable query explicitly clears the old measurement.
This cadence does not imply an update for each streamed token.

Only `session_compact` increments the observed successful-compaction lower bound.
The native emitter first appends the compaction entry, replaces live messages,
and rebases context statistics. Its stable entry ID identifies the observation
for SDK deduplication across redelivery. Compaction start, attempts, and
`auto_compaction_end` are not additional success evidence. The extension does not
claim that observed events cover a resumed conversation's earlier history, and
does not mix live successful events with an absolute history count. Compaction
summaries and conversation text are not included.

## Current execution directory

`execution-context@1` samples the exact `ctx.cwd` at the same native cadence.
The extension runner reads the owning session manager's live directory; it does
not use the process-global directory, launch argument, or a tool's per-command
working directory. Native `/move` changes are therefore visible in the next
sample. There is no dedicated public directory-change event, so this is not a
promise of immediate observation while idle.

An absolute local path is preserved byte for byte, including nested directories
and spaces. A missing or invalid path produces an attributed `unknown` update.
The source nonce and monotonically increasing sequence prevent delayed facts
from an earlier activation, including a clear that preserves the native UUID,
from replacing the current directory. This capability neither changes directories
nor authorizes an interactive resume recipe.

## Source evidence

The behavior above is traced to upstream oh-my-pi commit
`7a52f1e35eacc115b067c03a9c8ec7263e114cda`:

- `packages/coding-agent/src/extensibility/extensions/types.ts`:
  `ExtensionContext.getContextUsage` and `cwd`, public lifecycle hooks.
- `packages/coding-agent/src/session/session-stats.ts`:
  `getContextBreakdown` and `getContextUsage`, current occupancy and provider
  anchors rather than accumulated billing.
- `packages/coding-agent/src/session/session-maintenance.ts`:
  `#commitCompactionEntry` and snapcompact rescue commit, both emitting
  `session_compact` after installation.
- `packages/coding-agent/src/extensibility/extensions/runner.ts`:
  `cwd` getter and `createContext`, reading `sessionManager.getCwd()`.

The source revision records evidence; it is not an arbitrary minimum version.
The contract fixtures test source attribution and SDK admission. Actual native
acceptance is reported separately from fixture coverage.
