# Available models from the running profile

`model-catalog@1` reports the configured model set from a live root session's
public `ctx.models.list()` query. The host retains it under that exact process,
native session and activation, and shows an expandable **Configured models**
list in the existing Session details dialog. This is separate from the active
provider/model/reasoning observation and delegated model selection.

The native query uses the running session's registry, including its selected
profile, disabled-provider settings, configured credential sources and keyless
providers. It does not authenticate a model request or establish billing,
quota, or access. Native explicit model resolution uses this registry set.
The native RPC `get_available_models` additionally applies the session's
`enabledModels` picker filter, so the catalog is not an exact copy of that
picker. This capability supplies a live session inventory, not a prelaunch
picker or an account-discovery result.

## Capture and transport

The host negotiates the optional capability and the accepted activation reply
enables sampling. The extension awaits existing native background refresh
outside lifecycle delivery, retaining the owner captured before that await.
Retired activations are discarded before capture and dispatch. One refresh
may be pending per owner. Samples run after activation, at agent-loop and
fallback boundaries, and every 15 managed idle ticks. This is sampled
availability; it does not promise immediate observation of every native
configuration change.

The projection carries only provider/model identities, an optional display
name and an optional boolean describing native reasoning capability. Missing
reasoning remains unknown. It never copies native API keys, headers, endpoint
URLs, prices or private error text. An observed empty set remains an available
catalog of zero models. An unsupported or failed query reports unavailable.
The producer rejects ambiguous duplicate identities and invalid native text.

The native list is sorted by provider/model. A snapshot includes at most 128
rows and 24 KiB of sanitized JSON, leaving room inside the helper's existing
32 KiB input-line limit. The native total and `truncated` flag are always
retained. Large inventories therefore show a deterministic prefix with an
explicit shown-versus-total count; they never appear complete. Inputs above
16,384 native rows are unavailable rather than consuming unbounded memory.

Unchanged snapshots are suppressed after an accepted delivery, and queued
catalog work is replaceable optional telemetry. It neither consumes the
lifecycle queue budget nor invents a lifecycle transition. The adapter emits
an independently negotiated snapshot only for the matching root activation.
Malformed optional data cannot discard adjacent valid lifecycle facts. The
host clears stale inventory on replacement or observation loss and retains
source-order barriers against replay.

## Native authority and validation

The inspected native source is oh-my-pi 18.20.2, commit
`7a52f1e35eacc115b067c03a9c8ec7263e114cda`:

- [`extensions/types.ts`](https://github.com/can1357/oh-my-pi/blob/7a52f1e35eacc115b067c03a9c8ec7263e114cda/packages/coding-agent/src/extensibility/extensions/types.ts) exposes the public query and registry.
- [`extensions/model-api.ts`](https://github.com/can1357/oh-my-pi/blob/7a52f1e35eacc115b067c03a9c8ec7263e114cda/packages/coding-agent/src/extensibility/extensions/model-api.ts) implements `list()` through the registry's available set.
- [`config/model-registry.ts`](https://github.com/can1357/oh-my-pi/blob/7a52f1e35eacc115b067c03a9c8ec7263e114cda/packages/coding-agent/src/config/model-registry.ts) owns configured availability and background refresh.
- [`session/model-controls.ts`](https://github.com/can1357/oh-my-pi/blob/7a52f1e35eacc115b067c03a9c8ec7263e114cda/packages/coding-agent/src/session/model-controls.ts) applies the additional native picker filter.

Portable projection, activation, codec and host reducer tests cover optional
negotiation, replay, retirement, partiality, empty and unavailable states.
The host test loads the actual QML component and checks expansion, plain-text
names, displayed partial counts and reasoning false versus unknown.

The opt-in native acceptance uses official OMP and Bun in an isolated profile,
with local keyless model metadata and no prompt or model request:

```sh
python tests/native_catalog_acceptance.py \
  --runtime /path/to/official-omp-and-bun-installation \
  --artifacts /fresh/native-catalog-evidence \
  --package /path/to/built/package/logonomic.cli.omp \
  --host-driver /path/to/build/omp_native_host_driver
```

The package form exercises the actual native extension loader, packaged helper
and SDK Host simulator's packaged adapter. A fixture supplies the host wrapper
transport and ledger; it does not fabricate the native model list or adapter
classification. Omitting the last two options tests the production sampler
alone. The gate is distinct from portable fixtures and does not establish a
complete desktop launch or Windows/macOS runtime qualification.
