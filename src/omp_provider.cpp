#include "omp_provider.h"
#include "omp_wake.h"

#include <logonomic_cli_addon/launch_preparation.h>
#include <logonomic_cli_addon/launcher_utilities.h>
#include <logonomic_cli_addon/package_source.h>

#include <algorithm>
#include <utility>

namespace omp {
namespace addon = logonomic::cli_addon;

namespace {

constexpr std::string_view extension_asset = "assets/logonomic.ts";

bool one_of(std::string_view option, std::initializer_list<std::string_view> options)
{
    return std::find(options.begin(), options.end(), option) != options.end();
}

std::optional<bool> interactive_flags(std::span<const std::string> arguments, bool initial_prompt = false)
{
    bool trusted = false;
    bool ordinary_extension = false;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const std::string_view argument = arguments[index];
        const auto equals = argument.find('=');
        const auto option = argument.substr(0, equals);
        if (initial_prompt && one_of(option,
                {"--continue", "-c", "--resume", "-r", "--session", "--restore", "--fork", "--goal"})) {
            return std::nullopt;
        }
        if (one_of(option, {"--provider", "--model", "--thinking", "--approval-mode", "--profile", "--cwd", "--add-dir",
                "--config", "--tools", "--skills", "--system-prompt",
                "--append-system-prompt", "--system-prompt-template", "--extension", "-e", "--hook",
                "--trusted-extension", "--session-dir", "--fork", "--plugin-dir", "--api-key", "--goal",
                "--smol", "--slow", "--max-time", "--max-cost", "--warn-cost", "--service-tier"})) {
            trusted = trusted || option == "--trusted-extension";
            ordinary_extension = ordinary_extension || one_of(option, {"--extension", "-e", "--hook"});
            if (equals == std::string_view::npos && ++index >= arguments.size()) {
                return std::nullopt;
            }
        }
        else
        if (one_of(option, {"--resume", "-r", "--session", "--restore"})) {
            if (equals == std::string_view::npos && index + 1 < arguments.size() &&
                !arguments[index + 1].starts_with('-')) {
                ++index;
            }
        }
        else
        if (one_of(option, {"--continue", "-c", "--no-extensions", "--no-session", "--no-tools", "--no-skills",
                "--no-rules", "--no-context-files", "--no-lsp", "--no-title", "--allow-home", "--offline",
                "--hide-thinking", "--auto-approve", "--yolo"})) {
            if (equals != std::string_view::npos) {
                return std::nullopt;
            }
        }
        else {
            return std::nullopt;
        }
    }
    return trusted && ordinary_extension ? std::nullopt : std::optional<bool>(trusted);
}

} // namespace

bool interactive_arguments(std::span<const std::string> arguments)
{
    return interactive_flags(arguments).has_value();
}

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
    const auto unchanged = [&] {
        if (request->initial_prompt) {
            context.fail(addon::Result_status::UNAVAILABLE, "initial_prompt_unavailable",
                "This native invocation cannot carry an unchanged first message into a fresh interactive session.");
        }
        else {
            context.succeed(addon::encode_launch_patch(patch));
        }
    };
    if (!request->launch_environment ||
        std::find(request->declared_assets.begin(), request->declared_assets.end(), extension_asset) == request->declared_assets.end()) {
        unchanged();
        return;
    }
    const auto& launch_environment = *request->launch_environment;
    const auto launcher = recognize_launcher(request->launcher, launch_environment.platform_id, request->launcher_argv);
    const auto flags = launcher ? interactive_flags(launcher->arguments, request->initial_prompt.has_value()) : std::nullopt;
    if (!flags) {
        unchanged();
        return;
    }
    // AgentSession expands slash commands/templates and ^model mentions before
    // constructing a user message. A host-authored first message must stay literal.
    if (request->initial_prompt && (request->initial_prompt->text.starts_with('/') ||
            request->initial_prompt->text.find('^') != std::string::npos)) {
        unchanged();
        return;
    }
    const auto environment = addon::environment_pairs(launch_environment.effective_environment);
    const auto program = addon::resolve_program(launcher->program, environment, request->working_directory);
    if (!program || (launch_environment.platform_id.starts_with("windows-") && !addon::native_image(*program))) {
        unchanged();
        return;
    }
    for (const auto& name : launcher->environment_names) {
        const auto value = std::find_if(environment.begin(), environment.end(), [&](const auto& entry) {
            return launch_environment.platform_id.starts_with("windows-") ?
                addon::lower(entry.first) == addon::lower(name) : entry.first == name;
        });
        if (value == environment.end()) {
            unchanged();
            return;
        }
    }
    const bool trusted = *flags;
    std::vector<std::string> arguments{addon::path_to_utf8(*program)};
    arguments.insert(arguments.end(), launcher->arguments.begin(), launcher->arguments.end());
    arguments.insert(arguments.end(), {trusted ? "--trusted-extension" : "--extension", "{{asset:assets/logonomic.ts}}"});
    if (request->initial_prompt) {
        // Native parseArgs treats every argument after -- as one literal
        // message, including @file and flag-shaped text. main.ts submits each
        // argv message independently; keep the complete host text in one entry.
        arguments.insert(arguments.end(), {"--", request->initial_prompt->text});
        patch.initial_prompt = addon::Initial_prompt_acknowledgment{
            addon::initial_prompt_text_digest_hex(request->initial_prompt->text)};
    }
    patch.argv = std::move(arguments);
    for (const auto& name : launcher->environment_names) {
        const auto value = std::find_if(environment.begin(), environment.end(), [&](const auto& entry) {
            return launch_environment.platform_id.starts_with("windows-") ?
                addon::lower(entry.first) == addon::lower(name) : entry.first == name;
        });
        patch.set.emplace_back(name, value->second);
    }
    patch.disposition = addon::Launch_disposition{true, true, true};
    if (request->initial_prompt && !addon::validate_launch_patch(patch, request->declared_assets,
            {std::string(extension_asset)}, &failure)) {
        unchanged();
        return;
    }
    prepare_wake(*request, patch);
    context.succeed(addon::encode_launch_patch(patch));
}

} // namespace omp
