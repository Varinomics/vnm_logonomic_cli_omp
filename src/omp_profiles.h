#pragma once

#include <logonomic_cli_addon/accounts.h>
#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/session_contracts.h>

#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>

namespace omp {

struct Native_profile
{
    std::string name;
    std::filesystem::path home;
    std::filesystem::path base_config;
    std::filesystem::path config;
    std::filesystem::path agent;
    std::filesystem::path data;
    std::filesystem::path state;
    std::filesystem::path source_profile;
    std::optional<std::filesystem::path> explicit_session_directory;
    std::optional<std::filesystem::path> explicit_session_file;
    std::string account_key;
};

// Resolves only the native paths justified by this frozen literal invocation.
// It performs read-only directory checks, never loads credentials or evaluates
// dotenv/extension code, and never substitutes the adapter's own environment.
std::optional<Native_profile> resolve_native_profile(
    const logonomic::cli_addon::Launcher_request& launcher,
    const logonomic::cli_addon::Launch_environment& environment,
    std::string_view normalization_directory,
    std::string_view default_home_fallback,
    std::optional<std::span<const std::string>> host_argv,
    const std::function<bool()>& cancelled,
    logonomic::cli_addon::Contract_failure* failure);

bool validate_native_account(
    const logonomic::cli_addon::Session_launch_context& request,
    const std::function<bool()>& cancelled,
    logonomic::cli_addon::Contract_failure* failure);

bool validate_native_account(
    const logonomic::cli_addon::Prepare_request& request,
    const std::function<bool()>& cancelled,
    logonomic::cli_addon::Contract_failure* failure);

void handle_accounts(logonomic::cli_addon::Invocation_context& context);

} // namespace omp
