#include "omp_model_catalog.h"

#include <logonomic_cli_addon/capability_ids.h>
#include <logonomic_cli_addon/json_utilities.h>

#include <utility>

namespace omp {
namespace addon = logonomic::cli_addon;
namespace json = addon::json;

void append_native_catalog(
    const addon::Classify_event_request& request,
    addon::Classify_event_result& result)
{
    const auto* native = request.event.body.find("catalog");
    if (request.model_catalog_version != addon::k_model_catalog_version || !native) return;
    if (request.event.name != "model_catalog" || !json::text(request.event.body, "agent_id").empty() ||
        !json::only_keys(*native, {"state", "models", "total_count", "truncated"}) ||
        addon::serialize_json(*native).size() > 24u * 1024u)
    {
        result.catalog_failure = addon::Contract_failure{"observation_invalid",
            "Native catalog requires a bounded root registry sample"};
        return;
    }
    auto snapshot = *native;
    snapshot.set("provider_session_id", json::member(request.event.body, "session_id"));
    snapshot.set("transition_nonce", json::member(request.event.body, "activation_nonce"));
    snapshot.set("sequence", json::member(request.event.body, "sequence"));
    snapshot.set("source", addon::Json_value("omp:ctx.models.list"));
    const auto payload = json::object({{"model_catalog", json::object({
        {"version", addon::Json_value(1)}, {"snapshot", std::move(snapshot)}})}});
    addon::decode_model_catalog_update(payload, result.catalog_update, result.catalog_failure);
}

} // namespace omp
