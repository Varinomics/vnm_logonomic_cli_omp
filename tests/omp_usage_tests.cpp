#include "omp_profiles.h"
#include "omp_usage.h"

#include <logonomic_cli_addon/json_utilities.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/sha256.h>
#include <logonomic_cli_addon/usage_scan.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace addon = logonomic::cli_addon;
namespace json = addon::json;
namespace fs = std::filesystem;
using Json = addon::Json_value;

void require(bool condition, const std::string& message)
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
    std::size_t progress = 0;
    const addon::Invoke_request& request() const override { return input; }
    bool cancel_requested() const override { return cancelled; }
    bool wait_for_cancel(int) override { return cancelled; }
    void report_progress(std::uint64_t, std::optional<std::uint64_t>, std::string) override { ++progress; }
    void succeed(Json value) override { result = std::move(value); }
    void fail(addon::Result_status, std::string code, std::string) override { reason = std::move(code); }
    bool finished() const override { return result.has_value() || !reason.empty(); }
};

const char* timestamp = "2026-11-15T10:00:00.000Z";

Json header(std::string id)
{
    return json::object({{"type", Json("session")}, {"version", Json(3)}, {"id", Json(std::move(id))},
        {"timestamp", Json(timestamp)}, {"cwd", Json("/native/project")}});
}

Json usage(std::uint64_t total)
{
    return json::object({{"input", Json(total)}, {"output", Json(0)}, {"cacheRead", Json(0)},
        {"cacheWrite", Json(0)}, {"totalTokens", Json(total)}});
}

Json assistant(std::string id, std::uint64_t total)
{
    return json::object({{"type", Json("message")}, {"id", Json(std::move(id))}, {"parentId", Json()},
        {"timestamp", Json(timestamp)}, {"message", json::object({{"role", Json("assistant")},
            {"provider", Json("fixture")}, {"model", Json("local-model")}, {"api", Json("openai-completions")},
            {"responseId", Json("native-response")}, {"usage", usage(total)}, {"stopReason", Json("stop")},
            {"content", Json::array()}})}});
}

Json auxiliary(std::string id, std::uint64_t total, const char* purpose = "compaction")
{
    return json::object({{"type", Json("model_usage")}, {"id", Json(std::move(id))}, {"parentId", Json()},
        {"timestamp", Json(timestamp)}, {"activeLeafId", Json()}, {"purpose", Json(purpose)},
        {"provider", Json("fixture")}, {"model", Json("local-model")}, {"api", Json("openai-completions")},
        {"usage", usage(total)}, {"stopReason", Json("stop")}});
}

Json discarded_marker(const char* original)
{
    return json::object({{"type", Json("branch_summary")}, {"id", Json("marker01")}, {"parentId", Json()},
        {"timestamp", Json(timestamp)}, {"summary", Json("")}, {"details", json::object({
            {"kind", Json("discarded-entry-branch")}, {"discardedEntryId", Json(original)}})}});
}

void write(const fs::path& path, const std::vector<Json>& entries, const std::string& tail = {})
{
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    for (const auto& entry : entries) stream << addon::serialize_json(entry) << '\n';
    stream << tail;
    stream.close();
    require(static_cast<bool>(stream), "Fixture source write failed");
}

class Fixture
{
public:
    fs::path root = fs::temp_directory_path() / ("omp-usage-" + addon::random_nonce_hex());
    fs::path home = root / "home";
    fs::path sources = home / ".omp/agent/sessions/project";
    fs::path storage = root / "host";
    addon::Usage_scan_request request;
    std::size_t allocation = 0;

    explicit Fixture(const char* executable)
    {
        fs::create_directories(sources);
        fs::create_directories(storage);
        fs::create_directories(root / "workspace");
        addon::Launch_environment environment;
#ifdef _WIN32
        environment.platform_id = "windows-x86_64";
        const auto* home_name = "USERPROFILE";
        const auto program = root / "omp.exe";
#elif defined(__APPLE__)
        environment.platform_id = "macos-arm64";
        const auto* home_name = "HOME";
        const auto program = root / "omp";
#else
        environment.platform_id = "linux-x86_64";
        const auto* home_name = "HOME";
        const auto program = root / "omp";
#endif
        fs::copy_file(fs::absolute(executable), program);
        environment.effective_environment = json::object({{home_name, Json(addon::path_to_utf8(home))},
            {"PATH", Json(addon::path_to_utf8(root))}});
        request.context.launch_environment = environment;
        request.context.launcher = {"omp", "direct", ""};
        request.context.launcher_argv = {"omp"};
        request.context.working_directory = addon::path_to_utf8(root / "workspace");
        request.context.profile_storage = addon::Profile_storage_context{1, addon::path_to_utf8(storage)};
        next();
    }
    ~Fixture() { std::error_code error; fs::remove_all(root, error); }
    void select_account()
    {
        addon::Contract_failure failure;
        const auto profile = omp::resolve_native_profile(request.context.launcher, *request.context.launch_environment,
            request.context.working_directory, {}, request.context.launcher_argv, {}, &failure);
        require(profile.has_value(), "Fixture native profile must resolve");
        request.context.account_context = addon::Account_context{2, profile->account_key, addon::path_to_utf8(profile->source_profile)};
    }
    void next(std::optional<addon::Usage_cache_reference> accepted = std::nullopt, const char* mode = "incremental")
    {
        const auto path = storage / ("candidate-" + std::to_string(++allocation));
        fs::create_directory(path);
        request.cache_selection = addon::Usage_cache_selection{std::move(accepted), addon::path_to_utf8(path)};
        request.mode = mode;
        request.snapshot_id.reset();
        request.cursor.reset();
    }
    Invocation invoke(omp::Usage_limits limits = {}, bool cancelled = false) const
    {
        Invocation invocation;
        invocation.input.capability = {"usage-scan", 3};
        invocation.input.operation = "scan";
        invocation.input.payload = addon::encode_usage_scan_request(request);
        invocation.cancelled = cancelled;
        omp::handle_usage(invocation, limits);
        return invocation;
    }
    addon::Usage_scan_page scan(omp::Usage_limits limits = {}) const
    {
        const auto invocation = invoke(limits);
        require(invocation.result.has_value(), "Usage scan failed: " + invocation.reason);
        addon::Contract_failure failure;
        const auto page = addon::decode_usage_scan_result(*invocation.result, &failure);
        require(page.has_value(), "Usage page failed the real SDK codec: " + failure.detail);
        require(page->profile_storage && !addon::profile_storage_acknowledgment_failure(*request.context.profile_storage, *invocation.result),
            "Every page must preserve exact host profile custody");
        if (request.context.account_context)
            require(!addon::account_context_acknowledgment_failure(*request.context.account_context, *invocation.result),
                "Every page must preserve the complete selected account record");
        require(page->cache_candidate && addon::validate_usage_cache_candidate(*page->cache_candidate,
            request.context.profile_storage->root, request.context.account_context, {}, &failure).has_value(),
            "The returned candidate must already be fully sealed and valid");
        return *page;
    }
};

std::uint64_t total(const addon::Usage_scan_page& page)
{
    std::uint64_t result = 0;
    for (const auto& record : page.records) result += record.total_tokens;
    return result;
}

void native_sources_and_copies(const char* executable)
{
    Fixture f(executable);
    f.select_account();
    const auto call = assistant("11111111", 100);
    const auto summary = auxiliary("22222222", 5);
    auto task = assistant("44444444", 999);
    auto& result = *task.find("message");
    result.set("role", Json("toolResult"));
    result.set("toolName", Json("task"));
    result.set("details", json::object({{"usage", usage(999)}}));
    write(f.sources / "root.jsonl", {header("root"), call, summary, task});
    write(f.sources / "fork.jsonl", {header("fork"), call, summary});
    write(f.sources / "root/child.jsonl", {header("child"), assistant("33333333", 20)}, "{\"partial");
    write(f.sources / ".omp-gc-compress-stale/copy.jsonl", {header("scratch"), assistant("55555555", 500)});
    std::ofstream(f.sources / "compressed.jsonl.gz") << "Opaque compressed history remains incomplete";
    const auto page = f.scan();
    require(page.complete && page.records.size() == 3 && total(page) == 125, "Forks and Task aggregates must not multiply billed calls");
    require(page.incomplete_historical_coverage, "Surviving JSONL cannot establish complete lifetime history");
    for (const auto& record : page.records) {
        require(record.aggregate_evidence == "complete" && record.cross_source_event &&
            record.cross_source_event->authority_namespace == "oh-my-pi.entry-usage",
            "Each complete atomic native amount must carry its validated native-pair proof");
        require(!record.session_key && !record.repository_path && !record.repository_usage,
            "Copied events and moved sessions cannot acquire guessed repository or session ownership");
    }
    const auto custom = f.root / "external/session.jsonl";
    write(custom, {header("external"), assistant("66666666", 7)});
    write(f.root / "external/session/deep/child.jsonl", {header("external-child"), assistant("77777777", 3)});
    const auto registry = f.home / ".omp/agent/custom-session-files";
    fs::create_directories(registry);
    std::ofstream(registry / "native-path-hash") << addon::path_to_utf8(custom);
    f.next(page.cache_candidate);
    const auto expanded = f.scan();
    require(expanded.records.size() == 5 && total(expanded) == 135, "The native exact-file registry includes its nested child storage");
}

void discard_and_retention(const char* executable)
{
    Fixture f(executable);
    const auto root = f.sources / "root.jsonl";
    const auto fork = f.sources / "fork.jsonl";
    const auto call = assistant("original", 100);
    write(root, {header("root"), call});
    write(fork, {header("fork"), call});
    const auto initial = f.scan();
    require(initial.records.size() == 1, "Copied calls start with one billed event");
    const auto identity = initial.records.front().record_id;
    const auto proof = initial.records.front().cross_source_event->event_id;
    write(root, {header("root"), auxiliary("replaced", 100, "discarded-turn"), discarded_marker("original")});
    f.next(initial.cache_candidate);
    const auto rewritten = f.scan();
    require(rewritten.records.size() == 1 && total(rewritten) == 100 && rewritten.records.front().record_id == identity &&
        rewritten.records.front().cross_source_event->event_id == proof,
        "A native discard rewrite must preserve the already accepted billed identity without inventing a response ID");
    fs::remove(root);
    fs::remove(fork);
    f.next(rewritten.cache_candidate);
    const auto retained = f.scan();
    require(retained.records.size() == 1 && retained.records.front().record_id == identity && total(retained) == 100,
        "Deleted current sources must not erase the host-selected retained evidence");
    f.next(std::nullopt, "reset");
    require(f.scan().records.empty(), "Explicit reset reconstructs only currently available sources");
    write(root, {header("root"), auxiliary("replaced", 100, "discarded-turn")});
    f.next(rewritten.cache_candidate);
    require(f.invoke().reason == "usage_discard_alias_unavailable", "An unbound discarded-turn replacement cannot create another billed event");
    write(root, {header("root"), assistant("original", 101)});
    f.next(rewritten.cache_candidate);
    require(f.invoke().reason == "usage_native_event_conflict", "Contradictory native evidence cannot replace or maximize accepted counters");
    addon::Contract_failure failure;
    require(addon::validate_usage_cache_candidate(*rewritten.cache_candidate, f.request.context.profile_storage->root,
        {}, {}, &failure).has_value(), "A failed replacement must leave the previously selected cache untouched");
}

void dimensions_and_rejections(const char* executable)
{
    Fixture f(executable);
    const auto source = f.sources / "root.jsonl";
    auto call = assistant("11111111", 100);
    call.find("message")->find("usage")->erase("output");
    write(source, {header("root"), call});
    const auto partial = f.scan();
    require(partial.records.front().output.coverage->support == addon::Usage_support::UNKNOWN &&
        partial.records.front().output.coverage->covered_records == 0, "A missing native dimension is unknown, not an observed zero");
    call.find("message")->find("usage")->set("output", Json(17));
    write(source, {header("root"), call});
    f.next(partial.cache_candidate);
    const auto refined = f.scan();
    require(refined.records.front().output.value == 17 && refined.records.front().output.coverage->covered_records == 1,
        "A later native observation may fill a previously unknown dimension coherently");
    call.find("message")->find("usage")->set("output", Json(18));
    write(source, {header("root"), call});
    f.next(refined.cache_candidate);
    require(f.invoke().reason == "usage_native_event_conflict", "Known dimensions remain immutable for one native event");
    call.find("message")->find("usage")->erase("totalTokens");
    write(source, {header("root"), call});
    f.next();
    require(f.invoke().reason == "usage_native_counter_unavailable", "Missing authoritative totals cannot be replaced by guessed bucket sums");
    call.find("message")->find("usage")->set("totalTokens", Json(1.25));
    write(source, {header("root"), call});
    f.next();
    require(f.invoke().reason == "usage_native_counter_invalid", "Fractional token totals are invalid evidence");
    auto old_header = header("old");
    old_header.set("version", Json(1));
    write(source, {old_header});
    f.next();
    require(f.invoke().reason == "usage_native_format_unsupported", "Legacy unindexed files cannot acquire fabricated stable event IDs");
    write(source, {header("root"), assistant("11111111", 1), assistant("22222222", 1)});
    f.next();
    auto limits = omp::Usage_limits{};
    limits.events = 1;
    require(f.invoke(limits).reason == "usage_history_capacity", "Capacity exhaustion fails instead of returning truncated history");
    f.next();
    require(f.invoke({}, true).reason == "cancelled", "Cancellation remains effective before source or candidate work");
}

void paging_and_cache_custody(const char* executable)
{
    Fixture f(executable);
    f.select_account();
    std::vector<Json> records{header("paged")};
    for (int index = 0; index < 270; ++index) records.push_back(assistant("entry-" + std::to_string(index), 1));
    write(f.sources / "paged.jsonl", records);
    auto first = f.scan();
    require(!first.complete && first.records.size() == 256 && first.next_cursor, "A large snapshot must page within the SDK record bound");
    f.request.snapshot_id = first.snapshot_id;
    f.request.cursor = first.next_cursor;
    f.request.context.launch_environment->effective_environment.set("OMP_PROFILE", Json("other"));
    require(f.invoke().reason == "usage_snapshot_expired", "A changed frozen environment cannot continue another snapshot");
    f.request.context.launch_environment->effective_environment.erase("OMP_PROFILE");
    f.request.cursor = "257";
    require(f.invoke().reason == "usage_cursor_invalid", "Unissued cursors cannot omit an interval of the frozen ledger");
    f.request.cursor = first.next_cursor;
    fs::remove(f.sources / "paged.jsonl");
    const auto second = f.scan();
    require(second.complete && second.records.size() == 14 && second.cache_candidate == first.cache_candidate &&
        second.scope.observed_at_ms == first.scope.observed_at_ms && first.records.back().record_id < second.records.front().record_id,
        "Continuation must retain the sealed snapshot, ordering and metadata after current sources disappear");
    f.next(first.cache_candidate);
    require(f.scan().records.size() == 256, "A retained large ledger remains available after source deletion");
    f.next(first.cache_candidate);
    std::ofstream(addon::path_from_utf8(first.cache_candidate->root) / "ledger.jsonl", std::ios::app) << "corruption\n";
    require(!f.invoke().result, "A corrupt selected cache must fail without an empty-cache fallback");
    f.next();
    f.request.context.account_context->adapter_account_key = "foreign";
    require(f.invoke().reason == "account_context_mismatch", "Usage cannot acknowledge an account from another source");
}

} // namespace

int main(int, char** argv)
{
    try {
        native_sources_and_copies(argv[0]);
        discard_and_retention(argv[0]);
        dimensions_and_rejections(argv[0]);
        paging_and_cache_custody(argv[0]);
        std::puts("PASS native event accounting, fork/discard identity, sealed retention and bounded paging");
        return 0;
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
