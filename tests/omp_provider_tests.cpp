#include "omp_provider.h"

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
        throw std::runtime_error("Unexpected launch preparation failure: " + reason);
    }
    bool finished() const override { return m_result.has_value(); }
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
    request.declared_assets.clear();
    patch = prepare("", {});
    require(!patch.disposition->prepared, "A missing adopted asset capability cannot enable lifecycle promises");
}

} // namespace

int main(int, char** argv)
{
    try {
        recognition();
        launch_preparation(argv[0]);
        std::puts("PASS omp provider launcher contracts");
        return 0;
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
