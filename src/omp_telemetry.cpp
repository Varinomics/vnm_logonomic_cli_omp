#include "omp_telemetry.h"

#include <logonomic_cli_addon/json_utilities.h>

#include <string>
#include <utility>

namespace omp {
namespace addon = logonomic::cli_addon;
namespace json = addon::json;
namespace {

void append_context(
    const addon::Classify_event_request& request,
    addon::Classify_event_result& result)
{
    const auto* native = request.event.body.find("context");
    if (request.context_state_version != 2 || !native) {
        return;
    }
    const auto session_id = json::text(request.event.body, "session_id");
    if (!json::only_keys(*native, {"used_tokens", "context_window", "usage_unavailable", "compaction_id"})) {
        result.context_failure = addon::Context_failure{session_id,
            {"observation_invalid", "Native context observation contains unsupported facts"}};
        return;
    }
    addon::Json_value update = json::object({
        {"provider_session_id", addon::Json_value(session_id)},
        {"event_id", json::member(request.event.body, "event_id")}});
    for (const auto* key : {"used_tokens", "context_window", "usage_unavailable"}) {
        if (const auto* value = native->find(key)) {
            update.set(key, *value);
        }
    }
    if (const auto* compaction_id = native->find("compaction_id")) {
        if (request.event.name != "session_compact" || !json::bounded_string(*compaction_id, 240)) {
            result.context_failure = addon::Context_failure{session_id,
                {"observation_invalid", "A completed native compaction requires its stable entry identity"}};
            return;
        }
        update.set("event_id", addon::Json_value("compaction:" + compaction_id->as_string()));
        update.set("compacted", addon::Json_value(true));
    }
    addon::Json_value updates = addon::Json_value::array();
    updates.push_back(std::move(update));
    const auto payload = json::object({{"context_updates", std::move(updates)}});
    addon::decode_context_updates(payload, result.context_updates, result.context_failure);
}

void append_execution(
    const addon::Classify_event_request& request,
    addon::Classify_event_result& result)
{
    const auto* native = request.event.body.find("execution");
    if (request.execution_context_version != 1 || !native) {
        return;
    }
    if (!json::exact_keys(*native, {"state", "cwd"})) {
        result.execution_failure = addon::Contract_failure{"observation_invalid",
            "Native execution context requires exactly its state and directory"};
        return;
    }
    const auto state = json::text(*native, "state");
    if (state != "confirmed" && state != "unknown") {
        result.execution_failure = addon::Contract_failure{"observation_invalid",
            "Native local execution context state is unsupported"};
        return;
    }
    addon::Json_value update = json::object({
        {"provider_session_id", json::member(request.event.body, "session_id")},
        {"transition_nonce", json::member(request.event.body, "activation_nonce")},
        {"sequence", json::member(request.event.body, "sequence")},
        {"state", addon::Json_value(state)},
        {"environment", addon::Json_value(state == "confirmed" ? "local" : "")},
        {"cwd", json::member(*native, "cwd")},
        {"source", addon::Json_value("omp:" + request.event.name + ":ctx.cwd")}});
    addon::Json_value updates = addon::Json_value::array();
    updates.push_back(std::move(update));
    const auto payload = json::object({{"execution_context", json::object({
        {"version", addon::Json_value(1)}, {"updates", std::move(updates)}})}});
    addon::decode_execution_updates(payload, result.execution_updates, result.execution_failure);
}

} // namespace

void append_native_telemetry(
    const addon::Classify_event_request& request,
    addon::Classify_event_result& result)
{
    append_context(request, result);
    append_execution(request, result);
}

} // namespace omp
