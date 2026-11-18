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

#ifdef _WIN32
#include <windows.h>
#endif

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
    f.set("PI_CODING_AGENT_DIR", addon::path_to_utf8((f.home / ".omp/profiles/work/agent").make_preferred()));
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

void session_source_paths(const char* executable)
{
    Fixture f(executable);
    for (const auto* flag : {"--session", "--resume", "-r", "--restore"}) {
        f.request.launcher.script_body = std::string("omp ") + flag + " ./sessions/work";
        require(f.profile().explicit_session_file == f.cwd / "sessions/work",
            "Native path aliases retain exact relative extensionless sources");
    }
    const auto custom = f.cwd / addon::path_from_utf8("sessions/caf\xc3\xa9 history.record");
    f.request.launcher.script_body = "omp --session=\"" + addon::path_to_utf8(custom) + "\"";
    require(f.profile().explicit_session_file == custom,
        "Absolute non-ASCII custom-suffix paths remain exact sources");
    f.request.launcher.script_body = "omp --session folder\\work";
    require(f.profile().explicit_session_file == (f.cwd / addon::path_from_utf8("folder\\work")).lexically_normal(),
        "The native path criterion includes backslashes without inventing cross-platform separator rules");
    for (const auto* selector : {"work", "8fc4094a-6aa6-440e-b478-a5b2de576c48", "work.JSONL"}) {
        f.request.launcher.script_body = std::string("omp --session ") + selector;
        require(!f.profile().explicit_session_file, "Native names and IDs must remain opaque selectors");
    }
    f.request.launcher.script_body = "omp --session work.jsonl";
    require(f.profile().explicit_session_file == f.cwd / "work.jsonl",
        "A literal .jsonl basename is a native exact path without a separator");
    fs::create_directory(f.cwd / "other");
    f.request.launcher.script_body = "omp --cwd ./other --session ../sessions/work";
    require(f.profile().explicit_session_file == f.cwd / "sessions/work",
        "Relative sources resolve after the native startup cwd selection");
    f.request.launcher.script_body = "omp --session ./work --restore";
    require(f.profile().explicit_session_file == f.cwd / "work", "Bare restore preserves the selected native source");
    for (const auto* suffix : {"--resume", "--restore work", "--session work"}) {
        f.request.launcher.script_body = std::string("omp --session ./work ") + suffix;
        require(!f.profile().explicit_session_file, "A later picker or native selector clears an earlier exact path");
    }
    f.request.launcher.script_body = "omp --fork ./fork-source --session ./resume-source";
    require(f.profile().explicit_session_file == f.cwd / "fork-source",
        "Native fork source selection precedes resume irrespective of flag order");
    f.request.launcher.script_body = "omp --session ./resume-source --fork fork-name";
    require(!f.profile().explicit_session_file, "A native fork selector cannot inherit an unrelated resume path");
}

void implicit_home_working_directory(const char* executable)
{
    Fixture f(executable);
    f.request.normalization_directory = addon::path_to_utf8(f.home);
    fs::create_directory(f.home / "tmp");
    for (const auto* options : {"--session ./work", "--fork ./work"}) {
        f.request.launcher.script_body = std::string("omp ") + options;
        require(f.profile().explicit_session_file == f.home / "tmp/work",
            "Implicit native HOME relocation binds relative history to the first enterable native candidate");
    }
    f.request.launcher.script_body = "omp --session ./work.jsonl";
    require(f.profile().explicit_session_file == f.home / "tmp/work.jsonl", "Native relocation applies equally to .jsonl files");
#ifndef _WIN32
    fs::create_directory_symlink(f.home, f.root / "home-alias");
    f.request.normalization_directory = addon::path_to_utf8(f.root / "home-alias");
    require(f.profile().explicit_session_file == f.home / "tmp/work.jsonl",
        "Native complete-path normalization recognizes a symlink alias of HOME");
    f.request.normalization_directory = addon::path_to_utf8(f.home);
#endif
    f.request.launcher.script_body = "omp --session-dir ./history";
    require(f.profile().explicit_session_directory == f.home / "tmp/history", "Native relocation also applies to session directories");
    f.request.launcher.script_body = "omp --cwd ./other --cwd= --session ./work";
    require(f.profile().explicit_session_file == f.home / "tmp/work",
        "A last empty native cwd value restores implicit startup selection");
    f.request.launcher.script_body = "omp --allow-home --session ./work";
    require(f.profile().explicit_session_file == f.home / "work", "Native allow-home fixes relative history at HOME");
    f.request.launcher.script_body = "omp --cwd . --session ./work";
    require(f.profile().explicit_session_file == f.home / "work", "An explicit native cwd suppresses automatic relocation");
    f.request.launcher.script_body = "omp --cwd ./tmp --session ./work";
    require(f.profile().explicit_session_file == f.home / "tmp/work", "An explicit relative cwd remains usable at HOME");
    f.request.launcher.script_body = "omp --session \"" + addon::path_to_utf8(f.home / "work") + "\"";
    require(f.profile().explicit_session_file == f.home / "work", "Absolute exact sources do not depend on automatic relocation");
    f.request.launcher.script_body = "omp";
    require(!f.profile().account_key.empty(), "Ordinary source-profile inventory at HOME does not require a guessed cwd");
    f.set("PI_CODING_AGENT_SESSION_DIR", "relative-history");
    require(f.profile().explicit_session_directory == f.home / "tmp/relative-history",
        "Ambient relative session directories use the same native working-directory selection");
    f.request.launcher.script_body = "omp --allow-home";
    require(f.profile().explicit_session_directory == f.home / "relative-history",
        "Native allow-home also fixes ambient relative history directories");
#ifndef _WIN32
    fs::remove(f.home / "tmp");
    std::ofstream(f.home / "tmp") << "A file is not an enterable native candidate";
    f.request.launcher.script_body = "omp --session ./work";
    if (fs::is_directory("/tmp"))
        require(f.profile().explicit_session_file == fs::path("/tmp/work"),
            "Native POSIX candidate order skips a home/tmp file and reaches existing /tmp");
#endif
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
    std::ofstream(f.home / ".env.example") << "PI_PROFILE=example\n";
    std::ofstream(f.home / ".env.local") << "PI_PROFILE=local\n";
    std::ofstream(f.cwd / ".env.example") << "PI_PROFILE=example\n";
    require(f.accounts("discover").result.has_value(), "Unused dotenv templates do not change native accounts");
    std::ofstream(f.cwd / ".env.production.local") << "PI_PROFILE=production\n";
    require(f.accounts("resolve").result.has_value(), "Bun defaults to development when NODE_ENV is absent");
    f.set("NODE_ENV", "development");
    require(!f.profile().account_key.empty(), "Bun development mode ignores production dotenv files");
    f.set("NODE_ENV", "production");
    require(f.failure() == "native_profile_environment_unsupported", "Bun loads the selected mode's local dotenv");
    fs::remove(f.cwd / ".env.production.local");
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

#ifdef _WIN32
DWORD set_directory_case(const fs::path& path, bool sensitive)
{
    const HANDLE directory = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    require(directory != INVALID_HANDLE_VALUE, "The profile fixture directory could not be opened");
    FILE_CASE_SENSITIVE_INFO info{};
    if (sensitive) info.Flags = FILE_CS_FLAG_CASE_SENSITIVE_DIR;
    const BOOL set = SetFileInformationByHandle(directory, FileCaseSensitiveInfo, &info, sizeof(info));
    const DWORD error = set ? ERROR_SUCCESS : GetLastError();
    CloseHandle(directory);
    return error;
}
#endif

void filesystem_account_grants(const char* executable)
{
    Fixture fixture(executable);
    const auto profiles = fixture.root / "profiles";
    fs::create_directory(profiles);
    const auto select = [&](const fs::path& profile) {
        fixture.set("PI_CODING_AGENT_DIR", addon::path_to_utf8(profile));
        return fixture.profile();
    };
    const auto legacy_key = [](const fs::path& source) {
        auto path = addon::path_to_utf8(source);
#ifdef _WIN32
        for (char& c : path) {
            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        }
#endif
        return "omp-profile:" + addon::to_hex(addon::Sha256::digest(path));
    };
    const auto ordinary = select(profiles / addon::path_from_utf8("Caf\xc3\x89"));
    require(ordinary.account_key == legacy_key(ordinary.source_profile), "An ordinary profile retains its historical key");
    fs::create_directory(ordinary.source_profile);
    const auto existing = select(ordinary.source_profile);
    require(existing.account_key == ordinary.account_key, "Creating a selected profile preserves its identity");
#ifdef _WIN32
    const auto alias = profiles / addon::path_from_utf8("caf\xc3\xa9");
    require(omp::matches_native_account(existing, {2, existing.account_key, addon::path_to_utf8(alias)}),
        "An insensitive Windows directory accepts a native spelling alias in the original grant");
    // NTFS distinguishes Deseret capital and small long I (U+10400 / U+10428)
    // in ordinary directories. OMP historically preserved both UTF-8 spellings.
    const auto capital = select(profiles / addon::path_from_utf8("\xf0\x90\x90\x80"));
    const auto small = select(profiles / addon::path_from_utf8("\xf0\x90\x90\xa8"));
    require(fs::create_directory(capital.source_profile) && fs::create_directory(small.source_profile),
        "The two ordinary NTFS Unicode names must create separate native directories");
    require(capital.account_key == legacy_key(capital.source_profile) && small.account_key == legacy_key(small.source_profile) &&
        capital.account_key != small.account_key && select(capital.source_profile).account_key == capital.account_key &&
        select(small.source_profile).account_key == small.account_key, "The native Unicode pair preserves both historical OMP keys");
    require(!omp::matches_native_account(small, {2, small.account_key, addon::path_to_utf8(capital.source_profile)}),
        "A grant must not cross native-distinct Unicode paths in an ordinary NTFS directory");
#endif
    const auto different = select(profiles / "relocated");
    require(!omp::matches_native_account(different,
        {2, existing.account_key, addon::path_to_utf8(existing.source_profile)}), "A grant cannot follow a different source");

    const auto sensitive = fixture.root / "sensitive";
    fs::create_directory(sensitive);
#ifdef _WIN32
    const auto error = set_directory_case(sensitive, true);
    if (error == ERROR_ACCESS_DENIED || error == ERROR_NOT_SUPPORTED ||
        error == ERROR_INVALID_PARAMETER || error == ERROR_PRIVILEGE_NOT_HELD) {
        std::printf("SKIP case-sensitive source profiles on this volume (Windows error %lu)\n", error);
        return;
    }
    require(error == ERROR_SUCCESS, "Setting the native profile fixture case mode failed");
#endif
    const auto first = select(sensitive / "Account");
    const auto second = select(sensitive / "account");
    fs::create_directory(first.source_profile);
    if (!fs::create_directory(second.source_profile)) {
        std::puts("SKIP distinct native profile directories on this volume");
        return;
    }
    require(first.account_key != second.account_key && select(first.source_profile).account_key == first.account_key &&
        select(second.source_profile).account_key == second.account_key,
        "Distinct selectable source profiles keep separate keys before and after directory creation");
    require(!omp::matches_native_account(second, {2, first.account_key, addon::path_to_utf8(first.source_profile)}),
        "A case-sensitive sibling cannot use another source's grant");
#ifdef _WIN32
    const auto old = legacy_key(first.source_profile);
    require(!omp::matches_native_account(first, {1, old}) && !omp::matches_native_account(second, {1, old}),
        "A historical folded key alone cannot select a case-sensitive sibling");
    const addon::Account_context original{2, old, addon::path_to_utf8(first.source_profile)};
    require(omp::matches_native_account(first, original) && !omp::matches_native_account(second, original),
        "A historical path-bearing grant identifies only its original native source");
    const auto pending = select(sensitive / "missing-parent" / "agent");
    const addon::Account_context historical{2, legacy_key(pending.source_profile), addon::path_to_utf8(pending.source_profile)};
    require(!omp::matches_native_account(pending, historical), "An unobserved parent cannot admit a historical folded key");
    fs::create_directories(pending.source_profile);
    require(omp::matches_native_account(select(pending.source_profile), historical),
        "An observable containing directory allows the historical grant for that native locator");

    require(set_directory_case(first.source_profile, false) == ERROR_SUCCESS, "Setting the inner directory case mode failed");
    const auto upper = select(first.source_profile / addon::path_from_utf8("Caf\xc3\x89"));
    const auto lower = select(first.source_profile / addon::path_from_utf8("caf\xc3\xa9"));
    require(upper.account_key != lower.account_key &&
        omp::matches_native_account(lower, {2, upper.account_key, addon::path_to_utf8(upper.source_profile)}),
        "A current path-bearing grant preserves Unicode aliases inside a mixed-case-mode path");
#endif
}

} // namespace

int main(int, char** argv)
{
    try {
        selectors_and_paths(argv[0]);
        session_source_paths(argv[0]);
        implicit_home_working_directory(argv[0]);
        xdg_and_environment(argv[0]);
        inventory_and_acknowledgments(argv[0]);
        filesystem_account_grants(argv[0]);
        std::puts("PASS native source profiles, SDK accounts and selected-context acknowledgments");
        return 0;
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
