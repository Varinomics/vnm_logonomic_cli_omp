#pragma once

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/launcher_discovery.h>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace omp {

struct Launcher_command
{
    std::string              program;
    std::vector<std::string> arguments;
    std::vector<std::string> environment_names;
};

/// Only literal native invocations are adapted; the SDK owns shell interpretation.
std::optional<Launcher_command> recognize_launcher(
    const logonomic::cli_addon::Launcher_request& launcher,
    std::string_view platform_id,
    std::optional<std::span<const std::string>> host_argv = std::nullopt);

void handle_discovery(logonomic::cli_addon::Invocation_context& context);
void handle_launch_preparation(logonomic::cli_addon::Invocation_context& context);

} // namespace omp
