# Cooperative session descriptions

The managed extension registers the public native tool
`logonomic_describe_session`. A model can use it to return a requested session
title, record a concise Markdown working summary, or explicitly keep a reviewed
summary. Registration uses OMP's normal tool mechanism, `write` approval tier,
and active-tool selection. The extension does not enable a tool excluded by the
user's native policy. Native bindings without the public registration/schema
API leave this optional route unavailable.

The tool becomes usable after an accepted root activation negotiates
`cooperative-description@1`. Existing `model-context-delivery@2` continues to
carry the host's exact title and summary instructions. When the tool is active,
the extension adds a short routing sentence identifying it as the return path.
Neither registration nor a title instruction creates an automatic submission.
Child bindings cannot submit a description for the root.

| Kind | Model-supplied fields | Existing host policy |
|---|---|---|
| `title` | Required `title` | Suggests the automatic title; an assigned title retains its existing policy. |
| `summary` | Required `summary`, optional `title` | Records the working summary and may revise the automatic title. |
| `summary_unchanged` | Optional `title` | Reviews the existing summary without replacing its text; absence of a recorded summary is a refusal. |

The host keeps manual-title precedence and owns normalization, title length,
summary word limits and persistence. Native conversation text, compaction
summaries and automatically generated OMP titles are not scraped into this
route. The submission contains only text deliberately supplied by the model's
tool call.

## Activation, delivery, and receipts

The extension captures the native root identity, activation object and tool-call
ID before awaiting any helper work. The model cannot supply those identities.
The classifier checks the current native activation, and the host independently
checks its live package generation, native process generation, provider epoch,
session ID and transition nonce before applying its existing description policy.
The host retains this guard through synchronous title and summary publication
callbacks. A clear that preserves the native UUID still retires the old nonce.

Transport uses the existing SDK `Helper_client` and provider-event journal. A
description is an independent ignored classification with a typed optional
submission, without a fabricated lifecycle observation. Its deterministic
journal identity belongs to the exact native activation and tool invocation;
an unknown transport retry keeps that same identity. The helper remains
responsible for cancellation and wrapper reaping.

Only the host produces the typed description receipt. The extension checks its
version and exact request ID. A carrier's accepted flag alone does not prove
that the title or summary was recorded. A refusal retains the host's reason;
a missing receipt or uncertain delivery is reported as uncertain and is not
automatically retried. Native receipt retention is bounded to 128 tool requests
per activation without evicting unknown outcomes. At most four descriptions may
wait in the helper queue, separately from lifecycle and optional sample limits.

Title transport is bounded to 1 KiB, summary text to 16 KiB, and the encoded
submission to 24 KiB. The host may refuse a valid transport payload under its
existing policy. Description refusal, absence, or cancellation does not prevent
an independently observed native `agent_end` or session shutdown.

## Other lifecycle policy boundaries

OMP's public awaited `session_stop` hook can request continuation before the
later terminal event. The current authenticated host SDK route supplies title,
summary and context guidance, but no host completion decision. Its
`instruction_required` flag concerns title guidance. The add-on therefore keeps
host-blockable completion undeclared; it does not reinterpret that flag as a
stop decision.

The lifecycle observation contract carries native turn identity but no confirmed
host turn-origin record. An idle wake notification is bound to its target
activation, yet its public native submission result does not establish which
later turn started if user input competes with it. Turn-origin correlation
remains undeclared. A native description tool-call ID identifies only the
description submission and does not fill that separate authority gap.

## Source and validation

The native tool contract is traced to oh-my-pi
`7a52f1e35eacc115b067c03a9c8ec7263e114cda`,
`packages/coding-agent/src/extensibility/extensions/types.ts` (`ToolDefinition`,
`ExtensionAPI.registerTool`, `getActiveTools`), and the runner's public tool
execution context. This source revision is provenance, not an arbitrary minimum
version.

The host return path uses `cooperative-description@1` and delegates to the
existing title/summary methods in
`src/logonomic_session_service_sessions_remote_control.cpp`. Codec, classifier,
native tool and host activation-guard fixtures verify their respective
boundaries.

`tests/native_description_acceptance.py` separately exercises the official OMP
runtime with the assembled package, a local model and a fixture host receiver.
The verified OMP 18.20.2 run used native `write` approval policy, executed one
description tool call, and returned its accepted typed receipt in the second
real model request. Captured native events also produced one exact typed
submission through the packaged classifier, followed by native completion and
clean shutdown. The test records the native executable, package and harness
hashes alongside its request/receipt evidence.

The focused host gate compiles the production owner-admission kernel. It does
not build the complete Session-service application; the consumer translation
unit and delegation to the existing policies require source review. A native
tool run supplied with a simulated host receipt proves the native producer,
helper and receipt-return path, not execution of the production host policy.
