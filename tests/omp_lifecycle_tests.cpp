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

} // namespace

int main()
{
    try {
        sessions();
        root_turns();
        attention_and_children();
        std::puts("PASS omp native lifecycle and SDK feature contracts");
        return 0;
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
