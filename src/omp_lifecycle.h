#pragma once

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/helper_ingress.h>

namespace omp {

/// Classifies the bounded facts captured by the native extension. The host
/// ledger, rather than adapter process state, owns activation correlation.
logonomic::cli_addon::Attention_classification_result classify_native_event(
    const logonomic::cli_addon::Classify_event_request& request);

void handle_lifecycle(logonomic::cli_addon::Invocation_context& context);

} // namespace omp
