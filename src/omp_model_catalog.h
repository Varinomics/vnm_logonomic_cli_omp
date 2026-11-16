#pragma once

#include <logonomic_cli_addon/helper_ingress.h>

namespace omp {

// Optional companion; callers first establish the ordinary root activation.
void append_native_catalog(
    const logonomic::cli_addon::Classify_event_request& request,
    logonomic::cli_addon::Classify_event_result& result);

} // namespace omp
