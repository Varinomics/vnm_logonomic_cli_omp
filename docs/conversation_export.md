# Conversation export

The optional `conversation-export@1` handler reads the exact native file captured
when the source conversation activated. The record must contain the requested
`provider_session_id` and an absolute `session_file`. The file's native version 3
session header must have that same ID. A missing locator, unreadable source,
different ID, unsupported version, or unsupported storage format cannot select
another conversation. Export never starts OMP, follows a continuation, reads
terminal scrollback, or changes native history.

Supported sources are plain local native v3 JSONL files, including extensionless
and custom-suffix filenames. OMP accepts an exact path such as
`--session .omp-sessions/work`; the activation captures its absolute path.
Export validates the file's content and native identity instead of requiring
the conventional `.jsonl` suffix. It does not search adjacent files or append a
suffix when that captured source is missing.

Native `--no-session` launches have no persisted source to export. Compressed
child histories, SQL/Redis stores, and older native session versions are
unavailable through this reader. A different filename does not make compressed,
database or malformed bytes valid JSONL. Native resume support is independent
of export: an exact readable file can be exported even when native resume would
choose a continuation.

## Native ordering and content

The SDK reader freezes the open file's initial byte length. Only complete JSONL
records within that prefix are consumed; an unfinished last line is omitted and
reported in the result. A malformed complete record fails the export. A native
fixed title slot may precede the session header.

OMP records a graph, so physical file order is insufficient. The reader rebuilds
the native entry index, replaces repeated IDs as the native index does, and
selects the final active leaf. A late usage/background record's `activeLeafId`
preserves the branch that was active when it was appended; explicit null means
no selected branch. A dangling captured leaf falls back to the last record that
selected itself, matching native replay. The selected parent chain is emitted
oldest first. An incomplete or cyclic selected chain fails explicitly. Other
branches are excluded. `/clear` records a reset marker but retains earlier
transcript history, so full export preserves that earlier discussion.

Participant text, code and Unicode are preserved. Native assistant phases are
only classified when `stopReason` establishes them: `stop` is final and
`toolUse` is commentary. Other assistant text retains an unknown phase. Full
exports include recorded system prompts, plaintext reasoning, tool calls and
results, visible custom messages, user shell/Python executions, branch/compaction
summaries and reset markers. Internal state, usage, credential hashes, labels,
model configuration records and provider replay metadata are not participant
messages. Unknown message/content kinds fail instead of being silently dropped.

Images and referenced files have explicit attachment markers. Export never
fetches an attachment or copies binary data. It retains recorded file-mention
text in the full projection. Encrypted reasoning has one visible unavailable
placeholder per native message; opaque replay ciphertext is not emitted.

Compact export retains user discussion, final or unclassified assistant text,
user-visible custom messages, attachment markers and encrypted placeholders.
Known commentary, reasoning, tools and internal records are excluded. Native
agent-attributed developer instructions (including persisted plan reminders) are
internal; user-attributed or unclassified developer text remains visible. The shared
SDK writer supplies the three formats (`text`, `markdown`, `conversation`),
literal tool fences, control-character escaping, cancellation and staged output.
The capability result contains the fixed filename and counts, never the
conversation body, and echoes the historical source's account/profile context.

## Evidence and boundaries

The source oracle is OMP commit
[`7a52f1e35eacc115b067c03a9c8ec7263e114cda`](https://github.com/can1357/oh-my-pi/tree/7a52f1e35eacc115b067c03a9c8ec7263e114cda):

- [`session/session-entries.ts`](https://github.com/can1357/oh-my-pi/blob/7a52f1e35eacc115b067c03a9c8ec7263e114cda/packages/coding-agent/src/session/session-entries.ts): version, title/header, entry graph, active-leaf semantics and reset history.
- [`session/session-manager.ts`](https://github.com/can1357/oh-my-pi/blob/7a52f1e35eacc115b067c03a9c8ec7263e114cda/packages/coding-agent/src/session/session-manager.ts): `SessionEntryIndex` replacement, branch walking and native persistence.
- [`session/session-loader.ts`](https://github.com/can1357/oh-my-pi/blob/7a52f1e35eacc115b067c03a9c8ec7263e114cda/packages/coding-agent/src/session/session-loader.ts) and [`session/session-storage.ts`](https://github.com/can1357/oh-my-pi/blob/7a52f1e35eacc115b067c03a9c8ec7263e114cda/packages/coding-agent/src/session/session-storage.ts): exact custom file paths and raw file-backed JSONL loading, without a suffix requirement.
- [`pi-ai types`](https://github.com/can1357/oh-my-pi/blob/7a52f1e35eacc115b067c03a9c8ec7263e114cda/packages/ai/src/types.ts) and [`TUI messages`](https://github.com/can1357/oh-my-pi/blob/7a52f1e35eacc115b067c03a9c8ec7263e114cda/packages/tui/src/chat/messages.ts): native text, reasoning, tools, images and custom message shapes.

`omp_conversation_tests` checks these native shapes against the real SDK reader
and writer, including branch changes, duplicate IDs, late usage, clear boundaries,
all formats and projections, partial tails, large records, cancellation and
foreign identities. These are source-grounded fixtures. They do not claim that
this workspace ran a persisted native conversation: its native file-lock route
is denied by the environment. The separate live root acceptance uses
`--no-session` and cannot establish persisted-history writing behavior.

`tests/native_export_source_acceptance.py` additionally passes an actual native
in-memory `SessionManager` capture through the packaged adapter and SDK host
driver. It materializes the captured before/fork/discard records as isolated
fixtures and compares extensionless and custom-suffix exports with a `.jsonl`
control. All nine exports produced identical content and descriptors for their
matching native records, and all sources remained unchanged. Six negative
cases refused gzip, database bytes, a malformed complete record, a foreign ID,
an unknown version, and a missing exact file with a valid `.jsonl` neighbour.
This establishes the export route using real native record shapes; the test
does not exercise native persisted-session writing.

The opt-in gate records input, package and driver hashes, requests and outcomes:

```sh
python3 tests/native_export_source_acceptance.py \
  --native-entries /absolute/native-session-manager-capture.json \
  --package /absolute/build/package/logonomic.cli.omp \
  --host-driver /absolute/build/omp_native_host_driver \
  --artifacts /absolute/new-evidence-directory
```

The capture must contain `before`, `fork` and `after` objects with the actual
native `header` and `entries`; this gate never fabricates a native ownership or
resume acknowledgment from those fixtures.
