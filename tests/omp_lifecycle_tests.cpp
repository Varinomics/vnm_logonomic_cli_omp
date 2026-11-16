#include "omp_lifecycle.h"

#include <logonomic_cli_addon/json_utilities.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace {
namespace addon = logonomic::cli_addon;
namespace json = addon::json;
constexpr const char* nonce_a = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr const char* nonce_b = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

addon::Lifecycle_features declared_features()
{
    const auto path = std::filesystem::path(__FILE__).parent_path().parent_path() /
        "package/capabilities/lifecycle-events@2.json";
    std::ifstream input(path);
    const std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    const auto sidecar = json::parse(bytes);
    require(sidecar.has_value(), "The shipped lifecycle sidecar must parse");
    const auto declaration = addon::decode_lifecycle_declaration(json::member(*sidecar, "payload"), nullptr);
    require(declaration.has_value(), "The shipped lifecycle profile must satisfy the SDK contract");
    return declaration->features;
}

addon::Classify_event_request event(std::string name)
{
    addon::Classify_event_request request;
    request.event.name = std::move(name);
    request.event.body = json::object({
        {"session_id", addon::Json_value("session-A")},
        {"activation_nonce", addon::Json_value(nonce_a)},
        {"event_id", addon::Json_value("native-event-1")},
        {"sequence", addon::Json_value("1")},
        {"turn_id", addon::Json_value("turn-1")}});
    request.transition_nonce = nonce_a;
    request.ledger = {1, addon::Ledger_transition{addon::Transition_kind::START, "session-A", nonce_a}};
    return request;
}

addon::Attention_classification_result checked(const addon::Classify_event_request& request)
{
    const auto result = omp::classify_native_event(request);
    const auto encoded = addon::encode_attention_classification_result(result);
    require(addon::decode_attention_classification_result(encoded, nullptr).has_value(),
        "Every emitted result must pass the SDK attention/helper envelope contract");
    for (const auto& observation : result.base.observations) {
        require(addon::lifecycle_profile_admits(declared_features(), observation),
            "Every emitted observation must be admitted by the shipped feature declaration");
    }
    return result;
}

void sessions()
{
    auto start = event("session_start");
    start.ledger = {};
    start.event.body.set("source", addon::Json_value("load"));
    start.event.body.set("session_file", addon::Json_value("/sessions/exact.jsonl"));
    const auto first = checked(start);
    require(first.base.classification == addon::Classification::TRANSITION,
        "Native session load opens a root activation");
    require(first.base.observations.front().resume_record.has_value(),
        "The exact native session file is retained with the native session id");

    for (const auto& pair : {std::pair{"session_switch", "resume"}, {"session_clear", "clear"},
             {"session_branch", "fork"}, {"session_tree", "tree"}}) {
        auto next = event(pair.first);
        next.event.body.set("source", addon::Json_value(pair.second));
        next.event.body.set("previous_nonce", addon::Json_value(nonce_a));
        next.event.body.set("activation_nonce", addon::Json_value(nonce_b));
        next.transition_nonce = nonce_b;
        const auto changed = checked(next);
        require(changed.base.classification == addon::Classification::TRANSITION,
            "Successful clear/switch/branch/tree transitions replace activation even when the native id is unchanged");
        next.event.body.set("previous_nonce", addon::Json_value("cccccccccccccccccccccccccccccccc"));
        require(checked(next).base.classification == addon::Classification::IGNORED,
            "A delayed transition cannot replace a different activation");
    }
    auto duplicate = event("session_start");
    duplicate.event.body.set("source", addon::Json_value("load"));
    require(checked(duplicate).base.classification == addon::Classification::IGNORED,
        "Redelivery of the current transition cannot allocate another epoch");
    const auto end = checked(event("session_shutdown"));
    require(end.base.observations.front().session_transition == addon::Session_transition::ENDED,
        "Native shutdown ends the matching root session without inventing a turn result");
}

void root_turns()
{
    require(checked(event("agent_start")).base.observations.front().phase_transition == addon::Phase_transition::WORKING,
        "Native loop start supplies confirmed working evidence");
    require(checked(event("session_stop")).base.observations.empty(),
        "The native stop veto gate never supplies terminal evidence");
    auto end = event("agent_end");
    end.event.body.set("terminal", addon::Json_value(false));
    end.event.body.set("stop_reason", addon::Json_value("stop"));
    require(checked(end).base.observations.empty(), "A scheduled continuation must not complete the root turn");
    end.event.body.set("terminal", addon::Json_value(true));
    for (const auto& pair : {std::pair{"stop", addon::Outcome_transition::SUCCEEDED},
             {"error", addon::Outcome_transition::FAILED}, {"aborted", addon::Outcome_transition::INTERRUPTED},
             {"length", addon::Outcome_transition::NONE}, {"toolUse", addon::Outcome_transition::NONE}}) {
        end.event.body.set("stop_reason", addon::Json_value(pair.first));
        const auto result = checked(end);
        require(result.base.observations.front().outcome_transition == pair.second,
            "Only an affirmative native stop succeeds; errors, aborts and ambiguous limits keep their meaning");
        require(result.base.observations.front().phase_evidence == addon::Evidence::CONFIRMED,
            "A terminal native loop settle supplies confirmed idle evidence");
    }
    end.event.body.set("activation_nonce", addon::Json_value(nonce_b));
    require(checked(end).base.observations.empty(), "A late old-root completion cannot enter the replacement epoch");
}

void attention_and_children()
{
    for (const auto* name : {"tool_approval_requested", "tool_approval_resolved", "plan_review_requested",
             "plan_review_resolved", "ask_execution_start", "ask_execution_end"}) {
        auto request = event(name);
        request.event.body.set("request_id", addon::Json_value("native-request-1"));
        const auto result = checked(request);
        require(result.base.observations.size() == 1, "Native request ids must survive both sides of an attention pair");
        const auto expected = std::string_view(name).starts_with("ask_") ? addon::Evidence::INFERRED : addon::Evidence::CONFIRMED;
        require(result.base.observations.front().attention_evidence == expected,
            "Tool execution only infers ask attention; explicit approval/plan events confirm it");
    }
    auto child = event("task:subagent:lifecycle");
    child.event.body.set("agent_id", addon::Json_value("0-Explore"));
    child.event.body.set("agent_label", addon::Json_value("explore"));
    for (const auto* status : {"started", "completed", "failed", "aborted"}) {
        child.event.body.set("status", addon::Json_value(status));
        const auto result = checked(child);
        const auto& observation = result.base.observations.front();
        require(observation.agent_id == "0-Explore" && observation.provider_turn_id.empty(),
            "Direct children retain native registry identity independently of the root turn");
        require(observation.phase_transition == addon::Phase_transition::NONE &&
            observation.outcome_transition == addon::Outcome_transition::NONE,
            "A direct child settling never completes or fails its root");
    }
    const auto loss = checked(event("logonomic_observation_lost"));
    require(loss.base.classification == addon::Classification::IGNORED && loss.base.lifecycle_degradation &&
        loss.base.observations.empty(), "Lost observations quarantine lifecycle correlation without fabricating completion");
    auto invalid = event("agent_start");
    invalid.event.body.set("sequence", addon::Json_value("01"));
    require(checked(invalid).base.observations.empty(), "Noncanonical sequence evidence must not cross the SDK boundary");
}

void context_and_execution()
{
    auto request = event("agent_start");
    request.event.body.set("context", json::object({
        {"used_tokens", addon::Json_value(17000)}, {"context_window", addon::Json_value(16000)}}));
#if defined(_WIN32)
    constexpr const char* native_cwd = "C:\\work\\nested dir";
#else
    constexpr const char* native_cwd = "/work/nested dir";
#endif
    request.event.body.set("execution", json::object({
        {"state", addon::Json_value("confirmed")}, {"cwd", addon::Json_value(native_cwd)}}));
    const auto unrequested = checked(request);
    require(unrequested.base.context_updates.empty() && unrequested.base.execution_updates.empty(),
        "Optional telemetry is omitted unless the host independently opts in");
    request.context_state_version = 2;
    request.execution_context_version = 1;
    const auto sampled = checked(request);
    require(sampled.base.observations.size() == 1 && sampled.base.context_updates.size() == 1 &&
        sampled.base.execution_updates.size() == 1, "One authenticated event can carry independent lifecycle and optional facts");
    const auto& context = sampled.base.context_updates.front();
    require(context.used_tokens == 17000 && context.context_window == 16000 && !context.compaction_count,
        "Native occupancy is not capped as tokens or presented as complete history");
    const auto& execution = sampled.base.execution_updates.front();
    require(execution.cwd == native_cwd && execution.transition_nonce == nonce_a && execution.sequence == 1,
        "Exact nested directory spelling retains its source activation and sequence");
    addon::Execution_context_state directory;
    directory.provider_session_id = "session-A";
    directory.transition_nonce = nonce_a;
    require(addon::apply_execution_context_update(directory, execution), "The SDK host admits a current exact directory");
    require(!addon::apply_execution_context_update(directory, execution), "The SDK rejects duplicate execution sequence");

    auto malformed = request;
    malformed.event.body.set("context", json::object({{"used_tokens", addon::Json_value(-1)}}));
    malformed.event.body.set("execution", json::object({
        {"state", addon::Json_value("confirmed")}, {"cwd", addon::Json_value("relative-dir")}}));
    const auto rejected_optional = checked(malformed);
    require(rejected_optional.base.observations.size() == 1 && rejected_optional.base.context_failure &&
        rejected_optional.base.execution_failure && rejected_optional.base.context_updates.empty() &&
        rejected_optional.base.execution_updates.empty(),
        "Malformed optional facts fail only their capability beside a valid root start");

    request.event.name = "message_end";
    const auto context_only = checked(request);
    require(context_only.base.classification == addon::Classification::IGNORED &&
        context_only.base.observations.empty() && context_only.base.context_updates.size() == 1,
        "An assistant context sample does not settle or start a root turn");
    request.event.body.set("activation_nonce", addon::Json_value(nonce_b));
    require(checked(request).base.context_updates.empty() && checked(request).base.execution_updates.empty(),
        "Old-activation optional observations cannot replace the active native conversation");
    request.event.body.set("activation_nonce", addon::Json_value(nonce_a));
    request.event.body.set("sequence", addon::Json_value("01"));
    require(checked(request).base.context_updates.empty(), "Optional-only observations still require valid source sequence");
    request.event.body.set("sequence", addon::Json_value("2"));
    request.event.name = "session_compact";
    request.event.body.set("context", json::object({
        {"usage_unavailable", addon::Json_value(true)}, {"compaction_id", addon::Json_value("native-entry-42")}}));
    const auto compacted = checked(request).base.context_updates.front();
    require(compacted.compacted && compacted.usage_unavailable && compacted.event_id == "compaction:native-entry-42",
        "Installed compaction uses its stable native entry identity and can clear unavailable post-rewrite occupancy");
    addon::Context_state occupancy;
    addon::apply_context_update(occupancy, context);
    addon::apply_context_update(occupancy, compacted);
    addon::apply_context_update(occupancy, compacted);
    require(occupancy.compaction_count == 1 && !occupancy.compaction_count_complete && !occupancy.used_tokens,
        "SDK deduplicates successful compaction without inventing complete resumed history or stale occupancy");
    request.event.name = "auto_compaction_end";
    require(checked(request).base.context_updates.empty(), "An automatic attempt-ending notification cannot double count success");
    request.event.name = "message_end";
    require(checked(request).base.context_failure.has_value(), "Compaction identity cannot promote a different native event to success");

    auto start = event("session_clear");
    start.context_state_version = 2;
    start.execution_context_version = 1;
    start.event.body.set("source", addon::Json_value("clear"));
    start.event.body.set("activation_nonce", addon::Json_value(nonce_b));
    start.event.body.set("previous_nonce", addon::Json_value(nonce_a));
    start.transition_nonce = nonce_b;
    start.event.body.set("context", json::object({{"usage_unavailable", addon::Json_value(true)}}));
    start.event.body.set("execution", json::object({
        {"state", addon::Json_value("unknown")}, {"cwd", addon::Json_value("")}}));
    const auto cleared = checked(start);
    require(cleared.base.classification == addon::Classification::TRANSITION &&
        cleared.base.execution_updates.front().transition_nonce == nonce_b &&
        cleared.base.context_updates.front().usage_unavailable,
        "Accepted clear and its optional unknown snapshots share the new activation atomically");
}

} // namespace

int main()
{
    try {
        sessions();
        root_turns();
        attention_and_children();
        context_and_execution();
        std::puts("PASS omp native lifecycle and SDK feature contracts");
        return 0;
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
