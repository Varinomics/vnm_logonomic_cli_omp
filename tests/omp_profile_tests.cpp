#include "omp_profiles.h"
#include "omp_provider.h"
#include "omp_rpc.h"

#include <logonomic_cli_addon/json_utilities.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/sha256.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace {
namespace addon = logonomic::cli_addon;
namespace json = addon::json;
namespace fs = std::filesystem;
using Json = addon::Json_value;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

class Invocation : public addon::Invocation_context
{
public:
    addon::Invoke_request input;
    std::optional<Json> result;
    std::string reason;
    bool cancelled = false;
    const addon::Invoke_request& request() const override { return input; }
    bool cancel_requested() const override { return cancelled; }
    bool wait_for_cancel(int) override { return cancelled; }
    void report_progress(std::uint64_t, std::optional<std::uint64_t>, std::string) override {}
    void succeed(Json value) override { result = std::move(value); }
    void fail(addon::Result_status, std::string code, std::string) override { reason = std::move(code); }
    bool finished() const override { return result.has_value() || !reason.empty(); }
};

class Fixture
{
public:
    fs::path root = fs::temp_directory_path() / ("omp-source-profile-" + addon::random_nonce_hex());
    fs::path home = root / "home";
    fs::path cwd = root / "workspace";
    fs::path program;
    addon::Accounts_launcher_request request;

    explicit Fixture(const char* executable)
    {
        fs::create_directories(home / ".omp/profiles/work/agent");
        fs::create_directories(cwd);
#ifdef _WIN32
        request.launch_environment.platform_id = "windows-x86_64";
        const auto* home_name = "USERPROFILE";
        program = root / "omp.exe";
#elif defined(__APPLE__)
        request.launch_environment.platform_id = "macos-arm64";
        const auto* home_name = "HOME";
        program = root / "omp";
#else
        request.launch_environment.platform_id = "linux-x86_64";
        const auto* home_name = "HOME";
        program = root / "omp";
#endif
        fs::copy_file(fs::absolute(executable), program);
        request.launcher = {"omp", "direct", ""};
        request.normalization_directory = addon::path_to_utf8(cwd);
        request.default_home_fallback = addon::path_to_utf8(home);
        request.launch_environment.effective_environment = json::object({
            {home_name, Json(addon::path_to_utf8(home))}, {"PATH", Json(addon::path_to_utf8(root))}});
    }
    ~Fixture() { std::error_code error; fs::remove_all(root, error); }
    void set(const char* name, std::string value) { request.launch_environment.effective_environment.set(name, Json(std::move(value))); }
    omp::Native_profile profile() const
    {
        addon::Contract_failure failure;
        const auto result = omp::resolve_native_profile(request.launcher, request.launch_environment,
            request.normalization_directory, request.default_home_fallback, std::nullopt, {}, &failure);
        if (!result) throw std::runtime_error("Profile resolution failed: " + failure.reason_code);
        return *result;
    }
    std::string failure() const
    {
        addon::Contract_failure failure;
        require(!omp::resolve_native_profile(request.launcher, request.launch_environment,
            request.normalization_directory, request.default_home_fallback, std::nullopt, {}, &failure),
            "The unsupported native source must not resolve");
        return failure.reason_code;
    }
    Invocation accounts(const char* operation) const
    {
        Invocation invocation;
        invocation.input.capability = {"accounts", 2};
        invocation.input.operation = operation;
        invocation.input.payload = addon::encode_accounts_launcher_request(request);
        omp::handle_accounts(invocation);
        return invocation;
    }
};

void selectors_and_paths(const char* executable)
{
    Fixture f(executable);
    auto profile = f.profile();
    require(profile.agent == f.home / ".omp/agent" && profile.source_profile == fs::canonical(f.home / ".omp"),
        "The default source profile is the native config root, not a fabricated login");
    f.set("OMP_PROFILE", "work");
    f.set("PI_PROFILE", "ignored");
    f.set("PI_CODING_AGENT_DIR", addon::path_to_utf8(f.root / "custom"));
    profile = f.profile();
    require(profile.name == "work" && profile.agent == f.home / ".omp/profiles/work/agent",
        "Named native profiles override the agent-directory environment and legacy selector");
    f.request.launcher.script_body = "omp --profile default";
    require(f.profile().agent == f.root / "custom", "Explicit default restores a non-profile agent override");
    f.set("PI_CODING_AGENT_DIR", addon::path_to_utf8(f.home / ".omp/profiles/work/agent"));
    require(f.profile().agent == f.home / ".omp/agent", "Inherited profile-derived agent paths cannot become the default override");
    f.request.launcher.script_body = "omp";
    f.set("OMP_PROFILE", "");
    f.set("PI_PROFILE", "work");
    require(f.profile().agent == f.home / ".omp/agent", "An explicitly empty canonical profile suppresses the legacy selector");
    f.request.launcher.script_body = "omp --system-prompt --profile";
    require(f.profile().name.empty(), "A profile-looking scalar operand is not a selector");
    f.request.launcher.script_body = "omp --profile=work --profile=default";
    require(f.profile().name.empty(), "The last real native profile selector wins");
    f.request.launcher.script_body = "omp --profile=..";
    require(f.failure() == "native_profile_invalid", "Invalid profile names fail explicitly");
    for (const auto* name : {"CON", "con.txt", "com0", "work.", "two words"}) {
        f.request.launcher.script_body = "omp";
        f.set("OMP_PROFILE", name);
        require(f.failure() == "native_profile_invalid", "Native profile grammar and device names must be respected on every platform");
    }
    f.set("OMP_PROFILE", "\xef\xbb\xbfwork\xc2\xa0");
    require(f.profile().name == "work", "Native ECMAScript whitespace normalization includes BOM");
    f.set("OMP_PROFILE", "\xc2\x85work");
    require(f.failure() == "native_profile_invalid", "Native profile trimming must not accept U+0085");
    f.set("OMP_PROFILE", "default");
    f.set("PI_CONFIG_DIR", ".omp-other");
    f.set("PI_CODING_AGENT_DIR", "relative-agent");
    f.set("PI_CODING_AGENT_SESSION_DIR", "ambient-sessions");
    f.request.launcher.script_body = "omp --session-dir native-sessions";
    profile = f.profile();
    require(profile.agent == f.cwd / "relative-agent" && profile.explicit_session_directory == f.cwd / "native-sessions",
        "Relative agent and session paths use the frozen native cwd and explicit session-directory precedence");
    f.request.launcher.script_body = "omp";
    require(f.profile().explicit_session_directory == f.cwd / "ambient-sessions", "Native ambient session-directory selection is retained");
    f.request.launcher.script_body = "omp && echo wrong";
    f.request.launcher.runner_key = "bash";
    require(f.failure() == "native_profile_launcher_unsupported", "Compound launchers are never interpreted as profile declarations");
}

void xdg_and_environment(const char* executable)
{
    Fixture f(executable);
#ifndef _WIN32
    f.set("XDG_DATA_HOME", addon::path_to_utf8(f.root / "data"));
    f.set("XDG_STATE_HOME", addon::path_to_utf8(f.root / "state"));
    require(f.profile().data == f.home / ".omp/agent", "Absent XDG app roots do not redirect native storage");
    fs::create_directories(f.root / "data/omp");
    fs::create_directories(f.root / "state/omp");
    require(f.profile().data == f.root / "data/omp" && f.profile().state == f.root / "state/omp",
        "Native XDG data/state categories resolve independently");
    f.set("OMP_PROFILE", "work");
    require(f.profile().data == f.home / ".omp/profiles/work/agent",
        "A base XDG app directory cannot silently move a named profile");
    fs::create_directories(f.root / "data/omp/profiles/work");
    require(f.profile().data == f.root / "data/omp/profiles/work", "Named XDG routing requires the profile-specific native directory");
    f.set("OMP_PROFILE", "default");
    f.set("PI_CODING_AGENT_DIR", addon::path_to_utf8(f.root / "custom"));
    require(f.profile().data == f.root / "custom", "Explicit agent overrides do not inherit ambient XDG routing");
#endif
    std::ofstream(f.cwd / ".env") << "OPENAI_API_KEY=fixture-not-a-live-key\n";
    require(!f.profile().account_key.empty(), "Ordinary native credential dotenv files are not interpreted or rejected");
    for (const auto* content : {
            "export PI_CODING_AGENT_DIR=unresolved-native-source\n",
            "\xef\xbb\xbfPI_CONFIG_DIR=.redirected\n",
            "PI_CONFIG_DIR : .redirected\n",
            "\vPI_CONFIG_DIR\f= .redirected\n",
            "PI_CONFIG_DIR\n= .redirected\n",
            "# PI_CONFIG_DIR=.redirected\n",
            "\"PI_CONFIG_DIR\"=.redirected\n",
            "OPENAI_API_KEY=PI_CONFIG_DIR=.redirected\n",
            "OPENAI_API_KEY='quoted HOME text'\n"}) {
        std::ofstream(f.cwd / ".env.local", std::ios::binary) << content;
        require(f.failure() == "native_profile_environment_unsupported",
            "Selector spellings must be refused without guessing native assignment, comment or quoted-value grammar");
    }
    std::ofstream(f.cwd / ".env.local", std::ios::binary) << "\xef\xbb\xbfOPENAI_API_KEY : fixture-only\n"
        "ANTHROPIC_API_KEY='fixture\ncredential'\n# Ordinary credentials remain admissible.\n";
    require(!f.profile().account_key.empty(), "Credential-only dotenv syntax does not need an add-on parser");
    fs::remove(f.cwd / ".env.local");
    addon::Contract_failure failure;
    require(!omp::resolve_native_profile(f.request.launcher, f.request.launch_environment,
        f.request.normalization_directory, f.request.default_home_fallback, std::nullopt, [] { return true; }, &failure) &&
        failure.reason_code == "cancelled", "Profile inspection honors cancellation before filesystem work");
}

void inventory_and_acknowledgments(const char* executable)
{
    Fixture f(executable);
    const auto external = f.root / addon::path_from_utf8("external caf\xc3\xa9 profile");
    fs::create_directory(external);
    f.set("PI_CODING_AGENT_DIR", addon::path_to_utf8(external));
    auto discovery = f.accounts("discover");
    require(discovery.result.has_value(), "Native account discovery must succeed");
    const auto accounts = addon::decode_accounts_result(*discovery.result, nullptr);
    require(accounts && accounts->size() == 2, "Inventory includes the explicit external source and an existing native named profile");
    const auto source = addon::path_to_utf8(fs::canonical(external));
    const auto found = std::find_if(accounts->begin(), accounts->end(), [&](const auto& account) { return account.profile_path == source; });
    require(found != accounts->end() && found->display_name == addon::account_display_name(source) &&
        found->status == addon::Account_status::AVAILABLE, "Filesystem accounts use the SDK folder display-name contract");
    const auto resolution = f.accounts("resolve");
    require(resolution.result && addon::decode_account_resolution_result(*resolution.result, nullptr)->adapter_account_key == found->adapter_account_key,
        "Resolution selects the same admitted inventory identity");
    addon::Prepare_request request;
    request.launcher = f.request.launcher;
    request.launcher_argv = {"omp"};
    request.working_directory = addon::path_to_utf8(f.cwd);
    request.launch_environment = f.request.launch_environment;
    request.account_context = addon::Account_context{2, found->adapter_account_key, found->profile_path};
    request.declared_assets = {"assets/logonomic.ts", "assets/native_children.ts", "assets/native_wake.ts", "assets/native_model_catalog.ts"};
    const auto prepare = [&] {
        Invocation invocation;
        invocation.input.capability = {"launch-preparation", 3};
        invocation.input.operation = "prepare";
        invocation.input.payload = addon::encode_prepare_request(request);
        omp::handle_launch_preparation(invocation);
        return invocation;
    };
    auto prepared = prepare();
    require(prepared.result && !addon::account_context_acknowledgment_failure(*request.account_context, *prepared.result),
        "Actual preparation acknowledges the full selected version-2 source profile");
    request.account_context->version = 1;
    request.account_context->profile_path.reset();
    prepared = prepare();
    require(prepared.result && !addon::account_context_acknowledgment_failure(*request.account_context, *prepared.result),
        "Version-1 account acknowledgments retain the requested version");
    request.account_context->adapter_account_key = "different-source";
    require(prepare().reason == "account_context_mismatch", "Preparation must not echo an account belonging to a different native source");
    request.account_context = addon::Account_context{2, found->adapter_account_key, source + "/foreign"};
    require(prepare().reason == "account_context_mismatch", "A changed selected profile path cannot be acknowledged");
    request.account_context.reset();
    request.launcher.script_body = "omp --unknown-native-option";
    request.launcher_argv.push_back("--unknown-native-option");
    require(prepare().result.has_value(), "An unrelated ordinary launcher does not require account resolution when none is selected");
    for (int index = 0; index < 65; ++index) fs::create_directories(f.home / ".omp/profiles" / ("p" + std::to_string(index)));
    discovery = f.accounts("discover");
    require(discovery.reason == "account_inventory_capacity" && !discovery.result,
        "SDK inventory overflow fails instead of dropping selected or older profiles");
}

} // namespace

int main(int, char** argv)
{
    try {
        selectors_and_paths(argv[0]);
        xdg_and_environment(argv[0]);
        inventory_and_acknowledgments(argv[0]);
        std::puts("PASS native source profiles, SDK accounts and selected-context acknowledgments");
        return 0;
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
