#pragma once

#include <logonomic_cli_addon/adapter_runtime.h>

#include <cstddef>
#include <cstdint>

namespace omp {

struct Usage_limits
{
    std::size_t source_entries = 2000000;
    std::size_t events = 1000000;
    std::uint64_t source_file_bytes = 256ULL * 1024 * 1024;
    std::uint64_t ledger_bytes = 512ULL * 1024 * 1024;
};

// Limits fail the scan explicitly; they never truncate accepted history.
void handle_usage(logonomic::cli_addon::Invocation_context& context, Usage_limits limits = {});

} // namespace omp
