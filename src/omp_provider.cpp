#include "omp_provider.h"

#include <logonomic_cli_addon/launch_preparation.h>
#include <logonomic_cli_addon/launcher_utilities.h>

#include <algorithm>
#include <utility>

namespace omp {
namespace addon = logonomic::cli_addon;

std::optional<Launcher_command> recognize_launcher(
    const addon::Launcher_request& launcher,
    std::string_view platform_id,
    std::optional<std::span<const std::string>> host_argv)
{
    const auto invocation = addon::literal_launcher_invocation(launcher, platform_id, host_argv);
    if (!invocation) {
        return std::nullopt;
    }
    const bool windows = platform_id.starts_with("windows-");
    const auto& program = invocation->argv.front();
    const auto separator = program.find_last_of(windows ? "/\\" : "/");
    auto name = program.substr(separator == std::string::npos ? 0 : separator + 1);
    if (windows) {
        name = addon::lower(std::move(name));
    }
    if (name != "omp" && (!windows || (name != "omp.exe" && name != "omp.cmd" && name != "omp.ps1"))) {
        return std::nullopt;
    }
    Launcher_command result;
    result.program = program;
    result.arguments.assign(invocation->argv.begin() + 1, invocation->argv.end());
    result.environment_names = invocation->environment_names;
    return result;
}

void handle_discovery(addon::Invocation_context& context)
{
    addon::Contract_failure failure;
    if (context.request().operation == "discover") {
        const auto request = addon::decode_discover_request(context.request().payload, &failure);
        if (!request) {
            context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
            return;
        }
        const auto environment = addon::environment_pairs(request->ambient_environment);
        const auto program = addon::resolve_program("omp", environment, {});
        std::vector<addon::Launcher_candidate> candidates;
        if (program) {
            const auto runner = addon::preferred_discovery_runner(*program,
                request->platform_id.starts_with("windows-") ? addon::k_runner_powershell : addon::k_runner_direct);
            candidates.push_back({
                "interactive", "oh-my-pi", "omp", runner, "", true, addon::Json_value::object()});
        }
        context.succeed(addon::encode_candidates(candidates));
    }
    else
    if (context.request().operation == "recognize") {
        const auto request = addon::decode_recognize_request(context.request().payload, &failure);
        if (!request) {
            context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
            return;
        }
        addon::Launcher_recognition result;
        result.recognized = recognize_launcher(request->launcher, request->platform_id).has_value();
        if (result.recognized) {
            result.display_name = "oh-my-pi";
        }
        context.succeed(addon::encode_recognition(result));
    }
    else {
        context.reject_unknown_operation();
    }
}

void handle_launch_preparation(addon::Invocation_context& context)
{
    if (context.request().operation != "prepare") {
        context.reject_unknown_operation();
        return;
    }
    addon::Contract_failure failure;
    const auto request = addon::decode_prepare_request(context.request().payload, &failure);
    if (!request) {
        context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
        return;
    }
    addon::Launch_patch patch;
    patch.disposition     = addon::Launch_disposition{false, false, false};
    patch.profile_storage = request->profile_storage;
    patch.account_context = request->account_context;
    context.succeed(addon::encode_launch_patch(patch));
}

} // namespace omp
