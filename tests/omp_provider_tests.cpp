#include "omp_provider.h"
#include "omp_launcher_approval.h"

#include <logonomic_cli_addon/launch_preparation.h>
#include <logonomic_cli_addon/launcher_utilities.h>
#include <logonomic_cli_addon/package_source.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
namespace addon = logonomic::cli_addon;

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void recognition()
{
    const auto direct = omp::recognize_launcher({"omp --model anthropic/claude-sonnet-4", "direct", ""},
        "linux-x86_64");
    require(direct && direct->program == "omp", "The native omp executable must be recognized");
    require(direct->arguments == std::vector<std::string>{"--model", "anthropic/claude-sonnet-4"},
        "Native options must retain their values");
    require(!omp::recognize_launcher({"pi", "direct", ""}, "linux-x86_64"),
        "The distinct Pi CLI must not be attributed to oh-my-pi");
    require(!omp::recognize_launcher({"omp && echo done", "bash", ""}, "linux-x86_64"),
        "A compound shell script must not be replaced as one literal invocation");
    require(!omp::recognize_launcher({"OMP", "direct", ""}, "linux-x86_64"),
        "POSIX executable recognition is case-sensitive");
    require(omp::recognize_launcher({"OMP.EXE", "direct", ""}, "windows-x86_64").has_value(),
        "Windows executable recognition is case-insensitive");
    require(omp::recognize_launcher({"omp.cmd --model fast", "cmd", ""}, "windows-x86_64").has_value(),
        "The npm command launcher must retain its Windows identity");
    require(omp::recognize_launcher({"& 'C:\\Tools\\omp.exe'", "powershell", ""}, "windows-x86_64").has_value(),
        "Literal PowerShell launcher paths must be recognized");

    const addon::Launcher_request launcher{"omp --model fast", "direct", ""};
    const std::vector<std::string> matching{"omp", "--model", "fast"};
    const std::vector<std::string> different{"omp", "--model", "other"};
    require(omp::recognize_launcher(launcher, "linux-x86_64", matching).has_value(),
        "Matching frozen host arguments must be accepted");
    require(!omp::recognize_launcher(launcher, "linux-x86_64", different),
        "Replacement preparation must reject mismatched frozen host arguments");

    const auto assigned = omp::recognize_launcher({"PI_CODING_AGENT_DIR='/tmp/omp profile' omp", "bash", ""},
        "linux-x86_64");
    require(assigned && assigned->environment_names == std::vector<std::string>{"PI_CODING_AGENT_DIR"},
        "Shell assignments must be obtained from the frozen environment instead of re-evaluated");
}

void launcher_approval()
{
    // Oracles: native cli/args.ts and cli/flag-tables.ts parse the bypass
    // aliases/modes; tools/approval.ts gives autoApprove precedence. The SDK
    // contract requires literal operand preservation and idempotent editing.
    const auto& rules = omp::launcher_approval_rules();
    const auto transform = [&](std::string script, addon::Launcher_approval_action action,
                               std::string runner = "bash", std::string platform = "linux-x86_64") {
        const addon::Launcher_approval_transform_request request{{std::move(script), std::move(runner),
            "OMP_PROFILE=selected\nNATIVE_TEXT=--yolo"}, std::move(platform), action};
        const auto result = addon::transform_launcher_approval(request, rules);
        require(addon::decode_launcher_approval_transform_result(
            addon::encode_launcher_approval_transform_result(result), nullptr).has_value(),
            "The approval edit must satisfy its public SDK wire contract");
        if (result.disposition != addon::Launcher_approval_disposition::UNSUPPORTED) {
            auto repeated = request;
            repeated.launcher.script_body = result.script_body;
            const auto second = addon::transform_launcher_approval(repeated, rules);
            require(second.disposition == addon::Launcher_approval_disposition::UNCHANGED &&
                second.script_body == result.script_body, "Approval transformations must be idempotent");
        }
        return result;
    };
    const auto yolofy = addon::Launcher_approval_action::YOLOFY;
    const auto unyolofy = addon::Launcher_approval_action::UNYOLOFY;
    auto result = transform("omp --approval-mode write --profile 'work profile' --model provider/model "
        "--system-prompt '--yolo' 'do this'", yolofy);
    require(result.disposition == addon::Launcher_approval_disposition::CHANGED,
        "A known native approval mode can be replaced with the canonical bypass");
    const auto invocation = addon::literal_launcher_invocation({result.script_body, "bash", ""}, "linux-x86_64");
    require(invocation && invocation->argv == std::vector<std::string>{"omp", "--yolo", "--profile", "work profile",
        "--model", "provider/model", "--system-prompt", "--yolo", "do this"},
        "Approval edits must preserve profile/model selection and flag-shaped prompt values");
    require(result.script_body.find("--profile 'work profile'") != std::string::npos,
        "Unrelated original shell spelling must survive the edit");

    result = transform("omp --auto-approve --approval-mode=yolo --yolo --approval-mode always-ask-cached -- ask", unyolofy);
    require(result.disposition == addon::Launcher_approval_disposition::CHANGED &&
        result.script_body == "omp --approval-mode always-ask-cached -- ask",
        "Unyolofy removes every native bypass but preserves an explicit non-bypass mode and prompt boundary");
    result = transform("omp --approval-mode write --config policy.yml", unyolofy);
    require(result.disposition == addon::Launcher_approval_disposition::UNCHANGED &&
        result.script_body == "omp --approval-mode write --config policy.yml",
        "Removing a bypass must not invent or remove a native configured approval policy");
    result = transform("omp -- --yolo", unyolofy);
    require(result.disposition == addon::Launcher_approval_disposition::UNCHANGED,
        "A flag-shaped positional prompt is data, not an invocation bypass");
    result = transform("omp --resume --yolo prompt", unyolofy);
    require(result.disposition == addon::Launcher_approval_disposition::UNSUPPORTED,
        "Removal cannot let an optional resume selector consume a formerly positional prompt");
    result = transform("& 'C:\\Tools\\omp.exe' --auto-approve --model native/exact", unyolofy,
        "powershell", "windows-x86_64");
    require(result.disposition == addon::Launcher_approval_disposition::CHANGED &&
        result.script_body == "& 'C:\\Tools\\omp.exe' --model native/exact",
        "The SDK must own PowerShell quoting and literal native argument transport");
    result = transform("omp.cmd --approval-mode=write", yolofy, "cmd", "windows-x86_64");
    require(result.disposition == addon::Launcher_approval_disposition::CHANGED,
        "Saved npm wrappers can have their literal approval flags edited without replacing the wrapper");
    for (const auto* script : {"omp config set tools.approvalMode yolo", "omp --approval-mode maybe",
             "omp --unknown-flag value", "omp --plan --yolo prompt", "omp --extension C:\\part --yolo file.ts",
             "omp --yolo=false", "omp --model", "omp && echo done"}) {
        require(transform(script, yolofy).disposition == addon::Launcher_approval_disposition::UNSUPPORTED,
            "Management, shadowable/unknown arities and ambiguous native option forms must remain unsupported");
    }

    const auto key = [&](std::string script, std::string environment = "OMP_PROFILE=selected") {
        return addon::launcher_approval_comparison_key({{std::move(script), "direct", std::move(environment)},
            "linux-x86_64"}, rules);
    };
    const auto plain = key("omp --model native/exact");
    require(plain.supported && key("omp --yolo --model native/exact").key == plain.key &&
        key("omp --approval-mode write --model native/exact").key == plain.key,
        "Rescan comparison may ignore only understood approval arguments");
    require(key("omp --model native/other").key != plain.key &&
        key("omp --model native/exact", "OMP_PROFILE=other").key != plain.key,
        "Model and profile/account environment changes must remain distinct launchers");

    // Native cli-commands.ts resolveCliArgv handles `help` before launch
    // fallback and rejects its reserved top-level words based on argv[0].
    // These are not all members of the native registered command table.
    for (const auto* word : {"help", "extensions", "list", "remove", "uninstall", "marketplace",
             "discover", "upgrade", "enable", "disable"}) {
        for (const auto& prefix : {std::string("omp "), std::string("omp --profile chosen "),
                 std::string("omp --yolo "), std::string("omp --approval-mode=yolo ")}) {
            const auto script = prefix + word;
            require(transform(script, yolofy).disposition == addon::Launcher_approval_disposition::UNSUPPORTED &&
                transform(script, unyolofy).disposition == addon::Launcher_approval_disposition::UNSUPPORTED &&
                !key(script).supported,
                "Approval changes and comparison must not conflate native command dispatch with prompt text");
        }
    }
    for (const auto* script : {"omp marketplace add example", "omp --yolo marketplace add example",
             "omp uninstall example@registry", "omp --auto-approve uninstall example@registry",
             "omp help models", "omp --yolo help models"}) {
        require(transform(script, yolofy).disposition == addon::Launcher_approval_disposition::UNSUPPORTED &&
            transform(script, unyolofy).disposition == addon::Launcher_approval_disposition::UNSUPPORTED,
            "The native multiword reserved-word guard and help dispatch must not be bypassed");
    }
    result = transform("omp launch list all files", yolofy);
    require(result.disposition == addon::Launcher_approval_disposition::CHANGED &&
        result.script_body == "omp launch --yolo list all files",
        "Explicit native launch keeps reserved-looking text in its prompt role");
    result = transform("omp --model help -- marketplace add example", yolofy);
    require(result.disposition == addon::Launcher_approval_disposition::CHANGED &&
        result.script_body == "omp --yolo --model help -- marketplace add example",
        "Protected option values and post-separator text remain data during approval editing");
}

class Invocation_fixture : public addon::Invocation_context
{
public:
    explicit Invocation_fixture(const addon::Prepare_request& request)
    {
        m_request.capability = {"launch-preparation", 3};
        m_request.operation = "prepare";
        m_request.payload = addon::encode_prepare_request(request);
    }

    const addon::Invoke_request& request() const override { return m_request; }
    bool cancel_requested() const override { return false; }
    bool wait_for_cancel(int timeout_ms) override
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
        return false;
    }
    void report_progress(std::uint64_t, std::optional<std::uint64_t>, std::string) override {}
    void succeed(addon::Json_value payload) override { m_result = std::move(payload); }
    void fail(addon::Result_status, std::string reason, std::string) override
    {
        m_failure = std::move(reason);
    }
    bool finished() const override { return m_result.has_value() || m_failure.has_value(); }
    const std::optional<std::string>& failure() const { return m_failure; }
    addon::Launch_patch result() const
    {
        require(m_result.has_value(), "Preparation must finish with a response");
        const auto patch = addon::decode_launch_patch(*m_result, nullptr);
        require(patch.has_value(), "Preparation output must satisfy the SDK launch patch grammar");
        return *patch;
    }

private:
    addon::Invoke_request m_request;
    std::optional<addon::Json_value> m_result;
    std::optional<std::string> m_failure;
};

class Temporary_program
{
public:
    explicit Temporary_program(const char* executable)
    {
        m_directory = std::filesystem::temp_directory_path() / ("omp launch test " + addon::random_nonce_hex());
        std::filesystem::create_directory(m_directory);
#ifdef _WIN32
        m_program = m_directory / "omp.exe";
#else
        m_program = m_directory / "omp";
#endif
        std::filesystem::copy_file(std::filesystem::absolute(executable), m_program);
    }
    ~Temporary_program()
    {
        std::error_code error;
        std::filesystem::remove_all(m_directory, error);
    }
    std::string program() const { return addon::path_to_utf8(m_program); }
    std::string directory() const { return addon::path_to_utf8(m_directory); }

private:
    std::filesystem::path m_directory;
    std::filesystem::path m_program;
};

void launch_preparation(const char* executable)
{
    const Temporary_program program(executable);
    addon::Prepare_request request;
    request.working_directory = program.directory();
    request.declared_assets = {"assets/logonomic.ts"};
#ifdef _WIN32
    const char* platform = "windows-x86_64";
#elif defined(__APPLE__)
    const char* platform = "macos-arm64";
#else
    const char* platform = "linux-x86_64";
#endif
    request.launch_environment = addon::Launch_environment{platform, addon::Json_value::object()};
    request.profile_storage = addon::Profile_storage_context{1, program.directory()};
    const auto prepare = [&](const std::string& script_tail, const std::vector<std::string>& tail) {
        request.launcher = {'"' + program.program() + '"' + script_tail, "direct", ""};
        request.launcher_argv = {program.program()};
        request.launcher_argv.insert(request.launcher_argv.end(), tail.begin(), tail.end());
        Invocation_fixture context(request);
        omp::handle_launch_preparation(context);
        auto result = context.result();
        require(addon::validate_launch_patch(result, request.declared_assets, {"assets/logonomic.ts"}, nullptr),
            "Prepared asset references and environment names must obey the SDK launch policy");
        return result;
    };
    auto patch = prepare(" --model native/exact --approval-mode write --no-session",
        {"--model", "native/exact", "--approval-mode", "write", "--no-session"});
    require(patch.disposition && patch.disposition->prepared && patch.disposition->lifecycle_events &&
        patch.disposition->coordination_delivery, "A verified native interactive launcher enables its complete extension");
    require(patch.argv && patch.argv->front() == program.program() && patch.argv->at(2) == "native/exact" &&
        patch.argv->at(4) == "write" && patch.argv->at(5) == "--no-session",
        "Native model, permission and persistence choices must remain byte-exact");
    require(patch.argv->at(patch.argv->size() - 2) == "--extension" &&
        patch.argv->back() == "{{asset:assets/logonomic.ts}}", "The native extension comes from a custodied asset token");
    require(patch.set.empty() && patch.unset.empty() && !patch.apply_managed_configuration,
        "Direct preparation must not edit credentials or global native configuration");
    require(patch.profile_storage && patch.profile_storage->root == program.directory(),
        "The host's profile context must retain its exact acknowledgment");

    patch = prepare(" --trusted-extension /saved/user.ts", {"--trusted-extension", "/saved/user.ts"});
    require(patch.argv && patch.argv->at(patch.argv->size() - 2) == "--trusted-extension",
        "An explicit native allowlist must remain an allowlist when adding the package module");
    patch = prepare(" --system-prompt --trusted-extension", {"--system-prompt", "--trusted-extension"});
    require(patch.argv && patch.argv->at(patch.argv->size() - 2) == "--extension",
        "Flag-shaped native values cannot silently change extension discovery policy");

    for (const auto& pair : {std::pair{" --mode rpc", std::vector<std::string>{"--mode", "rpc"}},
             {" --extension-flag unknown", {"--extension-flag", "unknown"}},
             {" --print", {"--print"}}, {" config list", {"config", "list"}},
             {" --model", {"--model"}}}) {
        patch = prepare(pair.first, pair.second);
        require(patch.disposition && !patch.disposition->prepared && !patch.argv && patch.set.empty(),
            "Unsupported or incomplete invocation shapes retain the unchanged ordinary launcher");
    }
    request.initial_prompt = addon::Initial_prompt_request{
        "Logonomic continuation: read the brief with café and 'quotes'; preserve $() literally.",
        program.directory() + "/brief.md"};
    patch = prepare(" --model native/exact --no-session", {"--model", "native/exact", "--no-session"});
    require(patch.argv && patch.argv->at(patch.argv->size() - 2) == "--" &&
        patch.argv->back() == request.initial_prompt->text &&
        addon::validate_initial_prompt_acknowledgment(request.initial_prompt, patch, nullptr),
        "A fresh literal launch must submit exactly one host prompt and acknowledge its byte digest");
    require(patch.set.empty() && patch.unset.empty(), "Initial prompt preparation cannot rewrite native policy or credentials");
    patch = prepare(" --system-prompt --resume", {"--system-prompt", "--resume"});
    require(patch.initial_prompt.has_value(), "A flag-shaped scalar value is not a session selector");
    for (const auto* text : {"@file is literal message text", "--flag is literal message text", "  Preserve edge spaces  "}) {
        request.initial_prompt->text = text;
        patch = prepare("", {});
        require(patch.argv->back() == text && addon::validate_initial_prompt_acknowledgment(request.initial_prompt, patch, nullptr),
            "The native end-of-options boundary must preserve flag/attachment-shaped text and spaces");
    }
    const auto unavailable = [&](const std::string& script_tail, const std::vector<std::string>& tail) {
        request.launcher = {'"' + program.program() + '"' + script_tail, "direct", ""};
        request.launcher_argv = {program.program()};
        request.launcher_argv.insert(request.launcher_argv.end(), tail.begin(), tail.end());
        Invocation_fixture context(request);
        omp::handle_launch_preparation(context);
        require(context.failure() == "initial_prompt_unavailable",
            "An uncarriable initial prompt must fail explicitly instead of falling back to an ordinary launch");
    };
    for (const auto* text : {"/command would be expanded", "use ^provider/model", "a^b is conservatively unclassified"}) {
        request.initial_prompt->text = text;
        unavailable("", {});
    }
    request.initial_prompt->text = "Read the exact continuation brief.";
    for (const auto& pair : {std::pair{" --continue", std::vector<std::string>{"--continue"}},
             {" --resume exact", {"--resume", "exact"}}, {" --session exact", {"--session", "exact"}},
             {" --restore exact", {"--restore", "exact"}}, {" --fork exact", {"--fork", "exact"}},
             {" --goal objective", {"--goal", "objective"}}, {" existing-prompt", {"existing-prompt"}},
             {" @file", {"@file"}}, {" --mode rpc", {"--mode", "rpc"}},
             {" --unknown flag", {"--unknown", "flag"}}, {" --model", {"--model"}}}) {
        unavailable(pair.first, pair.second);
    }
    std::string many_flags;
    const std::vector<std::string> too_many(63, "--no-title");
    for (const auto& argument : too_many) {
        many_flags += " " + argument;
    }
    unavailable(many_flags, too_many);
    request.declared_assets.clear();
    unavailable("", {});
    request.initial_prompt.reset();
    patch = prepare("", {});
    require(!patch.disposition->prepared, "A missing adopted asset capability cannot enable lifecycle promises");
}

} // namespace

int main(int, char** argv)
{
    try {
        recognition();
        launcher_approval();
        launch_preparation(argv[0]);
        std::puts("PASS omp provider launcher contracts");
        return 0;
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
