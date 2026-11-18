#include "omp_profiles.h"
#include "omp_provider.h"

#include <logonomic_cli_addon/json_utilities.h>
#include <logonomic_cli_addon/launcher_utilities.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/sha256.h>

#include <algorithm>
#include <cerrno>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <stdexcept>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace omp {
namespace {
namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;
namespace json = addon::json;

class Profile_error final : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

bool one_of(std::string_view value, std::initializer_list<std::string_view> options)
{
    return std::find(options.begin(), options.end(), value) != options.end();
}

bool native_platform(std::string_view platform)
{
#ifdef _WIN32
    return platform.starts_with("windows-");
#elif defined(__APPLE__)
    return platform.starts_with("macos-");
#else
    return platform.starts_with("linux-");
#endif
}

std::optional<std::string> env(const addon::Launch_environment& environment, std::string_view name)
{
    for (const auto& member : environment.effective_environment.as_object()) {
        if (environment.platform_id.starts_with("windows-") ?
                addon::lower(member.name) == addon::lower(std::string(name)) : member.name == name) {
            return member.value.as_string();
        }
    }
    return std::nullopt;
}

void check_cancelled(const std::function<bool()>& cancelled)
{
    if (cancelled && cancelled()) {
        throw Profile_error("cancelled");
    }
}

std::string profile_name(std::string_view source)
{
    // ECMAScript trim includes BOM and excludes U+0085; the SDK's general
    // launcher whitespace grammar intentionally differs at those two points.
    constexpr std::string_view spaces[] = {" ", "\t", "\n", "\r", "\v", "\f", "\xc2\xa0", "\xe1\x9a\x80",
        "\xe2\x80\x80", "\xe2\x80\x81", "\xe2\x80\x82", "\xe2\x80\x83", "\xe2\x80\x84", "\xe2\x80\x85",
        "\xe2\x80\x86", "\xe2\x80\x87", "\xe2\x80\x88", "\xe2\x80\x89", "\xe2\x80\x8a", "\xe2\x80\xa8",
        "\xe2\x80\xa9", "\xe2\x80\xaf", "\xe2\x81\x9f", "\xe3\x80\x80", "\xef\xbb\xbf"};
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto space : spaces) {
            if (source.starts_with(space)) { source.remove_prefix(space.size()); changed = true; }
            if (source.ends_with(space)) { source.remove_suffix(space.size()); changed = true; }
        }
    }
    const std::string name(source);
    if (name.empty() || name == "default") {
        return {};
    }
    static const std::regex grammar("^[a-z0-9][a-z0-9._-]{0,63}$");
    static const std::regex device("^(con|prn|aux|nul|com[0-9]|lpt[0-9])([.].*)?$");
    if (!std::regex_match(name, grammar) || name == "." || name == ".." || name.ends_with('.') ||
        std::regex_match(name, device)) {
        throw Profile_error("native_profile_invalid");
    }
    return name;
}

std::string safe_profile_name(const std::optional<std::string>& value)
{
    try {
        return profile_name(value.value_or(""));
    }
    catch (const Profile_error&) {
        return {};
    }
}

fs::path normalize_path(std::string_view value, const fs::path& cwd)
{
    auto path = addon::path_from_utf8(value);
#ifdef _WIN32
    // Drive-relative and root-relative paths depend on native per-drive state
    // that is absent from the frozen SDK request. Do not guess that state.
    if ((path.has_root_name() && !path.has_root_directory()) ||
        (!path.has_root_name() && path.has_root_directory())) {
        throw Profile_error("native_profile_path_unsupported");
    }
#endif
    if (!path.is_absolute()) {
        path = cwd / path;
    }
    return path.lexically_normal();
}

fs::path canonical_profile(const fs::path& path)
{
    std::error_code error;
    auto canonical = fs::weakly_canonical(path, error);
    if (error || !canonical.is_absolute()) {
        throw Profile_error("native_profile_path_unavailable");
    }
    return canonical;
}

std::string account_key(std::string_view identity)
{
    return "omp-profile:" + addon::to_hex(addon::Sha256::digest(identity));
}

std::string native_comparable_path(const fs::path& path, bool windows)
{
    // Native normalizePathForComparison resolves only a complete existing
    // path. On failure it retains the lexical absolute spelling, rather than
    // resolving a missing suffix through its deepest existing ancestor.
    std::error_code error;
    const auto canonical = fs::canonical(path, error);
    auto result = addon::path_to_utf8(error ? path.lexically_normal() : canonical);
    if (windows) result = addon::lower(std::move(result));
    return result;
}

bool enterable_directory(const fs::path& path)
{
    std::error_code error;
    if (!fs::is_directory(fs::status(path, error)) || error) return false;
#ifdef _WIN32
    const auto directory = CreateFileW(path.c_str(), FILE_TRAVERSE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (directory == INVALID_HANDLE_VALUE)
        throw Profile_error("native_profile_working_directory_unsupported");
    CloseHandle(directory);
    return true;
#else
    // Native chdir uses effective credentials. Do not infer its permission
    // result from access() when an unusual launcher uses different real IDs.
    if (getuid() != geteuid() || getgid() != getegid())
        throw Profile_error("native_profile_working_directory_unsupported");
    if (access(path.c_str(), X_OK) == 0) return true;
    if (errno == EACCES || errno == ENOENT || errno == ENOTDIR) return false;
    throw Profile_error("native_profile_working_directory_unsupported");
#endif
}

fs::path automatic_home_directory(const fs::path& home, const std::function<bool()>& cancelled)
{
    std::vector<fs::path> candidates{home / "tmp"};
#ifndef _WIN32
    candidates.emplace_back("/tmp");
    candidates.emplace_back("/var/tmp");
#endif
    for (const auto& candidate : candidates) {
        check_cancelled(cancelled);
        if (enterable_directory(candidate)) return candidate;
    }
    // The remaining native os.tmpdir fallback depends on runtime/platform
    // inputs not all declared by these capabilities. Do not use this adapter's
    // own environment or bind the source to HOME when that choice is unknown.
    throw Profile_error("native_profile_working_directory_unsupported");
}

bool path_exists(const fs::path& path)
{
    std::error_code error;
    const auto status = fs::status(path, error);
    if (error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory) {
        return false;
    }
    if (error) {
        throw Profile_error("native_profile_source_unavailable");
    }
    return fs::exists(status);
}

// Bun can load project dotenv before native CLI bootstrap, and OMP later reads
// project/agent/config/home dotenv and refreshes its directory resolver. A path
// selector there cannot be resolved from the frozen host environment alone.
// This is deliberately not an assignment parser. Native parseEnv accepts BOM,
// colon delimiters and whitespace across lines, and its grammar is runtime-owned.
// Refuse any selector spelling, even inside comments or quoted values, instead
// of admitting a file on the strength of a partial dotenv interpretation.
void check_dotenv(const std::set<fs::path>& files, const std::function<bool()>& cancelled)
{
    constexpr std::string_view selectors[] = {"HOME", "USERPROFILE", "OMP_PROFILE", "PI_PROFILE",
        "PI_CONFIG_DIR", "OMP_CONFIG_DIR", "PI_CODING_AGENT_DIR", "OMP_CODING_AGENT_DIR",
        "PI_CODING_AGENT_SESSION_DIR", "OMP_CODING_AGENT_SESSION_DIR", "XDG_DATA_HOME",
        "XDG_STATE_HOME", "XDG_CACHE_HOME"};
    for (const auto& file : files) {
        check_cancelled(cancelled);
        std::error_code error;
        const auto status = fs::status(file, error);
        if (error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory) {
            continue;
        }
        if (error) {
            throw Profile_error("native_profile_source_unavailable");
        }
        if (!fs::is_regular_file(status)) {
            continue;
        }
        std::ifstream input(file, std::ios::binary);
        if (!input) throw Profile_error("native_profile_source_unavailable");
        std::string content;
        char buffer[4096];
        while (input.read(buffer, sizeof(buffer)) || input.gcount() != 0) {
            check_cancelled(cancelled);
            content.append(buffer, static_cast<std::size_t>(input.gcount()));
            if (content.size() > 1024 * 1024) throw Profile_error("native_profile_environment_unsupported");
        }
        if (!input.eof()) throw Profile_error("native_profile_source_unavailable");
        for (auto& character : content) {
            if (character >= 'a' && character <= 'z') character -= 'a' - 'A';
        }
        if (std::any_of(std::begin(selectors), std::end(selectors), [&](const auto selector) {
                return content.find(selector) != std::string::npos;
            })) {
            throw Profile_error("native_profile_environment_unsupported");
        }
    }
}

struct Selectors
{
    std::optional<std::string> profile;
    std::optional<std::string> session_directory;
    std::optional<std::string> session_file;
    std::optional<std::string> cwd;
    bool allow_home = false;
};

bool native_session_path(std::string_view value)
{
    // createSessionManager uses this lexical boundary before native ID/name
    // resolution. A bare custom filename is still a selector, even if it exists.
    return value.find('/') != std::string_view::npos || value.find('\\') != std::string_view::npos ||
        value.ends_with(".jsonl");
}

Selectors selectors(const Launcher_command& launcher)
{
    if (!interactive_arguments(launcher.arguments)) {
        throw Profile_error("native_profile_launcher_unsupported");
    }
    Selectors result;
    std::optional<std::string> fork_source;
    for (std::size_t index = 0; index < launcher.arguments.size(); ++index) {
        const std::string_view argument = launcher.arguments[index];
        const auto equals = argument.find('=');
        const auto option = argument.substr(0, equals);
        if (one_of(option, {"--resume", "-r", "--session", "--restore"})) {
            std::string value;
            if (equals != std::string_view::npos) value = argument.substr(equals + 1);
            else if (index + 1 < launcher.arguments.size() && !launcher.arguments[index + 1].starts_with('-'))
                value = launcher.arguments[++index];
            if (native_session_path(value)) result.session_file = std::move(value);
            else if (option != "--restore" || !value.empty()) result.session_file.reset();
            continue;
        }
        if (one_of(option, {"--continue", "-c", "--no-extensions", "--no-session", "--no-tools", "--no-skills",
                "--no-rules", "--no-context-files", "--no-lsp", "--no-title", "--allow-home", "--offline",
                "--hide-thinking", "--auto-approve", "--yolo"})) {
            if (option == "--allow-home") result.allow_home = true;
            continue;
        }
        // The existing supported interactive grammar has already proved that
        // every remaining option takes one literal value. Consume it before
        // considering profile-looking bytes in system prompts or other data.
        const std::string value = equals != std::string_view::npos ? std::string(argument.substr(equals + 1)) :
            launcher.arguments[++index];
        if (option == "--profile") {
            if (value.empty() || (equals == std::string_view::npos && value.starts_with('-')))
                throw Profile_error("native_profile_invalid");
            result.profile = value;
        }
        else if (option == "--session-dir") result.session_directory = value;
        else if (option == "--cwd") result.cwd = value;
        else if (option == "--fork") fork_source = value;
    }
    // Native fork selection precedes resume selection, regardless of flag
    // ordering. Keep selectors opaque; never resolve one as a filesystem path.
    if (fork_source && !fork_source->empty())
        result.session_file = native_session_path(*fork_source) ? fork_source : std::nullopt;
    return result;
}

Native_profile resolve(const addon::Launcher_request& request, const addon::Launch_environment& environment,
    std::string_view normalization_directory, std::string_view fallback,
    std::optional<std::span<const std::string>> argv, const std::function<bool()>& cancelled, bool node_env_declared)
{
    check_cancelled(cancelled);
    if (!native_platform(environment.platform_id)) throw Profile_error("native_profile_platform_unavailable");
    const auto launcher = recognize_launcher(request, environment.platform_id, argv);
    if (!launcher) throw Profile_error("native_profile_launcher_unsupported");
    for (const auto& name : launcher->environment_names) {
        if (!env(environment, name)) throw Profile_error("native_profile_environment_unavailable");
    }
    const auto options = selectors(*launcher);
    const auto cwd = addon::normalized_resume_directory(normalization_directory);
    if (!cwd) throw Profile_error("native_profile_path_unavailable");
    const bool windows = environment.platform_id.starts_with("windows-");
    const auto home_value = env(environment, windows ? "USERPROFILE" : "HOME");
    const std::string home_text = home_value && !home_value->empty() ? *home_value : std::string(fallback);
    const auto home = addon::normalized_resume_directory(home_text);
    if (!home) throw Profile_error("native_profile_home_unavailable");
    auto config_name = env(environment, "PI_CONFIG_DIR").value_or(".omp");
    if (config_name.empty()) config_name = ".omp";
    auto config_part = addon::path_from_utf8(config_name);
    // Native path.join keeps the initial home prefix on POSIX even when the
    // later component starts with '/'. Windows drive components need native
    // semantics that a second rooted path cannot safely approximate here.
#ifdef _WIN32
    if (config_part.has_root_name()) throw Profile_error("native_profile_path_unsupported");
#endif
    const auto base = (*home / config_part.relative_path()).lexically_normal();
    const auto omp_profile = env(environment, "OMP_PROFILE");
    const auto pi_profile = env(environment, "PI_PROFILE");
    const auto environment_profile = omp_profile ? omp_profile : pi_profile;
    const auto inherited_name = safe_profile_name(environment_profile);
    const auto inherited_pi = safe_profile_name(pi_profile);
    const auto selected = profile_name(options.profile ? *options.profile : environment_profile.value_or(""));
    auto agent_override = env(environment, "PI_CODING_AGENT_DIR");
    const auto inherited = !inherited_name.empty() ? inherited_name : inherited_pi;
    if (agent_override && !inherited.empty() &&
        *agent_override == addon::path_to_utf8(base / "profiles" / inherited / "agent")) {
        agent_override.reset();
    }
    Native_profile profile;
    profile.name = selected;
    profile.home = *home;
    profile.base_config = base;
    profile.config = selected.empty() ? base : base / "profiles" / selected;
    const auto default_agent = profile.config / "agent";
    profile.agent = selected.empty() && agent_override && !agent_override->empty() ?
        normalize_path(*agent_override, *cwd) : default_agent;
    profile.data = profile.agent;
    profile.state = profile.agent;
    if (!windows && profile.agent == default_agent) {
        for (const auto& [name, target] : std::initializer_list<std::pair<const char*, fs::path*>>{
                {"XDG_DATA_HOME", &profile.data}, {"XDG_STATE_HOME", &profile.state}}) {
            const auto value = env(environment, name);
            if (!value || value->empty()) continue;
            auto candidate = addon::path_from_utf8(*value) / "omp";
            if (!selected.empty()) candidate /= fs::path("profiles") / selected;
            candidate = normalize_path(addon::path_to_utf8(candidate), *cwd);
            if (path_exists(candidate)) *target = candidate;
        }
    }
    auto project = options.cwd ? normalize_path(*options.cwd, *cwd) : *cwd;
    std::set<fs::path> dotenv{*cwd / ".env", project / ".env", *home / ".env",
        profile.config / ".env", profile.agent / ".env"};
    // Only Bun's startup directory has mode-specific files. Structured and
    // delegated sessions may inherit NODE_ENV outside their filtered snapshot.
    auto mode = env(environment, "NODE_ENV");
    if ((mode || node_env_declared) && mode != "test" && mode != "production") {
        mode = "development";
    }
    if (mode != "test") {
        dotenv.insert(*cwd / ".env.local");
    }
    for (const std::string candidate : {"development", "production", "test"}) {
        if (mode && *mode != candidate) {
            continue;
        }
        dotenv.insert(*cwd / (".env." + candidate));
        dotenv.insert(*cwd / (".env." + candidate + ".local"));
    }
    check_dotenv(dotenv, cancelled);
    profile.source_profile = canonical_profile(profile.agent != default_agent ? profile.agent : profile.config);
    const auto identity = addon::native_account_profile_identity(profile.source_profile, addon::Account_profile_case_fold::ASCII);
    if (!identity) throw Profile_error("native_profile_path_unavailable");
    profile.account_key = account_key(*identity);
    const auto session_directory = options.session_directory ? options.session_directory : env(environment, "PI_CODING_AGENT_SESSION_DIR");
    const auto relative = [](const std::optional<std::string>& value) {
        return value && !value->empty() && !addon::path_from_utf8(*value).is_absolute();
    };
    if ((!options.cwd || options.cwd->empty()) && !options.allow_home &&
        (relative(options.session_file) || relative(session_directory))) {
        const auto native_cwd = native_comparable_path(*cwd, windows);
        const auto native_home = native_comparable_path(*home, windows);
        // Match native applyStartupCwd's ordered existing-directory choices
        // without changing this process's cwd or creating a native directory.
        if (native_cwd == native_home) project = automatic_home_directory(*home, cancelled);
    }
    if (session_directory && !session_directory->empty())
        profile.explicit_session_directory = normalize_path(*session_directory, project);
    if (options.session_file) profile.explicit_session_file = normalize_path(*options.session_file, project);
    return profile;
}

void set_failure(addon::Contract_failure* failure, const std::string& reason)
{
    if (failure) *failure = {reason, "The selected native source profile could not be established from this frozen invocation."};
}

} // namespace

bool matches_native_account(const Native_profile& profile, const addon::Account_context& grant)
{
    if (grant.version == 1) return grant.adapter_account_key == profile.account_key;
    if (grant.version != 2 || !grant.profile_path) return false;
    const auto granted = addon::path_from_utf8(*grant.profile_path);
    if (!addon::same_native_account_profile(granted, profile.source_profile)) return false;
    if (grant.adapter_account_key == profile.account_key) return true;
#ifdef _WIN32
    // An insensitive non-ASCII variant may retain a different ASCII-folded
    // digest for the same source, including within a mixed-case-mode path.
    const auto retained = addon::native_account_profile_identity(granted, addon::Account_profile_case_fold::ASCII);
    if (retained && grant.adapter_account_key == account_key(*retained)) return true;
    const auto historical = addon::historical_account_profile_identity(
        granted, profile.source_profile, addon::Account_profile_case_fold::ASCII);
    return historical && grant.adapter_account_key == account_key(*historical);
#else
    return false;
#endif
}

std::optional<Native_profile> resolve_native_profile(const addon::Launcher_request& launcher,
    const addon::Launch_environment& environment, std::string_view normalization_directory,
    std::string_view default_home_fallback, std::optional<std::span<const std::string>> host_argv,
    const std::function<bool()>& cancelled, addon::Contract_failure* failure, bool node_env_declared)
{
    try {
        return resolve(launcher, environment, normalization_directory, default_home_fallback, host_argv,
            cancelled, node_env_declared);
    }
    catch (const Profile_error& error) {
        set_failure(failure, error.what());
    }
    catch (const fs::filesystem_error&) {
        set_failure(failure, "native_profile_source_unavailable");
    }
    return std::nullopt;
}

namespace {

template<typename Request>
bool validate_account(const Request& request, bool node_env_declared,
    const std::function<bool()>& cancelled, addon::Contract_failure* failure)
{
    if (!request.account_context) return true;
    if (!request.launch_environment) {
        set_failure(failure, "native_profile_environment_unavailable");
        return false;
    }
    const auto profile = resolve_native_profile(request.launcher, *request.launch_environment,
        request.working_directory, {}, request.launcher_argv, cancelled, failure, node_env_declared);
    if (!profile) return false;
    if (!matches_native_account(*profile, *request.account_context)) {
        set_failure(failure, "account_context_mismatch");
        return false;
    }
    return true;
}

} // namespace

bool validate_native_account(const addon::Session_launch_context& request,
    const std::function<bool()>& cancelled, addon::Contract_failure* failure)
{
    return validate_account(request, false, cancelled, failure);
}

bool validate_native_account(const addon::Prepare_request& request,
    const std::function<bool()>& cancelled, addon::Contract_failure* failure)
{
    return validate_account(request, true, cancelled, failure);
}

void handle_accounts(addon::Invocation_context& context)
{
    if (context.request().operation != "discover" && context.request().operation != "resolve") {
        context.reject_unknown_operation();
        return;
    }
    addon::Contract_failure failure;
    const auto request = addon::decode_accounts_launcher_request(context.request().payload, &failure);
    if (!request) {
        context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
        return;
    }
    const auto cancelled = [&] { return context.cancel_requested(); };
    const auto selected = resolve_native_profile(request->launcher, request->launch_environment,
        request->normalization_directory, request->default_home_fallback, std::nullopt, cancelled, &failure);
    if (!selected) {
        context.fail(cancelled() ? addon::Result_status::CANCELLED : addon::Result_status::UNAVAILABLE,
            failure.reason_code, failure.detail);
        return;
    }
    if (context.request().operation == "resolve") {
        context.succeed(addon::encode_account_resolution({selected->account_key}));
        return;
    }
    try {
        std::map<std::string, Native_profile> profiles;
        profiles.emplace(selected->account_key, *selected);
        std::set<std::string> names{"default"};
        std::set<fs::path> directories{selected->base_config / "profiles"};
        for (const auto* name : {"XDG_DATA_HOME", "XDG_STATE_HOME"}) {
            const auto value = env(request->launch_environment, name);
            if (value && !value->empty() && !request->launch_environment.platform_id.starts_with("windows-"))
                directories.insert(normalize_path(*value, addon::path_from_utf8(request->normalization_directory)) / "omp/profiles");
        }
        for (const auto& directory : directories) {
            check_cancelled(cancelled);
            if (!path_exists(directory)) continue;
            for (const auto& entry : fs::directory_iterator(directory)) {
                check_cancelled(cancelled);
                if (!entry.is_directory()) continue;
                try {
                    auto name = profile_name(addon::path_to_utf8(entry.path().filename()));
                    if (!name.empty()) names.insert(std::move(name));
                }
                catch (const Profile_error&) { continue; }
                if (names.size() > 64) throw Profile_error("account_inventory_capacity");
            }
        }
        for (const auto& name : names) {
            check_cancelled(cancelled);
            auto environment = request->launch_environment;
            auto values = addon::environment_pairs(environment.effective_environment);
            addon::set_environment(values, "OMP_PROFILE", name);
            environment.effective_environment = addon::Json_value::object();
            for (const auto& [key, value] : values) environment.effective_environment.set(key, addon::Json_value(value));
            const auto profile = resolve({"omp", "direct", ""}, environment,
                request->normalization_directory, request->default_home_fallback, std::nullopt, cancelled, true);
            // Empty default profiles are still native selectable sources; the
            // status is unknown until their filesystem source exists.
            profiles.emplace(profile.account_key, profile);
            if (profiles.size() > 64) throw Profile_error("account_inventory_capacity");
        }
        std::vector<addon::Account_record> records;
        for (const auto& [key, profile] : profiles) {
            check_cancelled(cancelled);
            const auto path = addon::path_to_utf8(profile.source_profile);
            const auto status = path_exists(profile.source_profile) ?
                (fs::is_directory(profile.source_profile) ? addon::Account_status::AVAILABLE : addon::Account_status::UNAVAILABLE) :
                addon::Account_status::UNKNOWN;
            records.push_back({key, addon::account_display_name(path), status, path});
        }
        const auto encoded = addon::encode_accounts(records);
        if (!addon::decode_accounts_result(encoded, &failure)) throw Profile_error(failure.reason_code);
        context.succeed(encoded);
    }
    catch (const Profile_error& error) {
        context.fail(cancelled() ? addon::Result_status::CANCELLED : addon::Result_status::UNAVAILABLE, error.what());
    }
    catch (const fs::filesystem_error&) {
        context.fail(addon::Result_status::UNAVAILABLE, "native_profile_source_unavailable");
    }
}

} // namespace omp
