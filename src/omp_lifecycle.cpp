#include "omp_lifecycle.h"
#include "omp_telemetry.h"

#include <logonomic_cli_addon/json_utilities.h>

#include <charconv>
#include <string_view>
#include <utility>

namespace omp {
namespace addon = logonomic::cli_addon;
namespace json = addon::json;
namespace {

bool identity(const addon::Json_value& body, std::string_view name, bool optional = false)
{
    const auto* value = body.find(name);
    if (!value) {
        return optional;
    }
    if (!json::bounded_string(*value, 256, optional)) {
        return false;
    }
    for (const unsigned char ch : value->as_string()) {
        if (ch < 0x20 || ch == 0x7f) {
            return false;
        }
    }
    return true;
}

bool matches_activation(const addon::Classify_event_request& request)
{
    return request.ledger.current_transition &&
        request.ledger.current_transition->provider_session_id == json::text(request.event.body, "session_id") &&
        request.ledger.current_transition->nonce == json::text(request.event.body, "activation_nonce");
}

bool sequence(const addon::Json_value& body)
{
    const auto value = json::text(body, "sequence");
    if (value.empty() || (value.size() > 1 && value.front() == '0')) {
        return false;
    }
    std::uint64_t parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    return result.ec == std::errc{} && result.ptr == value.data() + value.size();
}

addon::Attention_classification_result classify_nested(const addon::Classify_event_request& request)
{
    addon::Attention_classification_result result;
    if (request.child_state_version != 1) {
        return result;
    }
    const auto invalid = [] {
        addon::Attention_classification_result rejected;
        rejected.base.child_failure = addon::Contract_failure{"observation_invalid",
            "Native nested child snapshot lacks bounded execution and identity evidence"};
        return rejected;
    };
    const auto& rows = json::member(request.event.body, "children");
    if (!rows.is_array() || rows.as_array().empty() || rows.as_array().size() > 8) {
        return invalid();
    }
    std::size_t index = 0;
    for (const auto& row : rows.as_array()) {
        if (!json::only_keys(row, {"agent_id", "agent_label", "child", "model", "transition", "sequence",
                "start_sequence", "execution_observed"}) || !identity(row, "agent_id") || !sequence(row)) {
            return invalid();
        }
        const auto& executed = json::member(row, "execution_observed");
        const auto action = json::text(row, "transition");
        const auto& child = json::member(row, "child");
        const auto state = json::text(child, "state");
        const bool terminal = state == "succeeded" || state == "failed" || state == "interrupted";
        const auto& depth = json::member(child, "depth");
        if (!executed.is_boolean() || !executed.as_boolean() ||
            !identity(child, "parent_agent_id") ||
            json::text(child, "parent_agent_id") == json::text(row, "agent_id") ||
            !depth.is_integer() || depth.as_integer() < 2 || depth.as_integer() > 8 ||
            ((action == "start" || action == "none") ? state != "running" :
                (action != "stop" && action != "start_stop") || !terminal)) {
            return invalid();
        }
        const auto row_sequence = json::text(row, "sequence");
        const auto start_sequence = json::text(row, "start_sequence");
        if (action == "start_stop") {
            const auto start_body = json::object({{"sequence", addon::Json_value(start_sequence)}});
            if (!sequence(start_body) || !(start_sequence.size() < row_sequence.size() ||
                    (start_sequence.size() == row_sequence.size() && start_sequence < row_sequence))) {
                return invalid();
            }
        }
        else
        if (row.find("start_sequence")) {
            return invalid();
        }
        auto native = request;
        native.event.body = json::object({
            {"session_id", json::member(request.event.body, "session_id")},
            {"activation_nonce", json::member(request.event.body, "activation_nonce")},
            {"event_id", addon::Json_value(json::text(request.event.body, "event_id") + ":" + std::to_string(index++))},
            {"sequence", addon::Json_value(row_sequence)},
            {"agent_id", json::member(row, "agent_id")}, {"child", child}});
        if (const auto* model = row.find("model")) {
            native.event.body.set("model", *model);
        }
        for (const bool starts : {true, false}) {
            if (starts ? (action != "start" && action != "start_stop") : (action != "stop" && action != "start_stop")) {
                continue;
            }
            addon::Lifecycle_observation observation;
            observation.provider_event_name = request.event.name;
            observation.provider_session_id = json::text(native.event.body, "session_id");
            observation.provider_event_id = json::text(native.event.body, "event_id") + (starts ? ":start" : ":stop");
            observation.provider_sequence = starts && action == "start_stop" ? start_sequence : row_sequence;
            observation.agent_id = json::text(row, "agent_id");
            if (const auto* label = row.find("agent_label"); label && json::bounded_string(*label, 160, true)) {
                observation.agent_label = label->as_string();
            }
            observation.subagent_transition = starts ? addon::Subagent_transition::STARTED : addon::Subagent_transition::STOPPED;
            if (!addon::decode_lifecycle_observation(addon::encode_lifecycle_observation(observation), nullptr,
                    addon::Lifecycle_observation_policy::HELPER_INGRESS)) {
                return invalid();
            }
            result.base.observations.push_back(std::move(observation));
        }
        addon::Classify_event_result telemetry;
        append_native_telemetry(native, telemetry);
        result.base.child_updates.insert(result.base.child_updates.end(), telemetry.child_updates.begin(), telemetry.child_updates.end());
        result.base.model_updates.insert(result.base.model_updates.end(), telemetry.model_updates.begin(), telemetry.model_updates.end());
        if (telemetry.child_failure) {
            result.base.child_failure = std::move(telemetry.child_failure);
        }
        if (telemetry.model_failure) {
            result.base.model_failure = std::move(telemetry.model_failure);
        }
    }
    if (result.base.child_failure) {
        result.base.child_updates.clear();
    }
    if (result.base.model_failure) {
        result.base.model_updates.clear();
    }
    result.base.classification = result.base.observations.empty() ? addon::Classification::IGNORED : addon::Classification::EVENT;
    return result;
}

} // namespace

addon::Attention_classification_result classify_native_event(const addon::Classify_event_request& request)
{
    addon::Attention_classification_result result;
    const auto& body = request.event.body;
    const auto& name = request.event.name;
    if (!json::only_keys(body, {"session_id", "activation_nonce", "previous_nonce", "event_id", "sequence",
            "turn_id", "source", "session_file", "terminal", "stop_reason", "request_id", "agent_id",
            "agent_label", "status", "context", "execution", "model", "child", "child_loss", "children"}) ||
        !identity(body, "session_id") || !identity(body, "event_id") ||
        !identity(body, "turn_id", true) || !identity(body, "request_id", true) ||
        !identity(body, "agent_id", true) || !sequence(body) ||
        !addon::is_valid_transition_nonce_hex(json::text(body, "activation_nonce"))) {
        return result;
    }

    addon::Lifecycle_observation observation;
    observation.provider_event_name = name;
    observation.provider_session_id = json::text(body, "session_id");
    observation.provider_turn_id    = json::text(body, "turn_id");
    observation.provider_event_id   = json::text(body, "event_id");
    observation.provider_sequence   = json::text(body, "sequence");

    const bool starts = name == "session_start" || name == "session_switch" ||
        name == "session_clear" || name == "session_branch" || name == "session_tree";
    if (starts) {
        const auto nonce = json::text(body, "activation_nonce");
        const auto previous = json::text(body, "previous_nonce");
        if (nonce != request.transition_nonce || matches_activation(request) ||
            (!previous.empty() && (!request.ledger.current_transition ||
                previous != request.ledger.current_transition->nonce))) {
            return result;
        }
        const auto source = json::text(body, "source");
        if (source != "load" && source != "new" && source != "resume" && source != "fork" &&
            source != "clear" && source != "branch" && source != "btw" && source != "tree") {
            return result;
        }
        observation.session_transition = addon::Session_transition::STARTED;
        observation.session_source = source;
        if (const auto* file = body.find("session_file"); file && json::bounded_string(*file, 8192)) {
            observation.resume_record = json::object({
                {"provider_session_id", addon::Json_value(observation.provider_session_id)},
                {"session_file", *file}});
        }
        result.base.classification = addon::Classification::TRANSITION;
        result.base.transition = source == "resume" ? addon::Transition_kind::RESUME : addon::Transition_kind::START;
        if (request.child_state_version == 1) {
            result.base.helper_response = json::object({{"child_state_version", addon::Json_value(1)}});
        }
    }
    else {
        if (!matches_activation(request)) {
            return result;
        }
        if (name == "logonomic_observation_lost") {
            result.base.lifecycle_degradation = addon::Lifecycle_degradation{1, observation.provider_session_id};
            return result;
        }
        if (name == "task:subagent:snapshot") {
            return classify_nested(request);
        }
        if (name == "message_end" || name == "session_compact" || name == "model_observation" || name == "task:subagent:progress" ||
            name == "retry_fallback_applied" || name == "retry_fallback_succeeded" || name == "task:subagent:loss") {
            // These optional native samples carry no turn/lifecycle inference.
            append_native_telemetry(request, result.base);
            return result;
        }
        if (name == "session_shutdown") {
            observation.session_transition = addon::Session_transition::ENDED;
            observation.session_end_reason = "shutdown";
        }
        else
        if (name == "agent_start" && !observation.provider_turn_id.empty()) {
            observation.phase_transition = addon::Phase_transition::WORKING;
            observation.phase_evidence = addon::Evidence::CONFIRMED;
        }
        else
        if (name == "agent_end" && !observation.provider_turn_id.empty()) {
            const auto& terminal = json::member(body, "terminal");
            if (!terminal.is_boolean() || !terminal.as_boolean()) {
                return result;
            }
            observation.phase_transition = addon::Phase_transition::AWAITING_INPUT;
            observation.phase_evidence = addon::Evidence::CONFIRMED;
            const auto reason = json::text(body, "stop_reason");
            if (reason == "stop") {
                observation.outcome_transition = addon::Outcome_transition::SUCCEEDED;
            }
            else
            if (reason == "error") {
                observation.outcome_transition = addon::Outcome_transition::FAILED;
            }
            else
            if (reason == "aborted") {
                observation.outcome_transition = addon::Outcome_transition::INTERRUPTED;
            }
            if (observation.outcome_transition != addon::Outcome_transition::NONE) {
                observation.outcome_evidence = addon::Evidence::CONFIRMED;
            }
        }
        else
        if (name == "tool_approval_requested" || name == "tool_approval_resolved" ||
            name == "plan_review_requested" || name == "plan_review_resolved" ||
            name == "ask_execution_start" || name == "ask_execution_end") {
            observation.attention_request_id = json::text(body, "request_id");
            if (observation.attention_request_id.empty()) {
                return result;
            }
            const bool requested = name == "tool_approval_requested" || name == "plan_review_requested" ||
                name == "ask_execution_start";
            observation.attention_transition = requested ? addon::Attention_transition::REQUESTED :
                addon::Attention_transition::RESOLVED;
            observation.attention_evidence = name.starts_with("ask_") ? addon::Evidence::INFERRED :
                addon::Evidence::CONFIRMED;
            if (requested) {
                result.attention_kind = addon::Attention_kind{0,
                    name == "tool_approval_requested" ? "permission" : "interactive_question"};
            }
        }
        else
        if (name == "task:subagent:lifecycle") {
            observation.agent_id = json::text(body, "agent_id");
            if (observation.agent_id.empty()) {
                return result;
            }
            const auto status = json::text(body, "status");
            if (status == "started") {
                observation.subagent_transition = addon::Subagent_transition::STARTED;
            }
            else
            if (status == "completed" || status == "failed" || status == "aborted") {
                observation.subagent_transition = addon::Subagent_transition::STOPPED;
            }
            else {
                return result;
            }
            if (const auto* label = body.find("agent_label"); label && json::bounded_string(*label, 160, true)) {
                observation.agent_label = label->as_string();
            }
            // A child exit, even a successful one, is never a root outcome.
            observation.provider_turn_id.clear();
        }
        else {
            return result;
        }
        result.base.classification = addon::Classification::EVENT;
    }

    addon::Contract_failure failure;
    if (!addon::decode_lifecycle_observation(addon::encode_lifecycle_observation(observation), &failure,
            addon::Lifecycle_observation_policy::HELPER_INGRESS)) {
        return {};
    }
    result.base.observations.push_back(std::move(observation));
    append_native_telemetry(request, result.base);
    return result;
}

void handle_lifecycle(addon::Invocation_context& context)
{
    if (context.request().operation != "classify_event") {
        context.reject_unknown_operation();
        return;
    }
    addon::Contract_failure failure;
    if (context.request().capability.capability_id == "attention-kind") {
        const auto request = addon::decode_attention_classification_request(context.request().payload, &failure);
        if (!request) {
            context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
            return;
        }
        context.succeed(addon::encode_attention_classification_result(classify_native_event(request->base)));
        return;
    }
    const auto request = addon::decode_classify_event_request(context.request().payload, &failure);
    if (!request) {
        context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
        return;
    }
    context.succeed(addon::encode_classify_event_result(classify_native_event(*request).base));
}

} // namespace omp
