#pragma once

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/session_contracts.h>

#include <optional>

namespace omp {

std::optional<logonomic::cli_addon::Structured_prepare_result> prepare_structured_session(
    const logonomic::cli_addon::Structured_prepare_request& request,
    logonomic::cli_addon::Contract_failure* failure);

std::optional<logonomic::cli_addon::Delegated_prepare_result> prepare_delegated_session(
    const logonomic::cli_addon::Delegated_prepare_request& request,
    logonomic::cli_addon::Contract_failure* failure);

logonomic::cli_addon::Structured_inspect_result inspect_rpc(
    const logonomic::cli_addon::Structured_inspect_request& request);

void handle_structured_session(logonomic::cli_addon::Invocation_context& context);
void handle_delegated_session(logonomic::cli_addon::Invocation_context& context);

} // namespace omp
