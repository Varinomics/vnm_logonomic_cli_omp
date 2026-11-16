# Native historical usage accounting

`usage-scan@3` reads billable OMP entries into a retained event ledger. It is
independent of live model observations, authentication, remaining quota and the
provider principal who may have used a source profile. `accounts@2` identifies
the filesystem source; it does not assign historical tokens to a login identity.

## Counted native evidence

The scanner reads local version 3 JSONL sessions from the resolved native
profile's managed sessions directory, an explicit native session directory or
file, and the native `custom-session-files` registry. Exact CLI paths and
registry targets may be extensionless or have a custom suffix; their JSONL
content, rather than their filename, must satisfy the same native version and
entry checks. Recursive directory discovery remains limited to `.jsonl` files.
For `.jsonl` parents, the public native artifact-directory contract also admits
their child/advisor storage. Native compression scratch directories are excluded.

The CLI path-versus-selector boundary follows native `createSessionManager`:
an argument containing `/` or `\`, or ending in `.jsonl`, is a path. A bare
session name, ID or prefix remains a selector even if a same-named file exists.
Relative paths resolve after native startup `--cwd` selection; native fork
selection takes precedence over resume selection. This supplies historical
source locators, not an exact-resume or current-session identity guarantee.
If the launcher starts in native HOME, relative session paths follow native
automatic working-directory selection, unless `--allow-home` or `--cwd` fixes
it. Unestablished permission or runtime-fallback choices fail explicitly rather
than binding the source to a different transcript; see the profile rules below.

Each assistant `message.usage` and auxiliary `model_usage.usage` contributes
one event, across all saved branches. The authoritative amount is the native
`totalTokens` integer. Input, output, cache-read and cache-write fields retain
their own observed coverage. Missing dimensions stay unknown; missing or
malformed totals fail the scan rather than producing guessed bucket sums.
Native error/aborted calls with an explicit usage amount still count. Monetary
prices and Task tool-result usage summaries are not added: those summaries can
repeat amounts already present in child transcripts.

Forks copy native entry IDs and timestamps. The ledger keys a billed event by
its original entry ID and preserved **entry-envelope timestamp**, independent
of profile path, current session file or branch. The short entry ID alone is
insufficient. Copies must agree on provider, model, API, event kind and every
observed token amount. Two present response IDs must also agree. A missing
response ID is kept missing unless another actual copy supplies it; it is
never synthesized from a different identifier.

Native `discardEntryDurably` can replace a removed assistant with a new
`model_usage` entry. Its `discarded-turn` purpose, preserved original timestamp,
captured active leaf, and immediately following `discarded-entry-branch`
marker establish the original ID. The scanner requires that complete native
relationship before retaining the replacement under the original billed
identity. An unmatched or contradictory replacement fails explicitly.

The native stats database also uses entry/timestamp copy identities. Its
display/database timestamp may prefer `message.timestamp`; this ledger uses
the envelope timestamp because native discard preserves it while removing
the original message object. The emitted date is the UTC date on that native
envelope. No session or repository allocation is guessed from a copied entry
or a header whose working directory may have moved.

`usage-cross-source-event@1` carries the original native ID/timestamp pair in
the `oh-my-pi.entry-usage` authority domain. Each proved record contains the
complete authoritative amount of one native event, allowing the host to
recognize the same copied event in multiple source profiles. Conflicting
copies within a scan fail; the host owns admission and conflict policy across
independently selected source snapshots.

## Cache custody and paging

Only the cache selected in the host's request supplies retained evidence.
Its SDK manifest, exact inventory, file digests, account context, private
format and native source owner are validated before use. There is no latest
cache pointer or fallback to an unselected directory.

Every new scan re-reads the current native sources. Incremental and repair
requests merge them with the selected cache; reset requires no selected cache
and reconstructs current sources only. Missing current transcripts never
delete accepted events. Unknown dimensions may be refined by actual later
observations, but contradictory known amounts fail rather than being combined
with independent maxima.

The host allocates an empty, exclusive candidate beneath its acknowledged
profile storage. The adapter writes its ledger using the SDK stored accounting
codec, retains the native identity/provenance needed to replay it, and asks the
SDK to durably seal the exact inventory before returning page one. It never
mutates or reclaims a sealed candidate. Private stored records do not themselves
grant public observation authority: the native-event proof is validated again
when the page is built.

Pages remain under SDK byte/count bounds, carry at most 256 records, and use
strictly increasing stable record IDs. Every page preserves the exact account
and profile acknowledgment, source scope, sealed candidate and snapshot.
Only issued continuation cursors are accepted. Changed frozen requests and
expired in-process snapshots fail locally; previously accepted host data stays
selected. The adapter retains at most two bounded in-process snapshots.
Cancellation and progress reporting remain active during reads, merging and
sealing.

## Coverage and limits

Every completed scan reports `incomplete_historical_coverage:true`. Native
deletion, no-session runs, inaccessible remote history and missing child files
prevent a claim of full lifetime coverage. Gzip/database storage is not decoded.
Child locations belonging to extensionless or custom-suffix parents may remain
unobserved: public `artifactsDirectoryFor` and `collectSubSessions` provide no
child-root mapping for those names. The scanner does not use the private Task
implementation's unconditional six-character truncation to guess a directory;
that can escape the parent's adjacent artifact namespace for short filenames.
Symlink entries encountered during recursive discovery are not followed.
Partial trailing JSONL records are omitted from the frozen complete prefix;
malformed complete records and unsupported native versions fail explicitly.

These boundaries are distinct from an event's complete token amount. A scan
can retain a complete amount for each observed native call while lacking older
calls. In particular, omitted child transcripts do not justify adding Task
aggregate summaries, which might overlap other admitted child records.

The default resource limits are two million inspected directory/record entries,
one million billed events, 256 MiB per native source file, and 512 MiB of encoded
ledger/snapshot data. Exceeding a limit fails the capability; it never truncates
history or replaces accepted data with a smaller candidate. Ordinary source
inspection requires a supported literal launcher and the same native profile
resolution rules as [source-profile accounts](native_profiles.md).

## Source and verification

The source contract is pinned to upstream commit
`7a52f1e35eacc115b067c03a9c8ec7263e114cda`:

- [`session/session-manager.ts`](https://github.com/can1357/oh-my-pi/blob/7a52f1e35eacc115b067c03a9c8ec7263e114cda/packages/coding-agent/src/session/session-manager.ts):
  native billable entries, auxiliary calls, forks and discarded-turn replacement.
- [`session/session-entries.ts`](https://github.com/can1357/oh-my-pi/blob/7a52f1e35eacc115b067c03a9c8ec7263e114cda/packages/coding-agent/src/session/session-entries.ts)
  and `session-migrations.ts`: preserved envelope identity and per-session short IDs.
- [`session/session-paths.ts`](https://github.com/can1357/oh-my-pi/blob/7a52f1e35eacc115b067c03a9c8ec7263e114cda/packages/coding-agent/src/session/session-paths.ts):
  the exact custom-file registry.
- [`main.ts`](https://github.com/can1357/oh-my-pi/blob/7a52f1e35eacc115b067c03a9c8ec7263e114cda/packages/coding-agent/src/main.ts)
  and `cli/flag-tables.ts`: native exact-path versus session-selector grammar.
- [`session/sub-sessions.ts`](https://github.com/can1357/oh-my-pi/blob/7a52f1e35eacc115b067c03a9c8ec7263e114cda/packages/coding-agent/src/session/sub-sessions.ts):
  public child-artifact discovery for `.jsonl` parent names.
- [`stats/src/db.ts`](https://github.com/can1357/oh-my-pi/blob/7a52f1e35eacc115b067c03a9c8ec7263e114cda/packages/stats/src/db.ts)
  and `parser.ts`: copy deduplication, native counters and compression scratch exclusion.

Portable regression tests cover copies, child-versus-Task totals, auxiliary
calls, discard aliasing, retained history, reset, unknown-dimension refinement,
conflict rejection, corrupt caches, capacity, cancellation and bounded paging.
Exact-source regressions include extensionless/custom-suffix registry and CLI
paths, opaque ID/name selectors, startup cwd/fork precedence, malformed exact
files and refusal to infer child directories from a custom suffix.
`tests/native_usage_locator_acceptance.py` replays the unchanged native-authored
100-token source through the packaged adapter under those filename authorities.
It also invokes the installed public `applyStartupCwd` helper to compare HOME
relocation, relative session directories, `--allow-home`, explicit and empty
`--cwd`, and absolute-path controls. These checks make no model request and do
not depend on native persistent file locks.
`tests/native_usage_acceptance.py` additionally asks the official OMP
`SessionManager.inMemory` API to create a real branch copy, discard rewrite and
auxiliary usage entry, then serializes those exact returned entries through the
packaged adapter. It checks copy → rewrite → missing-source retention → reset
and validates the returned cache seals. Its token values are fixture inputs;
it makes no model request and does not qualify native file-lock persistence or
real account billing. No native lock is patched or disabled.
