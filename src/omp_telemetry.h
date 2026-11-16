#pragma once

#include <logonomic_cli_addon/helper_ingress.h>

namespace omp {

/// Adds independently negotiated observations after the caller admits the
/// native source activation. Malformed optional facts never reject lifecycle.
void append_native_telemetry(
    const logonomic::cli_addon::Classify_event_request& request,
    logonomic::cli_addon::Classify_event_result& result);

} // namespace omp
