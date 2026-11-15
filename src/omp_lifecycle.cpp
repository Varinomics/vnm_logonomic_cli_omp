#include "omp_lifecycle.h"

#include <logonomic_cli_addon/json_utilities.h>

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

} // namespace

addon::Attention_classification_result classify_native_event(const addon::Classify_event_request& request)
{
    addon::Attention_classification_result result;
    const auto& body = request.event.body;
    const auto& name = request.event.name;
    if (!json::only_keys(body, {"session_id", "activation_nonce", "previous_nonce", "event_id", "sequence",
            "turn_id", "source", "session_file", "terminal", "stop_reason", "request_id", "agent_id",
            "agent_label", "status"}) ||
        !identity(body, "session_id") || !identity(body, "event_id") ||
        !identity(body, "turn_id", true) || !identity(body, "request_id", true) ||
        !identity(body, "agent_id", true) ||
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
    }
    else {
        if (!matches_activation(request)) {
            return result;
        }
        if (name == "logonomic_observation_lost") {
            result.base.lifecycle_degradation = addon::Lifecycle_degradation{1, observation.provider_session_id};
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
