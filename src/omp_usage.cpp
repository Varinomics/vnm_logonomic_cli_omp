#include "omp_usage.h"
#include "omp_profiles.h"

#include <logonomic_cli_addon/conversation_export.h>
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/json_utilities.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/sha256.h>
#include <logonomic_cli_addon/stored_usage_accounting.h>
#include <logonomic_cli_addon/usage_scan.h>
#include <logonomic_cli_addon/usage_temporal.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string_view>

namespace omp {
namespace {
namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;
namespace json = addon::json;
using Json = addon::Json_value;

constexpr std::uint64_t k_max_exact = 9007199254740991ULL;
constexpr const char* k_format = "omp-native-usage@1";
constexpr const char* k_event_namespace = "oh-my-pi.entry-usage";

class Usage_error final : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

class Progress
{
public:
    explicit Progress(addon::Invocation_context& context) : context_(context) {}
    std::uint64_t examined = 0;
    bool cancelled()
    {
        if (context_.cancel_requested()) return true;
        const auto now = std::chrono::steady_clock::now();
        if (now - last_ >= std::chrono::seconds(1)) {
            context_.report_progress(examined, std::nullopt, "Inspecting native usage evidence");
            last_ = now;
        }
        return false;
    }
    void check() { if (cancelled()) throw Usage_error("cancelled"); }
private:
    addon::Invocation_context& context_;
    std::chrono::steady_clock::time_point last_{};
};

std::string digest(std::string_view text)
{
    return addon::to_hex(addon::Sha256::digest(text));
}

std::string required_text(const Json& object, std::string_view key, std::size_t bound = 256)
{
    const auto* value = object.find(key);
    if (!value || !json::bounded_string(*value, bound) ||
        std::any_of(value->as_string().begin(), value->as_string().end(), [](unsigned char byte) {
            return byte < 0x20 || byte == 0x7f;
        })) throw Usage_error("usage_native_identity_invalid");
    return value->as_string();
}

std::optional<std::uint64_t> count(const Json& object, std::string_view key)
{
    const auto* value = object.find(key);
    if (!value) return std::nullopt;
    if (!value->is_integer() || value->as_integer() < 0 ||
        static_cast<std::uint64_t>(value->as_integer()) > k_max_exact)
        throw Usage_error("usage_native_counter_invalid");
    return static_cast<std::uint64_t>(value->as_integer());
}

std::string date_for(std::string_view timestamp)
{
    // Native entry envelopes use Date.toISOString. Keep an explicit UTC
    // instant; local-time guesses would make identity and dates host-dependent.
    if (timestamp.size() != 24 || timestamp[10] != 'T' || timestamp[19] != '.' || timestamp.back() != 'Z' ||
        !addon::parse_native_usage_timestamp(timestamp)) throw Usage_error("usage_native_timestamp_invalid");
    return std::string(timestamp.substr(0, 10));
}

struct Event
{
    std::string entry_id;
    std::string timestamp;
    std::string provider;
    std::string model;
    std::string api;
    std::string kind;
    std::optional<std::string> response_id;
    addon::Usage_record accounting;
};

std::string event_identity(const Event& event)
{
    // The native entry identifier is only unique within a session. Its
    // preserved envelope timestamp is essential to the native copy identity.
    return addon::serialize_json(Json(Json::Array{Json(event.entry_id), Json(event.timestamp)}));
}

Json encode_event(const Event& event)
{
    return json::object({{"entry_id", Json(event.entry_id)}, {"timestamp", Json(event.timestamp)},
        {"provider", Json(event.provider)}, {"model", Json(event.model)}, {"api", Json(event.api)},
        {"kind", Json(event.kind)}, {"response_id", event.response_id ? Json(*event.response_id) : Json()},
        {"accounting", addon::encode_stored_usage_accounting(event.accounting)}});
}

void validate_event(const Event& event)
{
    const auto& record = event.accounting;
    const auto expected_model = event.provider + "/" + event.model;
    if (record.record_id != "omp-event:" + digest(event_identity(event)) ||
        record.completed_date != date_for(event.timestamp) ||
        record.semantics != addon::Usage_counter_semantics::DELTA_EVENT ||
        record.coverage_unit != addon::Usage_coverage_unit::RECORDS || record.session_key ||
        record.repository_path || record.repository_usage ||
        record.model != (expected_model.size() <= 256 ? std::optional(expected_model) : std::nullopt) ||
        record.counter_field != "usage.totalTokens" || record.source_counter_semantics != "per_event_sum" ||
        (event.kind != "assistant" && (!event.kind.starts_with("model_usage:") ||
            event.kind == "model_usage:" || event.kind == "model_usage:discarded-turn")))
        throw Usage_error("usage_cache_evidence_invalid");
    for (const auto* amount : {&record.input, &record.output, &record.cache_read, &record.cache_creation}) {
        if (!amount->applicable || !amount->value || !amount->coverage || amount->coverage->overflow ||
            amount->coverage->total_records != 1 ||
            (amount->coverage->support == addon::Usage_support::SUPPORTED ? amount->coverage->covered_records != 1 :
                amount->coverage->support != addon::Usage_support::UNKNOWN ||
                amount->coverage->covered_records != 0 || *amount->value != 0))
            throw Usage_error("usage_cache_evidence_invalid");
    }
    addon::Contract_failure failure;
    if (!addon::decode_stored_usage_accounting(addon::encode_stored_usage_accounting(record), &failure))
        throw Usage_error("usage_native_accounting_invalid");
}

Event decode_event(const Json& value)
{
    if (!json::exact_keys(value, {"entry_id", "timestamp", "provider", "model", "api", "kind", "response_id", "accounting"}))
        throw Usage_error("usage_cache_evidence_invalid");
    Event result;
    result.entry_id = required_text(value, "entry_id", 128);
    result.timestamp = required_text(value, "timestamp", 24);
    result.provider = required_text(value, "provider");
    result.model = required_text(value, "model", 1024);
    result.api = required_text(value, "api");
    result.kind = required_text(value, "kind", 1024);
    if (!json::member(value, "response_id").is_null()) result.response_id = required_text(value, "response_id", 1024);
    addon::Contract_failure failure;
    const auto accounting = addon::decode_stored_usage_accounting(json::member(value, "accounting"), &failure);
    if (!accounting) throw Usage_error("usage_cache_evidence_invalid");
    result.accounting = *accounting;
    validate_event(result);
    return result;
}

addon::Usage_amount amount(const Json& usage, const char* name)
{
    const auto value = count(usage, name);
    return {true, value.value_or(0), addon::Usage_field_coverage{
        value ? 1ULL : 0ULL, 1, false, value ? addon::Usage_support::SUPPORTED : addon::Usage_support::UNKNOWN}};
}

Event native_event(const Json& entry, const Json& message, const fs::path& source, std::string kind)
{
    Event event;
    event.entry_id = required_text(entry, "id", 128);
    event.timestamp = required_text(entry, "timestamp", 24);
    event.provider = required_text(message, "provider");
    event.model = required_text(message, "model", 1024);
    event.api = required_text(message, "api");
    event.kind = std::move(kind);
    if (message.find("responseId")) event.response_id = required_text(message, "responseId", 1024);
    const auto& usage = json::member(message, "usage");
    if (!usage.is_object()) throw Usage_error("usage_native_counter_unavailable");
    const auto total = count(usage, "totalTokens");
    if (!total) throw Usage_error("usage_native_counter_unavailable");
    auto& record = event.accounting;
    record.record_id = "omp-event:" + digest(event_identity(event));
    record.completed_date = date_for(event.timestamp);
    record.total_tokens = *total;
    record.source_path = addon::path_to_utf8(source);
    record.semantics = addon::Usage_counter_semantics::DELTA_EVENT;
    record.input = amount(usage, "input");
    record.output = amount(usage, "output");
    record.cache_read = amount(usage, "cacheRead");
    record.cache_creation = amount(usage, "cacheWrite");
    const auto model = event.provider + "/" + event.model;
    if (model.size() <= 256) record.model = model;
    record.counter_field = "usage.totalTokens";
    record.source_counter_semantics = "per_event_sum";
    validate_event(event);
    return event;
}

struct Ledger
{
    std::map<std::string, Event, std::less<>> events;
    std::uint64_t bytes = 0;
    Usage_limits limits;

    void merge(Event incoming)
    {
        const auto identity = incoming.accounting.record_id;
        const auto found = events.find(identity);
        if (found == events.end()) {
            if (events.size() >= limits.events) throw Usage_error("usage_history_capacity");
            const auto size = addon::serialize_json(encode_event(incoming)).size() + 1;
            if (bytes > limits.ledger_bytes || size > limits.ledger_bytes - bytes) throw Usage_error("usage_history_capacity");
            bytes += size;
            events.emplace(identity, std::move(incoming));
            return;
        }
        auto& retained = found->second;
        if (retained.entry_id != incoming.entry_id || retained.timestamp != incoming.timestamp ||
            retained.provider != incoming.provider || retained.model != incoming.model || retained.api != incoming.api ||
            retained.kind != incoming.kind || retained.accounting.total_tokens != incoming.accounting.total_tokens ||
            (retained.response_id && incoming.response_id && retained.response_id != incoming.response_id))
            throw Usage_error("usage_native_event_conflict");
        const auto old_size = addon::serialize_json(encode_event(retained)).size();
        // Missing fields are not contradictions and are never fabricated from
        // another type's defaults. Only independently observed amounts refine
        // unknown dimensions; observed amounts themselves are immutable.
        for (const auto member : {&addon::Usage_record::input, &addon::Usage_record::output,
                &addon::Usage_record::cache_read, &addon::Usage_record::cache_creation}) {
            auto& previous = retained.accounting.*member;
            const auto& current = incoming.accounting.*member;
            const bool known = previous.coverage->support == addon::Usage_support::SUPPORTED;
            const bool observed = current.coverage->support == addon::Usage_support::SUPPORTED;
            if (known && observed && previous.value != current.value) throw Usage_error("usage_native_event_conflict");
            if (!known && observed) previous = current;
        }
        if (!retained.response_id && incoming.response_id) retained.response_id = incoming.response_id;
        const auto new_size = addon::serialize_json(encode_event(retained)).size();
        if (new_size > old_size) {
            if (bytes > limits.ledger_bytes || new_size - old_size > limits.ledger_bytes - bytes)
                throw Usage_error("usage_history_capacity");
            bytes += new_size - old_size;
        }
    }
};

bool path_exists(const fs::path& path)
{
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory) return false;
    if (error) throw Usage_error("usage_source_unavailable");
    return fs::exists(status);
}

std::string comparable_path(const fs::path& path)
{
    auto value = addon::path_to_utf8(fs::weakly_canonical(path).lexically_normal());
#ifdef _WIN32
    for (auto& byte : value) if (byte >= 'A' && byte <= 'Z') byte += 'a' - 'A';
#endif
    return value;
}

bool overlaps(const fs::path& left, const fs::path& right)
{
    const auto first = comparable_path(left);
    const auto second = comparable_path(right);
    const auto below = [](const std::string& child, const std::string& parent) {
        return child == parent || (child.starts_with(parent) && child.size() > parent.size() &&
            (!parent.empty() && (parent.back() == '/' || parent.back() == '\\') ||
                child[parent.size()] == '/' || child[parent.size()] == '\\'));
    };
    return below(first, second) || below(second, first);
}

void read_native_file(const fs::path& path, Ledger& ledger, Progress& progress)
{
    progress.check();
    if (fs::file_size(path) > ledger.limits.source_file_bytes) throw Usage_error("usage_source_capacity");
    bool header = false;
    bool title = false;
    std::map<std::string, std::string, std::less<>> identifiers;
    std::optional<Event> discarded;
    Json discarded_parent;
    addon::Contract_failure failure;
    const auto snapshot = addon::read_conversation_jsonl(path, [&](const Json& entry, addon::Contract_failure*) {
        progress.check();
        if (++progress.examined > ledger.limits.source_entries) throw Usage_error("usage_source_capacity");
        const auto type = required_text(entry, "type");
        if (!header && !title && type == "title") { title = true; return true; }
        if (!header) {
            const auto* version = entry.find("version");
            if (type != "session" || !version || !version->is_integer() || version->as_integer() != 3)
                throw Usage_error("usage_native_format_unsupported");
            (void)required_text(entry, "id", 256);
            header = true;
            return true;
        }
        const auto id = required_text(entry, "id", 128);
        const auto timestamp = required_text(entry, "timestamp", 24);
        const auto* parent = entry.find("parentId");
        if (!parent || (!parent->is_null() && !json::bounded_string(*parent, 128)))
            throw Usage_error("usage_native_identity_invalid");
        const auto [found, inserted] = identifiers.emplace(id, timestamp);
        if (!inserted && found->second != timestamp) throw Usage_error("usage_native_event_conflict");
        if (discarded) {
            const auto& details = json::member(entry, "details");
            if (type != "branch_summary" || json::text(details, "kind") != "discarded-entry-branch" ||
                *parent != discarded_parent) throw Usage_error("usage_discard_alias_unavailable");
            const auto original = required_text(details, "discardedEntryId", 128);
            if (original == discarded->entry_id || identifiers.contains(original))
                throw Usage_error("usage_discard_alias_unavailable");
            discarded->entry_id = original;
            discarded->kind = "assistant";
            discarded->accounting.record_id = "omp-event:" + digest(event_identity(*discarded));
            validate_event(*discarded);
            ledger.merge(std::move(*discarded));
            discarded.reset();
        }
        if (type == "message") {
            const auto& message = json::member(entry, "message");
            const auto role = required_text(message, "role");
            if (role == "assistant") ledger.merge(native_event(entry, message, path, "assistant"));
            // Task result usage summarizes child calls. Their native JSONL
            // entries are independent sources and must not be counted twice.
        }
        else if (type == "model_usage") {
            const auto purpose = required_text(entry, "purpose");
            auto event = native_event(entry, entry, path,
                purpose == "discarded-turn" ? "assistant" : "model_usage:" + purpose);
            if (purpose == "discarded-turn") {
                const auto* leaf = entry.find("activeLeafId");
                if (!leaf || *leaf != *parent) throw Usage_error("usage_discard_alias_unavailable");
                discarded_parent = *parent;
                discarded = std::move(event);
            }
            else ledger.merge(std::move(event));
        }
        else {
            constexpr std::string_view known[] = {"thinking_level_change", "model_change", "service_tier_change",
                "compaction", "branch_summary", "custom", "custom_message", "label", "title_change", "ttsr_injection",
                "session_init", "mode_change", "credential_pin", "reset_boundary"};
            if (std::find(std::begin(known), std::end(known), type) == std::end(known))
                throw Usage_error("usage_native_format_unsupported");
        }
        return true;
    }, [&] { return progress.cancelled(); }, &failure);
    if (!snapshot) throw Usage_error(failure.reason_code == "cancelled" ? "cancelled" : "usage_source_unavailable");
    if (!header) throw Usage_error("usage_native_format_unsupported");
    if (discarded) throw Usage_error("usage_discard_alias_unavailable");
}

std::set<fs::path> native_sources(const Native_profile& profile, const addon::Usage_scan_request& request,
    const Usage_limits& limits, Progress& progress)
{
    std::set<fs::path> files;
    std::set<fs::path> roots{profile.data / "sessions"};
    if (profile.explicit_session_directory) roots.insert(*profile.explicit_session_directory);
    const auto registry = profile.state / "custom-session-files";
    const auto& selection = *request.cache_selection;
    const auto refuse_cache_overlap = [&](const fs::path& source) {
        if (overlaps(source, addon::path_from_utf8(selection.candidate_root)) ||
            (selection.accepted && overlaps(source, addon::path_from_utf8(selection.accepted->root))))
            throw Usage_error("usage_storage_scope_conflict");
    };
    const auto add_file = [&](const fs::path& path) {
        progress.check();
        refuse_cache_overlap(path);
        if (!path_exists(path)) return; // Deleted history remains explicitly incomplete.
        if (!fs::is_regular_file(fs::symlink_status(path))) throw Usage_error("usage_source_unavailable");
        if (path.extension() == ".jsonl") files.insert(fs::canonical(path));
    };
    if (profile.explicit_session_file) add_file(*profile.explicit_session_file);
    refuse_cache_overlap(registry);
    if (path_exists(registry)) {
        if (!fs::is_directory(fs::symlink_status(registry))) throw Usage_error("usage_source_unavailable");
        for (const auto& item : fs::directory_iterator(registry)) {
            progress.check();
            if (++progress.examined > limits.source_entries) throw Usage_error("usage_source_capacity");
            if (!fs::is_regular_file(item.symlink_status()) || item.file_size() > 32768)
                throw Usage_error("usage_source_unavailable");
            std::ifstream input(item.path(), std::ios::binary);
            const std::string value((std::istreambuf_iterator<char>(input)), {});
            if (!input.eof() && input.fail()) throw Usage_error("usage_source_unavailable");
            if (!addon::is_valid_utf8(value) || value.empty() || value.find('\0') != std::string::npos)
                throw Usage_error("usage_native_registry_invalid");
            const auto path = addon::path_from_utf8(value);
            if (!path.is_absolute()) throw Usage_error("usage_native_registry_invalid");
            add_file(path);
            if (path.extension() == ".jsonl") roots.insert(path.parent_path() / path.stem());
        }
    }
    if (profile.explicit_session_file) roots.insert(profile.explicit_session_file->parent_path() / profile.explicit_session_file->stem());
    for (const auto& root : roots) {
        progress.check();
        refuse_cache_overlap(root);
        if (!path_exists(root)) continue;
        if (!fs::is_directory(fs::symlink_status(root))) throw Usage_error("usage_source_unavailable");
        for (auto iterator = fs::recursive_directory_iterator(root); iterator != fs::recursive_directory_iterator(); ++iterator) {
            progress.check();
            if (++progress.examined > limits.source_entries) throw Usage_error("usage_source_capacity");
            const auto status = iterator->symlink_status();
            if (fs::is_symlink(status)) { iterator.disable_recursion_pending(); continue; }
            if (fs::is_directory(status)) {
                if (addon::path_to_utf8(iterator->path().filename()).starts_with(".omp-gc-compress-"))
                    iterator.disable_recursion_pending();
            }
            else if (fs::is_regular_file(status) && iterator->path().extension() == ".jsonl") add_file(iterator->path());
        }
    }
    return files;
}

Json cache_owner(const Native_profile& profile)
{
    return json::object({{"type", Json("omp-native-usage-owner")}, {"version", Json(1)},
        {"source_profile", Json(addon::path_to_utf8(profile.source_profile))}, {"account_key", Json(profile.account_key)}});
}

void read_retained(const addon::Usage_scan_request& request, const Native_profile& profile, Ledger& ledger, Progress& progress)
{
    const auto& selected = request.cache_selection->accepted;
    if (!selected) return;
    addon::Contract_failure failure;
    const auto manifest = addon::validate_usage_cache_candidate(*selected, request.context.profile_storage->root,
        request.context.account_context, [&] { return progress.cancelled(); }, &failure);
    if (!manifest) throw Usage_error(failure.reason_code);
    if (manifest->format != k_format || manifest->files.size() != 1 || manifest->files.front().path != "ledger.jsonl")
        throw Usage_error("usage_cache_format_unsupported");
    if (manifest->files.front().bytes > ledger.limits.ledger_bytes + 32768) throw Usage_error("usage_history_capacity");
    bool owner = false;
    std::string previous;
    const auto snapshot = addon::read_conversation_jsonl(
        addon::usage_cache_io_path(addon::path_from_utf8(selected->root)) / "ledger.jsonl",
        [&](const Json& record, addon::Contract_failure*) {
            progress.check();
            if (!owner) {
                if (record != cache_owner(profile)) throw Usage_error("usage_cache_owner_mismatch");
                owner = true;
            }
            else {
                auto event = decode_event(record);
                if (!previous.empty() && event.accounting.record_id <= previous)
                    throw Usage_error("usage_cache_evidence_invalid");
                previous = event.accounting.record_id;
                ledger.merge(std::move(event));
            }
            return true;
        }, [&] { return progress.cancelled(); }, &failure);
    if (!snapshot && failure.reason_code == "cancelled") throw Usage_error("cancelled");
    if (!snapshot || !owner || snapshot->incomplete_tail_omitted) throw Usage_error("usage_cache_evidence_invalid");
}

void write_candidate(const addon::Usage_scan_request& request, const Native_profile& profile,
    const Ledger& ledger, Progress& progress)
{
    const auto path = addon::usage_cache_io_path(addon::path_from_utf8(request.cache_selection->candidate_root)) / "ledger.jsonl";
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw Usage_error("usage_storage_write_failed");
    output << addon::serialize_json(cache_owner(profile)) << '\n';
    for (const auto& [key, event] : ledger.events) {
        (void)key;
        progress.check();
        output << addon::serialize_json(encode_event(event)) << '\n';
        if (!output) throw Usage_error("usage_storage_write_failed");
    }
    output.close();
    if (!output) throw Usage_error("usage_storage_write_failed");
    // The SDK seals and fsyncs the complete inventory before any page escapes.
}

struct Snapshot
{
    std::string owner;
    std::string identifier;
    addon::Observation_scope scope;
    addon::Usage_cache_reference cache;
    std::vector<addon::Usage_record> records;
    std::uint64_t bytes = 0;
    mutable std::set<std::size_t> issued_cursors;
};

std::timed_mutex snapshots_mutex;
std::map<std::string, std::shared_ptr<const Snapshot>> snapshots;
std::vector<std::string> snapshot_order;

std::shared_ptr<Snapshot> build_snapshot(const addon::Usage_scan_request& request, const std::string& owner,
    Usage_limits limits, Progress& progress)
{
    addon::Contract_failure failure;
    const auto& context = request.context;
    const auto& selection = *request.cache_selection;
    if (!context.launch_environment) throw Usage_error("native_profile_environment_unavailable");
    const auto profile = resolve_native_profile(context.launcher, *context.launch_environment,
        context.working_directory, {}, context.launcher_argv, [&] { return progress.cancelled(); }, &failure);
    if (!profile) throw Usage_error(failure.reason_code);
    if (context.account_context && (context.account_context->adapter_account_key != profile->account_key ||
        (context.account_context->version == 2 && context.account_context->profile_path != addon::path_to_utf8(profile->source_profile))))
        throw Usage_error("account_context_mismatch");
    if (!addon::validate_usage_cache_allocation(selection.candidate_root, context.profile_storage->root, &failure))
        throw Usage_error(failure.reason_code);
    Ledger ledger{{}, 0, limits};
    read_retained(request, *profile, ledger, progress);
    const auto files = native_sources(*profile, request, limits, progress);
    for (const auto& path : files) read_native_file(path, ledger, progress);
    progress.check();
    auto result = std::make_shared<Snapshot>();
    result->owner = owner;
    result->identifier = addon::random_nonce_hex();
    result->scope.source_account_key = context.account_context ?
        std::optional(context.account_context->adapter_account_key) : std::nullopt;
    result->scope.observed_at_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    result->scope.fresh_for_ms = 0;
    write_candidate(request, *profile, ledger, progress);
    const auto cache = addon::seal_usage_cache_candidate(selection.candidate_root, context.profile_storage->root,
        context.account_context, result->identifier, k_format, [&] { return progress.cancelled(); }, &failure);
    if (!cache) throw Usage_error(failure.reason_code);
    result->cache = *cache;
    result->records.reserve(ledger.events.size());
    for (const auto& [key, event] : ledger.events) {
        (void)key;
        progress.check();
        auto record = event.accounting;
        record.aggregate_evidence = "complete";
        record.cross_source_event = addon::Usage_cross_source_event{k_event_namespace, event_identity(event)};
        if (!addon::decode_usage_record(addon::encode_usage_record(record), &failure)) throw Usage_error("usage_native_accounting_invalid");
        const auto bytes = addon::serialize_json(addon::encode_usage_record(record)).size();
        if (result->bytes > limits.ledger_bytes || bytes > limits.ledger_bytes - result->bytes) throw Usage_error("usage_history_capacity");
        result->bytes += bytes;
        result->records.push_back(std::move(record));
    }
    return result;
}

} // namespace

void handle_usage(addon::Invocation_context& context, Usage_limits limits)
{
    if (context.request().operation != "scan") { context.reject_unknown_operation(); return; }
    addon::Contract_failure failure;
    const auto request = addon::decode_usage_scan_request(context.request().payload, &failure);
    if (!request) { context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail); return; }
    try {
        Progress progress(context);
        progress.check();
        auto identity = *request;
        identity.snapshot_id.reset();
        identity.cursor.reset();
        const auto owner = digest(addon::serialize_json_canonical(addon::encode_usage_scan_request(identity)));
        std::unique_lock lock(snapshots_mutex, std::defer_lock);
        while (!lock.try_lock_for(std::chrono::milliseconds(100))) progress.check();
        std::shared_ptr<const Snapshot> snapshot;
        std::size_t begin = 0;
        if (request->snapshot_id) {
            const auto& cursor = *request->cursor;
            const auto parsed = std::from_chars(cursor.data(), cursor.data() + cursor.size(), begin);
            if (parsed.ec != std::errc{} || parsed.ptr != cursor.data() + cursor.size() || std::to_string(begin) != cursor)
                throw Usage_error("usage_cursor_invalid");
            const auto found = snapshots.find(*request->snapshot_id);
            if (found == snapshots.end() || found->second->owner != owner || begin == 0 || begin >= found->second->records.size())
                throw Usage_error("usage_snapshot_expired");
            if (!found->second->issued_cursors.contains(begin)) throw Usage_error("usage_cursor_invalid");
            snapshot = found->second;
        }
        else {
            auto candidate = build_snapshot(*request, owner, limits, progress);
            while (!snapshot_order.empty()) {
                std::uint64_t bytes = candidate->bytes;
                for (const auto& [key, value] : snapshots) { (void)key; bytes += value->bytes; }
                if (snapshot_order.size() < 2 && bytes <= limits.ledger_bytes) break;
                snapshots.erase(snapshot_order.front());
                snapshot_order.erase(snapshot_order.begin());
            }
            snapshots.emplace(candidate->identifier, candidate);
            snapshot_order.push_back(candidate->identifier);
            snapshot = std::move(candidate);
        }
        addon::Usage_scan_page page;
        page.scope = snapshot->scope;
        page.snapshot_id = snapshot->identifier;
        page.cache_candidate = snapshot->cache;
        page.account_context = request->context.account_context;
        page.profile_storage = request->context.profile_storage;
        // Native deletion, no-session runs, remote storage and compressed or
        // unavailable child histories prevent a claim of full lifetime coverage.
        page.incomplete_historical_coverage = true;
        std::size_t bytes = 0;
        std::size_t end = begin;
        while (end < snapshot->records.size()) {
            progress.check();
            page.complete = end + 1 == snapshot->records.size();
            page.next_cursor = page.complete ? std::nullopt : std::optional(std::to_string(end + 1));
            if (!addon::append_usage_page_record(page, snapshot->records[end], bytes)) break;
            ++end;
        }
        if (end == begin && end < snapshot->records.size()) throw Usage_error("usage_record_too_large");
        page.complete = end == snapshot->records.size();
        page.next_cursor = page.complete ? std::nullopt : std::optional(std::to_string(end));
        const auto encoded = addon::encode_usage_scan_result(page);
        if (!addon::decode_usage_scan_result(encoded, &failure)) throw Usage_error("usage_page_invalid");
        progress.check();
        if (!page.complete) snapshot->issued_cursors.insert(end);
        lock.unlock();
        context.succeed(encoded);
    }
    catch (const Usage_error& error) {
        context.fail(std::string_view(error.what()) == "cancelled" ? addon::Result_status::CANCELLED : addon::Result_status::FAILED,
            error.what());
    }
    catch (const std::exception&) {
        context.fail(addon::Result_status::FAILED, "usage_source_unavailable");
    }
}

} // namespace omp
