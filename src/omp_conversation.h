#pragma once

#include <logonomic_cli_addon/conversation_export.h>

namespace omp {

/// Export the captured native JSONL session, without opening or resuming OMP.
std::optional<logonomic::cli_addon::Conversation_export_result> export_conversation(
    const logonomic::cli_addon::Conversation_export_request& request,
    const std::function<bool()>& cancelled,
    logonomic::cli_addon::Contract_failure* failure);

} // namespace omp
