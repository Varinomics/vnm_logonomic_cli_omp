#include "omp_lifecycle.h"

#include <logonomic_cli_addon/json_utilities.h>

#include <cstdio>
#include <stdexcept>
#include <utility>

namespace {
namespace addon = logonomic::cli_addon;
namespace json = addon::json;
constexpr const char* nonce = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

addon::Classify_event_request catalog_request()
{
    addon::Classify_event_request request;
    request.event.name = "model_catalog";
    request.event.body = json::object({
        {"session_id", addon::Json_value("native-root")}, {"activation_nonce", addon::Json_value(nonce)},
        {"event_id", addon::Json_value("catalog-1")}, {"sequence", addon::Json_value("1")}});
    request.transition_nonce = nonce;
    request.ledger = {1, addon::Ledger_transition{addon::Transition_kind::START, "native-root", nonce}};
    request.model_catalog_version = 1;
    addon::Json_value models = addon::Json_value::array();
    models.push_back(json::object({{"provider", addon::Json_value("native-provider")},
        {"model", addon::Json_value("model")}, {"display_name", addon::Json_value("Native model")},
        {"reasoning", addon::Json_value(false)}}));
    request.event.body.set("catalog", json::object({{"state", addon::Json_value("available")},
        {"models", std::move(models)}, {"total_count", addon::Json_value(200)}, {"truncated", addon::Json_value(true)}}));
    return request;
}

void optional_activation_and_projection()
{
    auto request = catalog_request();
    auto result = omp::classify_native_event(request);
    require(result.base.classification == addon::Classification::IGNORED && result.base.observations.empty() &&
        result.base.catalog_update && !result.base.catalog_failure, "A native registry snapshot must not invent lifecycle facts");
    require(result.base.catalog_update->models.size() == 1 && result.base.catalog_update->total_count == 200 &&
        result.base.catalog_update->truncated && result.base.catalog_update->models.front().reasoning == false &&
        result.base.catalog_update->source == "omp:ctx.models.list", "Catalog projection lost native completeness or reasoning semantics");
    auto decoded = addon::decode_attention_classification_result(addon::encode_attention_classification_result(result), nullptr);
    require(decoded && decoded->base.catalog_update == result.base.catalog_update,
        "The catalog result must pass the actual optional SDK codec");
    request.model_catalog_version = 0;
    result = omp::classify_native_event(request);
    require(!result.base.catalog_update && !result.base.catalog_failure, "An older host must not receive an unsolicited catalog");
    request.model_catalog_version = 1;
    request.event.body.set("activation_nonce", addon::Json_value("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"));
    require(!omp::classify_native_event(request).base.catalog_update, "An expired native activation cannot publish its catalog");
    request = catalog_request();
    request.event.body.set("agent_id", addon::Json_value("child-1"));
    result = omp::classify_native_event(request);
    require(!result.base.catalog_update && result.base.catalog_failure, "A child registry cannot replace the root profile inventory");

    request = catalog_request();
    request.event.name = "session_start";
    request.event.body.erase("catalog");
    request.event.body.set("source", addon::Json_value("load"));
    request.ledger = {};
    request.child_state_version = 1;
    result = omp::classify_native_event(request);
    require(result.base.classification == addon::Classification::TRANSITION && result.base.helper_response &&
        json::member(*result.base.helper_response, "model_catalog_version").as_integer() == 1 &&
        json::member(*result.base.helper_response, "child_state_version").as_integer() == 1,
        "Accepted activation must negotiate catalog without dropping the existing child companion");
}

void malformed_catalog_preserves_lifecycle()
{
    auto request = catalog_request();
    request.event.body.find("catalog")->find("models")->as_array().front().set("api_key", addon::Json_value("forbidden"));
    auto result = omp::classify_native_event(request);
    require(result.base.catalog_failure && !result.base.catalog_update, "Credential fields must not enter catalog state");
    request.event.name = "agent_start";
    request.event.body.set("turn_id", addon::Json_value("native-turn"));
    result = omp::classify_native_event(request);
    require(result.base.catalog_failure && result.base.observations.size() == 1 &&
        result.base.observations.front().phase_transition == addon::Phase_transition::WORKING,
        "Bad optional catalog data cannot discard a valid native start");
    request = catalog_request();
    request.event.body.find("catalog")->set("total_count", addon::Json_value(1));
    result = omp::classify_native_event(request);
    require(result.base.catalog_failure && !result.base.catalog_update, "Contradictory truncation cannot establish a catalog");
}

} // namespace

int main()
{
    try {
        optional_activation_and_projection();
        malformed_catalog_preserves_lifecycle();
        std::puts("Native model catalog classification passed");
        return 0;
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
