#pragma once

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/launch_preparation.h>

namespace omp {

/// Adds a private, per-launch native bridge offer only to an already managed
/// launch. Unavailable wake never disables ordinary preparation.
void prepare_wake(
    const logonomic::cli_addon::Prepare_request& request,
    logonomic::cli_addon::Launch_patch& patch);

void handle_session_wake(logonomic::cli_addon::Invocation_context& context);

} // namespace omp
