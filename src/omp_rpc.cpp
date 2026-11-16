#include "omp_rpc.h"

#include "omp_provider.h"
#include "omp_profiles.h"

#include <logonomic_cli_addon/json_utilities.h>
#include <logonomic_cli_addon/launcher_utilities.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/sha256.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace omp {
namespace addon = logonomic::cli_addon;
namespace json = addon::json;
using Json = addon::Json_value;

namespace {

// OMP's physical JSONL frame includes its newline. Logical frames must also
// fit the host's continuation budget; exceeding it is an explicit failure.
constexpr std::size_t k_frame_bytes   = 1024 * 1024;
constexpr std::size_t k_logical_bytes = 1280 * 1024;
constexpr std::size_t k_chunk_bytes   = 256 * 1024;
constexpr std::size_t k_result_bytes  = addon::limits::k_max_structured_stream_bytes;
constexpr std::size_t k_error_bytes   = addon::limits::k_max_structured_failure_message_bytes;

enum class Phase
{
    READY,
    NEGOTIATING,
    FILTERING,
    CHECKING,
    CATALOG,
    RUNNING,
    DRAINING,
};

struct Rpc_state
{
    Phase                    phase = Phase::READY;
    std::string              prompt;
    Json                     expected_tools = Json::array();
    std::string              tool_scope;
    std::string              review_scope;
    std::string              native_session;
    std::string              selected_model;
    std::string              selected_effort;
    std::string              pending;
    std::string              output;
    std::string              stderr_tail;
    std::string              chunk_id;
    std::string              chunk;
    std::size_t              chunk_count = 0;
    std::size_t              chunk_next = 0;
    std::size_t              chunk_length = 0;
    std::size_t              frame_limit = k_frame_bytes;
    bool                     acknowledged = false;
    bool                     completed = false;
    bool                     settled = false;
};

void reject(addon::Contract_failure* failure, std::string reason, std::string detail)
{
    if (failure) {
        *failure = {std::move(reason), std::move(detail)};
    }
}

std::string diagnostic_text(std::string text)
{
    text.erase(std::remove(text.begin(), text.end(), '\0'), text.end());
    if (!addon::is_valid_utf8(text)) {
        return {};
    }
    if (text.size() > k_error_bytes) {
        text.resize(k_error_bytes);
        while (!text.empty() && !addon::is_valid_utf8(text)) {
            text.pop_back();
        }
    }
    return text;
}

addon::Structured_inspect_result failed(
    const char* reason, std::string message, const char* source = "protocol_error")
{
    addon::Structured_inspect_result result;
    result.complete = true;
    result.failure_reason = reason;
    const bool truncated = message.size() > k_error_bytes;
    message = diagnostic_text(std::move(message));
    if (!message.empty()) {
        result.failure_diagnostics = addon::Structured_failure_diagnostics{source, std::move(message), truncated};
    }
    return result;
}

Json strings(const std::vector<std::string>& values)
{
    auto array = Json::array();
    for (const auto& value : values) {
        array.push_back(Json(value));
    }
    return array;
}

std::string save_state(const Rpc_state& state)
{
    return addon::serialize_json(json::object({
        {"version", Json(3)}, {"phase", Json(static_cast<int>(state.phase))},
        {"prompt", Json(state.prompt)}, {"expected_tools", state.expected_tools}, {"tool_scope", Json(state.tool_scope)},
        {"review_scope", Json(state.review_scope)}, {"native_session", Json(state.native_session)},
        {"selected_model", Json(state.selected_model)}, {"selected_effort", Json(state.selected_effort)},
        {"pending", Json(addon::encode_base64(state.pending))}, {"output", Json(state.output)},
        {"stderr", Json(addon::encode_base64(state.stderr_tail))},
        {"chunk_id", Json(state.chunk_id)}, {"chunk", Json(addon::encode_base64(state.chunk))},
        {"chunk_count", Json(static_cast<std::uint64_t>(state.chunk_count))},
        {"chunk_next", Json(static_cast<std::uint64_t>(state.chunk_next))},
        {"chunk_length", Json(static_cast<std::uint64_t>(state.chunk_length))},
        {"frame_limit", Json(static_cast<std::uint64_t>(state.frame_limit))},
        {"acknowledged", Json(state.acknowledged)}, {"completed", Json(state.completed)},
        {"settled", Json(state.settled)},
    }));
}

bool integer_in(const Json& value, std::int64_t minimum, std::int64_t maximum)
{
    return value.is_integer() && value.as_integer() >= minimum && value.as_integer() <= maximum;
}

std::optional<Rpc_state> load_state(std::string_view bytes)
{
    const auto value = addon::parse_exchange_protocol_state(bytes);
    if (!value || !json::exact_keys(*value, {"version", "phase", "prompt", "expected_tools", "tool_scope", "review_scope", "native_session",
            "selected_model", "selected_effort", "pending", "output", "stderr", "chunk_id", "chunk",
            "chunk_count", "chunk_next", "chunk_length", "frame_limit", "acknowledged", "completed", "settled"}) ||
        json::member(*value, "version") != Json(3) ||
        !integer_in(json::member(*value, "phase"), 0, static_cast<int>(Phase::DRAINING)))
    {
        return std::nullopt;
    }
    for (const auto key : {"prompt", "tool_scope", "review_scope", "native_session", "selected_model", "selected_effort", "pending", "output", "stderr", "chunk_id", "chunk"}) {
        if (!json::member(*value, key).is_string()) {
            return std::nullopt;
        }
    }
    for (const auto key : {"acknowledged", "completed", "settled"}) {
        if (!json::member(*value, key).is_boolean()) {
            return std::nullopt;
        }
    }
    for (const auto key : {"chunk_count", "chunk_next"}) {
        if (!integer_in(json::member(*value, key), 0, 256)) {
            return std::nullopt;
        }
    }
    if (!integer_in(json::member(*value, "chunk_length"), 0, k_logical_bytes) ||
        !integer_in(json::member(*value, "frame_limit"), 1, k_frame_bytes))
    {
        return std::nullopt;
    }
    const auto& tools = json::member(*value, "expected_tools");
    if (!tools.is_array() || tools.as_array().size() > 512) {
        return std::nullopt;
    }
    Rpc_state state;
    for (const auto& tool : tools.as_array()) {
        if (!json::exact_keys(tool, {"server", "action"}) ||
            !json::bounded_string(json::member(tool, "server"), 128) ||
            !json::bounded_string(json::member(tool, "action"), 64))
        {
            return std::nullopt;
        }
    }
    state.expected_tools = tools;
    state.tool_scope = json::text(*value, "tool_scope");
    state.review_scope = json::text(*value, "review_scope");
    state.native_session = json::text(*value, "native_session");
    auto pending = addon::decode_base64(json::text(*value, "pending"));
    auto chunk = addon::decode_base64(json::text(*value, "chunk"));
    auto errors = addon::decode_base64(json::text(*value, "stderr"));
    if (!pending || !chunk || !errors || pending->size() >= k_frame_bytes ||
        chunk->size() > k_logical_bytes || errors->size() > k_error_bytes)
    {
        return std::nullopt;
    }
    state.phase           = static_cast<Phase>(json::member(*value, "phase").as_integer());
    state.prompt          = json::text(*value, "prompt");
    state.selected_model  = json::text(*value, "selected_model");
    state.selected_effort = json::text(*value, "selected_effort");
    state.pending         = std::move(*pending);
    state.output          = json::text(*value, "output");
    state.stderr_tail     = std::move(*errors);
    state.chunk_id        = json::text(*value, "chunk_id");
    state.chunk           = std::move(*chunk);
    state.chunk_count     = static_cast<std::size_t>(json::member(*value, "chunk_count").as_integer());
    state.chunk_next      = static_cast<std::size_t>(json::member(*value, "chunk_next").as_integer());
    state.chunk_length    = static_cast<std::size_t>(json::member(*value, "chunk_length").as_integer());
    state.frame_limit     = static_cast<std::size_t>(json::member(*value, "frame_limit").as_integer());
    state.acknowledged    = json::member(*value, "acknowledged").as_boolean();
    state.completed       = json::member(*value, "completed").as_boolean();
    state.settled         = json::member(*value, "settled").as_boolean();
    if (state.prompt.size() > addon::limits::k_max_structured_prompt_bytes || state.output.size() > k_result_bytes ||
        state.selected_model.size() > addon::limits::k_max_delegated_model_bytes ||
        state.selected_effort.size() > addon::limits::k_max_delegated_reasoning_effort_bytes ||
        state.chunk_id.size() > 128 || state.tool_scope.size() > 64 || state.pending.size() >= state.frame_limit ||
        (!state.review_scope.empty() && !addon::is_valid_generation_nonce_hex(state.review_scope)) ||
        state.native_session.size() > 256 || (state.review_scope.empty() && !state.native_session.empty()) ||
        (state.expected_tools.as_array().empty() != state.tool_scope.empty()) ||
        (state.chunk_id.empty() && (state.chunk_count || state.chunk_next || state.chunk_length || !state.chunk.empty())) ||
        (!state.chunk_id.empty() && (state.chunk_count < 2 || state.chunk_next == 0 ||
            state.chunk_next >= state.chunk_count || state.chunk_length < k_frame_bytes ||
            state.chunk.size() > state.chunk_length)) ||
        (state.phase == Phase::DRAINING && (!state.acknowledged || !state.completed || !state.settled)))
    {
        return std::nullopt;
    }
    return state;
}

std::string command(const char* id, const char* type, Json fields = Json::object())
{
    fields.set("id", Json(id));
    fields.set("type", Json(type));
    return addon::serialize_json(fields) + '\n';
}

void presented(addon::Structured_inspect_result& result, addon::Presentation_event_kind kind)
{
    result.presentation_state = addon::Presentation_state::AVAILABLE;
    const auto previous = std::find_if(result.presentation.begin(), result.presentation.end(),
        [&](const auto& event) { return event.kind == kind; });
    if (previous != result.presentation.end()) {
        ++previous->count;
    }
    else {
        result.presentation.push_back({kind, 1});
    }
}

bool exact_selection(const Rpc_state& state, const Json& native)
{
    if (!state.selected_model.empty()) {
        const auto& model = json::member(native, "model");
        const auto id = json::text(model, "id");
        const auto qualified = json::text(model, "provider") + "/" + id;
        if (state.selected_model != id && state.selected_model != qualified) {
            return false;
        }
    }
    return state.selected_effort.empty() || json::text(native, "thinkingLevel") == state.selected_effort;
}

bool tools_available(const Rpc_state& state, const Json& native)
{
    if (state.expected_tools.as_array().empty()) {
        return true;
    }
    const auto& entries = json::member(native, "entries");
    if (!entries.is_array()) {
        return false;
    }
    const Json* inventory = nullptr;
    for (const auto& entry : entries.as_array()) {
        if (json::text(entry, "type") != "custom" || json::text(entry, "customType") != "logonomic-task-control") {
            continue;
        }
        const auto& data = json::member(entry, "data");
        if (json::text(data, "scope") != state.tool_scope) {
            continue;
        }
        if (inventory || json::member(data, "version") != Json(1) || !json::member(data, "tools").is_array()) {
            return false;
        }
        inventory = &json::member(data, "tools");
    }
    if (!inventory) {
        return false;
    }
    std::vector<std::string> names;
    for (const auto& expected : state.expected_tools.as_array()) {
        const auto& tools = inventory->as_array();
        if (std::count_if(tools.begin(), tools.end(), [&](const auto& tool) {
                return json::member(tool, "server") == json::member(expected, "server") &&
                    json::member(tool, "action") == json::member(expected, "action");
            }) != 1)
        {
            return false;
        }
        const auto found = std::find_if(tools.begin(), tools.end(), [&](const auto& tool) {
            return json::member(tool, "server") == json::member(expected, "server") &&
                json::member(tool, "action") == json::member(expected, "action");
        });
        const auto name = json::text(*found, "name");
        if (!json::bounded_string(json::member(*found, "name"), 64) ||
            std::find(names.begin(), names.end(), name) != names.end())
        {
            return false;
        }
        names.push_back(name);
    }
    return true;
}

bool review_guard_available(const Rpc_state& state, const Json& data)
{
    if (state.review_scope.empty()) {
        return true;
    }
    const auto& entries = json::member(data, "entries");
    if (!entries.is_array()) {
        return false;
    }
    const Json* acknowledgment = nullptr;
    for (const auto& entry : entries.as_array()) {
        if (json::text(entry, "type") != "custom" || json::text(entry, "customType") != "logonomic-read-only") {
            continue;
        }
        const auto& value = json::member(entry, "data");
        if (json::text(value, "scope") != state.review_scope) {
            continue;
        }
        if (acknowledgment) {
            return false;
        }
        acknowledgment = &value;
    }
    return acknowledgment && json::exact_keys(*acknowledgment, {"version", "scope", "session_id"}) &&
        json::member(*acknowledgment, "version") == Json(1) &&
        json::text(*acknowledgment, "session_id") == state.native_session;
}

void submit_prompt(Rpc_state& state, std::string& input, addon::Structured_inspect_result& result)
{
    input += command("lg-prompt", "prompt", json::object({{"message", Json(state.prompt)}}));
    state.prompt.clear();
    state.phase = Phase::RUNNING;
    presented(result, addon::Presentation_event_kind::REVIEW_INITIALIZED);
}

std::optional<addon::Structured_inspect_result> consume_frame(
    Rpc_state& state, const Json& frame, std::string& input, addon::Structured_inspect_result& result)
{
    const auto type = json::text(frame, "type");
    if (type.empty() || type == "rpc_chunk") {
        return failed("omp_protocol_invalid", "Expected one OMP RPC object with a nonempty type.");
    }
    if (type == "rpc_frame_error") {
        return failed("omp_output_too_large", "OMP could not deliver a lossless RPC frame.");
    }
    if (type == "notice" && json::text(frame, "level") == "error") {
        return failed("omp_native_error", json::text(frame, "message"), "native_error");
    }
    if (type == "extension_ui_request") {
        const auto method = json::text(frame, "method");
        if (method == "select" || method == "confirm" || method == "input" || method == "editor" || method == "ask") {
            return failed("hook_approval_required", "OMP requested a user decision during a session without an interactive answer channel.", "native_error");
        }
        return std::nullopt;
    }
    if (type == "ready") {
        const auto& versions = json::member(frame, "supportedProtocolVersions");
        if (state.phase != Phase::READY || !versions.is_array() ||
            std::find(versions.as_array().begin(), versions.as_array().end(), Json(2)) == versions.as_array().end() ||
            !integer_in(json::member(frame, "maxFrameBytes"), 1, 64 * 1024 * 1024) ||
            !integer_in(json::member(frame, "maxReassembledFrameBytes"), k_frame_bytes, 64 * 1024 * 1024))
        {
            return failed("update_required", "OMP must support lossless RPC protocol 2 and settlement-aware prompt results.");
        }
        state.frame_limit = std::min(k_frame_bytes,
            static_cast<std::size_t>(json::member(frame, "maxFrameBytes").as_integer()));
        input += command("lg-protocol", "negotiate_protocol", json::object({{"protocolVersion", Json(2)}}));
        state.phase = Phase::NEGOTIATING;
        return std::nullopt;
    }
    if (type == "response") {
        const auto id = json::text(frame, "id");
        const auto native_command = json::text(frame, "command");
        const char* expected_id = nullptr;
        const char* expected_command = nullptr;
        switch (state.phase) {
            case Phase::NEGOTIATING: expected_id = "lg-protocol"; expected_command = "negotiate_protocol"; break;
            case Phase::FILTERING: expected_id = "lg-filter"; expected_command = "set_event_filter"; break;
            case Phase::CHECKING: expected_id = "lg-state"; expected_command = "get_state"; break;
            case Phase::CATALOG: expected_id = "lg-tools"; expected_command = "get_entries"; break;
            case Phase::RUNNING:
            case Phase::DRAINING: expected_id = "lg-prompt"; expected_command = "prompt"; break;
            case Phase::READY: break;
        }
        if (!expected_id || id != expected_id || native_command != expected_command ||
            !json::member(frame, "success").is_boolean())
        {
            return failed("omp_protocol_invalid", "An OMP command response did not match the outstanding request.");
        }
        if (!json::member(frame, "success").as_boolean()) {
            return failed("omp_native_error", json::text(frame, "error"), "native_error");
        }
        const auto& data = json::member(frame, "data");
        if (state.phase == Phase::NEGOTIATING) {
            if (json::member(data, "protocolVersion") != Json(2)) {
                return failed("update_required", "OMP did not acknowledge RPC protocol 2.");
            }
            input += command("lg-filter", "set_event_filter", json::object({
                {"events", strings({"message_update", "message_end", "tool_execution_end", "agent_end",
                    "auto_retry_start", "auto_retry_end", "retry_fallback_applied", "thinking_level_changed"})},
                {"messageUpdates", Json("delta")},
            }));
            state.phase = Phase::FILTERING;
        }
        else
        if (state.phase == Phase::FILTERING) {
            input += command("lg-state", "get_state");
            state.phase = Phase::CHECKING;
        }
        else
        if (state.phase == Phase::CHECKING) {
            if (json::member(data, "isSettled") != Json(true) ||
                json::member(data, "isStreaming") != Json(false) ||
                json::member(data, "hasPendingAsyncWork") != Json(false) ||
                json::member(data, "queuedMessageCount") != Json(0))
            {
                return failed("omp_session_not_idle", "OMP did not confirm a settled session before accepting the task.");
            }
            if (!exact_selection(state, data)) {
                return failed("model_selection_unavailable", "OMP did not retain the exact requested model and thinking level.", "native_error");
            }
            if (!state.selected_model.empty()) {
                const auto& model = json::member(data, "model");
                state.selected_model = json::text(model, "provider") + "/" + json::text(model, "id");
            }
            if (!state.review_scope.empty()) {
                if (!json::bounded_string(json::member(data, "sessionId"), 256)) {
                    return failed("read_only_unavailable", "OMP did not identify the guarded native review session.", "native_error");
                }
                state.native_session = json::text(data, "sessionId");
            }
            if (state.expected_tools.as_array().empty() && state.review_scope.empty()) {
                submit_prompt(state, input, result);
            }
            else {
                input += command("lg-tools", "get_entries");
                state.phase = Phase::CATALOG;
            }
        }
        else
        if (state.phase == Phase::CATALOG) {
            if (!review_guard_available(state, data)) {
                return failed("read_only_unavailable", "OMP did not acknowledge this task's active native read-only guard.", "native_error");
            }
            if (!tools_available(state, data)) {
                return failed("task_control_unavailable", "OMP did not confirm every requested task-server action in this task's enabled tool inventory.", "native_error");
            }
            submit_prompt(state, input, result);
        }
        else {
            if (state.acknowledged) {
                return failed("omp_protocol_invalid", "OMP acknowledged the same task more than once.");
            }
            state.acknowledged = true;
            if (json::member(data, "agentInvoked") == Json(false)) {
                state.completed = true;
                state.settled = true;
            }
        }
        return std::nullopt;
    }
    if (type == "prompt_result") {
        if (state.phase != Phase::RUNNING || state.completed || json::text(frame, "id") != "lg-prompt" ||
            !json::member(frame, "agentInvoked").is_boolean() ||
            !json::member(frame, "sessionSettled").is_boolean())
        {
            return failed("omp_protocol_invalid", "OMP's prompt result did not identify the active task exactly once.");
        }
        const auto status = json::text(frame, "status");
        if (status == "aborted") {
            return failed("omp_cancelled", "OMP aborted the task.", "native_error");
        }
        if (status == "error") {
            return failed("omp_native_error", json::text(json::member(frame, "error"), "message"), "native_error");
        }
        if (status != "completed") {
            return failed("omp_protocol_invalid", "OMP returned an unknown task completion status.");
        }
        state.completed = true;
        state.settled = json::member(frame, "sessionSettled").as_boolean();
        return std::nullopt;
    }
    if (type == "session_settled") {
        if (state.phase == Phase::RUNNING && state.completed) {
            state.settled = true;
        }
        return std::nullopt;
    }
    if (state.phase != Phase::RUNNING && state.phase != Phase::DRAINING) {
        return std::nullopt;
    }
    if (type == "retry_fallback_applied" && (!state.selected_model.empty() || !state.selected_effort.empty())) {
        return failed("model_selection_unavailable", "OMP attempted model fallback despite the task's explicit selection.", "native_error");
    }
    if (type == "thinking_level_changed" && !state.selected_effort.empty() &&
        json::text(frame, "thinkingLevel") != state.selected_effort)
    {
        return failed("model_selection_unavailable", "OMP changed the task's explicitly selected thinking level.", "native_error");
    }
    if (type == "message_end") {
        const auto& message = json::member(frame, "message");
        if (json::text(message, "role") != "assistant") {
            return std::nullopt;
        }
        if (!state.selected_model.empty() &&
            json::text(message, "provider") + "/" + json::text(message, "model") != state.selected_model)
        {
            return failed("model_selection_unavailable", "OMP produced a task message using a different model.", "native_error");
        }
        const auto stop = json::text(message, "stopReason");
        if (stop == "error" || stop == "aborted") {
            // The correlated prompt_result owns the terminal outcome, including retries.
            return std::nullopt;
        }
        const auto& content = json::member(message, "content");
        if (!content.is_array() || !json::bounded_string(json::member(frame, "messageId"), 256)) {
            return failed("omp_protocol_invalid", "OMP's assistant message has no stable identity or content array.");
        }
        std::string text;
        for (const auto& block : content.as_array()) {
            if (json::text(block, "type") == "text") {
                if (!json::member(block, "text").is_string()) {
                    return failed("omp_protocol_invalid", "OMP's assistant text block is malformed.");
                }
                text += json::text(block, "text");
                if (text.size() > k_result_bytes) {
                    return failed("omp_output_too_large", "OMP's answer exceeds the structured result bound.");
                }
            }
        }
        // Each message_end is authoritative. Streaming deltas, tool arguments,
        // reasoning blocks and agent_end's compacted history are never answers.
        state.output = std::move(text);
    }
    else
    if (type == "command_output") {
        if (!json::member(frame, "text").is_string()) {
            return failed("omp_protocol_invalid", "OMP's local command output is malformed.");
        }
        state.output += json::text(frame, "text");
        if (state.output.size() > k_result_bytes) {
            return failed("omp_output_too_large", "OMP's local command output exceeds the structured result bound.");
        }
    }
    else
    if (type == "message_update") {
        const auto event = json::text(json::member(frame, "assistantMessageEvent"), "type");
        if (event == "thinking_delta") {
            presented(result, addon::Presentation_event_kind::REASONING);
        }
        else
        if (event == "text_delta") {
            presented(result, addon::Presentation_event_kind::ANALYSIS_UPDATE);
        }
    }
    else
    if (type == "tool_execution_end") {
        presented(result, addon::Presentation_event_kind::TOOL_RESPONSE);
    }
    else
    if (type == "auto_retry_start") {
        presented(result, addon::Presentation_event_kind::RATE_LIMIT_STATUS);
    }
    return std::nullopt;
}

// Only the native options whose token arity is known can be preserved without
// accidentally turning a launcher message or command into an extra task.
bool session_arguments(const std::vector<std::string>& arguments, bool* trusted_extensions = nullptr, bool read_only = false)
{
    constexpr std::string_view valued[] = {
        "--profile", "--provider", "--model", "--config", "--add-dir", "--smol", "--slow", "--plan",
        "--prewalk-into", "--plan-yolo-into", "--max-time", "--warn-cost", "--max-cost", "--service-tier",
        "--spinner-interval", "--max-fps", "--motion", "--api-key", "--system-prompt", "--system-prompt-template",
        "--append-system-prompt", "--prompt-cache-key", "--session-dir", "--models", "--tools", "--thinking",
        "--hook", "--extension", "-e", "--trusted-extension", "--plugin-dir", "--skills", "--approval-mode",
    };
    constexpr std::string_view flags[] = {
        "--allow-home", "--no-session", "--no-tools", "--no-lsp", "--no-pty", "--no-judge", "--no-spotlight",
        "--offline", "--hide-thinking", "--advisor", "--wait-async", "--external-thinking", "--prewalk",
        "--no-prewalk", "--plan-yolo", "--no-extensions", "--no-skills", "--no-rules", "--no-context-files",
        "--no-title", "--no-ui", "--auto-approve", "--yolo",
    };
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto& argument = arguments[index];
        const auto equals = argument.starts_with("--") ? argument.find('=') : std::string::npos;
        const std::string_view name(argument.data(), equals == std::string::npos ? argument.size() : equals);
        if (read_only) {
            constexpr std::string_view conflicting[] = {
                "--hook", "--extension", "-e", "--trusted-extension", "--tools", "--no-tools", "--session-dir",
                "--plan", "--plan-yolo", "--plan-yolo-into", "--prewalk", "--prewalk-into", "--advisor",
            };
            if (std::find(std::begin(conflicting), std::end(conflicting), name) != std::end(conflicting)) {
                return false;
            }
        }
        if (std::find(std::begin(valued), std::end(valued), name) != std::end(valued)) {
            if (trusted_extensions && name == "--trusted-extension") {
                *trusted_extensions = true;
            }
            if (equals == std::string::npos && ++index == arguments.size()) {
                return false;
            }
        }
        else
        if (std::find(std::begin(flags), std::end(flags), name) == std::end(flags)) {
            return false;
        }
    }
    return true;
}

std::optional<addon::Process_proposal> prepare_process(
    addon::Structured_purpose purpose, const addon::Session_launch_context& context,
    std::string_view prompt, addon::Contract_failure* failure)
{
    if (prompt.empty() || !addon::is_valid_utf8(prompt) || prompt.find('\0') != std::string_view::npos ||
        prompt.size() > addon::limits::k_max_structured_prompt_bytes || !context.launch_environment)
    {
        reject(failure, "frame_violation", "A UTF-8 task and frozen launch environment are required.");
        return std::nullopt;
    }
    const auto& environment = *context.launch_environment;
    const bool read_only = purpose == addon::Structured_purpose::READ_ONLY_REVIEW;
    const auto first = prompt.find_first_not_of(" \t\r\n");
    if (read_only && first != std::string_view::npos && prompt[first] == '/') {
        reject(failure, "purpose_unavailable", "A read-only task must be model input, not a native slash command.");
        return std::nullopt;
    }
    if (!validate_native_account(context, {}, failure)) {
        return std::nullopt;
    }
    const auto launcher = recognize_launcher(context.launcher, environment.platform_id, context.launcher_argv);
    if (!launcher || !session_arguments(launcher->arguments, nullptr, read_only)) {
        reject(failure, "launcher_unavailable", "The saved launcher must be a literal OMP invocation with supported options and no startup task, resume, alias, mode or directory override.");
        return std::nullopt;
    }
    const auto frozen = addon::environment_pairs(environment.effective_environment);
    const auto program = addon::resolve_program(launcher->program, frozen, context.working_directory);
    if (!program) {
        reject(failure, "launcher_unavailable", "OMP could not be resolved in the frozen launch environment.");
        return std::nullopt;
    }
    const bool windows = environment.platform_id.starts_with("windows-");
    if (windows && !addon::native_image(*program)) {
        reject(failure, "launcher_unavailable", "Structured sessions require the native OMP executable on Windows.");
        return std::nullopt;
    }
    addon::Process_proposal process;
    process.executable = windows ? "helpers/omp_launch_bridge.exe" : "helpers/omp_launch_bridge";
    process.argv.push_back(addon::path_to_utf8(*program));
    process.argv.insert(process.argv.end(), launcher->arguments.begin(), launcher->arguments.end());
    process.argv.insert(process.argv.end(), {"--mode", "rpc", "--no-ui"});
    process.stdin_behavior = addon::Stdin_behavior::PAYLOAD;
    for (const auto& entry : frozen) {
        if (!addon::is_reserved_environment_name(entry.first)) {
            process.environment_names.push_back(entry.first);
        }
    }
    if (!addon::validate_structured_process_proposal(process, failure)) {
        return std::nullopt;
    }
    return process;
}

bool configure_review(
    const addon::Session_launch_context& context, addon::Process_proposal& process,
    Rpc_state& state, addon::Contract_failure* failure)
{
    constexpr const char* asset = "assets/native_review.ts";
    if (std::find(context.declared_assets.begin(), context.declared_assets.end(), asset) == context.declared_assets.end()) {
        reject(failure, "read_only_unavailable", "The host did not adopt the managed native review guard.");
        return false;
    }
    state.review_scope = addon::random_nonce_hex();
    const auto policy = addon::serialize_json(json::object({
        {"version", Json(1)}, {"scope", Json(state.review_scope)}, {"tools", state.expected_tools},
    }));
    if (policy.size() > addon::limits::k_max_environment_value_bytes) {
        reject(failure, "payload_too_large", "The exact review task-control policy exceeds the native launch bound.");
        return false;
    }
    process.executable = context.launch_environment->platform_id.starts_with("windows-")
        ? "helpers/omp_review_bridge.exe" : "helpers/omp_review_bridge";
    std::erase_if(process.environment_names, [](const auto& name) {
        const auto lowered = addon::ascii_lowercase(name);
        return lowered == "omp_logonomic_review_asset" || lowered == "omp_logonomic_review_policy";
    });
    process.environment.emplace_back("OMP_LOGONOMIC_REVIEW_ASSET", "{{asset:assets/native_review.ts}}");
    process.environment.emplace_back("OMP_LOGONOMIC_REVIEW_POLICY", policy);
    process.assets.push_back(asset);
    return addon::validate_structured_process_proposal(process, failure);
}

std::string inventory_extension(const Rpc_state& state)
{
    // Native extension getActiveTools() is the enabled union, including xd://
    // devices (modes/runtime-init.ts). MCP provenance is supplied by
    // getAllTools(); names are never inferred from prompt prose or a hash.
    return "const expected = " + addon::serialize_json(state.expected_tools) + ";\n" +
        "const scope = " + addon::serialize_json(Json(state.tool_scope)) + ";\n" + R"(export default function (pi) {
    pi.on("session_start", (_event, ctx) => {
        if (ctx.agent.kind !== "main") return;
        const enabled = new Set(pi.getActiveTools());
        const tools = pi.getAllTools().filter(tool => enabled.has(tool.name) &&
            expected.some(wanted => wanted.server === tool.mcpServerName && wanted.action === tool.mcpToolName))
            .map(tool => ({name: tool.name, server: tool.mcpServerName, action: tool.mcpToolName}));
        pi.appendEntry("logonomic-task-control", {version: 1, scope, tools});
    });
}
)";
}

} // namespace

std::optional<addon::Structured_prepare_result> prepare_structured_session(
    const addon::Structured_prepare_request& request, addon::Contract_failure* failure)
{
    auto process = prepare_process(request.purpose, request.context, request.prompt, failure);
    if (!process) {
        return std::nullopt;
    }
    Rpc_state state;
    state.prompt = request.prompt;
    if (request.purpose == addon::Structured_purpose::READ_ONLY_REVIEW &&
        !configure_review(request.context, *process, state, failure))
    {
        return std::nullopt;
    }
    addon::Structured_prepare_result result;
    result.process         = std::move(*process);
    result.inspection      = {true, save_state(state), true};
    result.profile_storage = request.context.profile_storage;
    result.account_context = request.context.account_context;
    return result;
}

std::optional<addon::Delegated_prepare_result> prepare_delegated_session(
    const addon::Delegated_prepare_request& request, addon::Contract_failure* failure)
{
    auto process = prepare_process(request.purpose, request.context, request.prompt, failure);
    if (!process) {
        return std::nullopt;
    }
    if (!request.control.connection) {
        reject(failure, "task_control_unavailable", "Delegation requires the host's task-control relay connection.");
        return std::nullopt;
    }
    const auto& connection = *request.control.connection;
    for (const auto& path : {connection.relay_program, connection.descriptor_path}) {
        if (path.find("${CLAUDE_PLUGIN_ROOT}") != std::string::npos || path.find("${OMP_PLUGIN_ROOT}") != std::string::npos) {
            reject(failure, "task_control_unavailable", "The relay path contains an OMP plugin substitution token.");
            return std::nullopt;
        }
    }
    Rpc_state state;
    state.prompt = request.prompt;
    if (request.model_selection) {
        if (request.model_selection->model) {
            state.selected_model = *request.model_selection->model;
            process->argv.insert(process->argv.end(), {"--model", state.selected_model});
        }
        if (request.model_selection->reasoning_effort) {
            state.selected_effort = *request.model_selection->reasoning_effort;
            constexpr std::string_view efforts[] = {"off", "minimal", "low", "medium", "high", "xhigh", "max"};
            if (std::find(std::begin(efforts), std::end(efforts), state.selected_effort) == std::end(efforts)) {
                reject(failure, "model_selection_unavailable", "A concrete native OMP thinking level is required.");
                return std::nullopt;
            }
            process->argv.insert(process->argv.end(), {"--thinking", state.selected_effort});
        }
    }
    const auto plugin_name = "lgo-" + addon::to_hex(addon::Sha256::digest(request.control.task_id)).substr(0, 12);
    state.tool_scope = plugin_name;
    auto servers = Json::object();
    addon::Delegated_prepare_result result;
    for (std::size_t index = 0; index < request.control.servers.size(); ++index) {
        const auto& server = request.control.servers[index];
        const auto alias = "s" + std::to_string(index);
        for (const auto& action : server.actions) {
            state.expected_tools.push_back(json::object({{"server", Json(plugin_name + ":" + alias)}, {"action", Json(action)}}));
        }
        servers.set(alias, json::object({
            {"type", Json("stdio")}, {"command", Json(connection.relay_program)},
            {"args", strings({"--descriptor", connection.descriptor_path, "--server", server.server_id})},
        }));
        result.acknowledgment.server_ids.push_back(server.server_id);
    }
    // OMP's documented Claude-plugin layout accepts an absolute executable.
    // Portable Agent Plugins deliberately restrict commands to bare names or
    // contained relative files and cannot represent this host-owned relay.
    const auto manifest = json::object({{"name", Json(plugin_name)}});
    const auto mcp = json::object({{"mcpServers", std::move(servers)}});
    result.private_artifacts = {
        {".claude-plugin/plugin.json", addon::serialize_json(manifest)}, {".mcp.json", addon::serialize_json(mcp)},
        {"task-control.ts", inventory_extension(state)},
    };
    process->argv.insert(process->argv.end(), {"--plugin-dir", request.control.private_artifact_root});
    const auto root = addon::path_from_utf8(request.control.private_artifact_root);
    const auto launcher = recognize_launcher(request.context.launcher, request.context.launch_environment->platform_id,
        request.context.launcher_argv);
    bool trusted_extensions = false;
    session_arguments(launcher->arguments, &trusted_extensions);
    process->argv.insert(process->argv.end(), {trusted_extensions || request.purpose == addon::Structured_purpose::READ_ONLY_REVIEW
        ? "--trusted-extension" : "--extension",
        addon::path_to_utf8(root / "task-control.ts")});
    if (request.model_selection) {
        result.private_artifacts.push_back({"selection.yml", addon::serialize_json(json::object({
            {"retry", json::object({{"modelFallback", Json(false)}})},
            {"contextPromotion", json::object({{"enabled", Json(false)}})},
        }))});
        process->argv.insert(process->argv.end(), {"--config", addon::path_to_utf8(root / "selection.yml")});
    }
    if (request.purpose == addon::Structured_purpose::READ_ONLY_REVIEW &&
        !configure_review(request.context, *process, state, failure))
    {
        return std::nullopt;
    }
    if (!addon::validate_structured_process_proposal(*process, failure)) {
        return std::nullopt;
    }
    result.process                           = std::move(*process);
    result.acknowledgment.connection_version = addon::k_delegated_connection_version;
    result.inspection                        = {true, save_state(state), true};
    result.profile_storage                   = request.context.profile_storage;
    result.account_context                   = request.context.account_context;
    result.model_selection                   = request.model_selection;
    return result;
}

addon::Structured_inspect_result inspect_rpc(const addon::Structured_inspection& inspection)
{
    auto restored = load_state(inspection.protocol_state);
    if (!restored) {
        return failed("omp_state_invalid", "The OMP RPC continuation is malformed or exceeds its bounds.");
    }
    auto& state = *restored;
    if (inspection.timed_out) {
        return failed("omp_timed_out", "The host's OMP task deadline expired.", "process_exit");
    }
    state.stderr_tail += inspection.stderr_bytes;
    if (state.stderr_tail.size() > k_error_bytes) {
        state.stderr_tail.erase(0, state.stderr_tail.size() - k_error_bytes);
    }
    state.pending += inspection.stdout_bytes;
    addon::Structured_inspect_result result;
    std::string input;
    std::size_t offset = 0;
    for (;;) {
        const auto newline = state.pending.find('\n', offset);
        if (newline == std::string::npos) {
            break;
        }
        const auto length = newline - offset;
        if (!length || length + 1 > state.frame_limit) {
            return failed("omp_output_too_large", "An OMP RPC line is empty or exceeds its advertised frame bound.");
        }
        auto frame = addon::parse_json(std::string_view(state.pending).substr(offset, length),
            addon::Json_parse_limits{k_frame_bytes, 32, 1024, k_frame_bytes}, nullptr);
        offset = newline + 1;
        if (!frame || !frame->is_object()) {
            return failed("omp_protocol_invalid", "OMP wrote malformed JSON or invalid UTF-8 to its RPC stream.");
        }
        if (json::text(*frame, "type") == "rpc_chunk") {
            const auto id = json::text(*frame, "chunkId");
            const auto& index = json::member(*frame, "index");
            const auto& count = json::member(*frame, "count");
            const auto& bytes = json::member(*frame, "byteLength");
            if (state.phase < Phase::FILTERING || id.empty() || id.size() > 128 ||
                !integer_in(index, 0, 255) || !integer_in(count, 2, 256) ||
                !integer_in(bytes, k_frame_bytes, 64 * 1024 * 1024))
            {
                return failed("omp_protocol_invalid", "OMP supplied invalid RPC chunk metadata.");
            }
            if (bytes.as_integer() > static_cast<std::int64_t>(k_logical_bytes)) {
                return failed("omp_output_too_large", "An OMP logical frame exceeds the retained exchange bound.");
            }
            const auto part = addon::decode_base64(json::text(*frame, "data"));
            if (!part || part->empty() || part->size() > k_chunk_bytes) {
                return failed("omp_protocol_invalid", "OMP supplied invalid RPC chunk bytes.");
            }
            if (state.chunk_id.empty()) {
                state.chunk_id = id;
                state.chunk_count = static_cast<std::size_t>(count.as_integer());
                state.chunk_length = static_cast<std::size_t>(bytes.as_integer());
            }
            if (id != state.chunk_id || static_cast<std::size_t>(index.as_integer()) != state.chunk_next ||
                static_cast<std::size_t>(count.as_integer()) != state.chunk_count ||
                static_cast<std::size_t>(bytes.as_integer()) != state.chunk_length ||
                state.chunk_next >= state.chunk_count || state.chunk.size() + part->size() > state.chunk_length)
            {
                return failed("omp_protocol_invalid", "OMP's RPC chunk sequence is inconsistent or out of order.");
            }
            state.chunk += *part;
            if (++state.chunk_next != state.chunk_count) {
                continue;
            }
            if (state.chunk.size() != state.chunk_length) {
                return failed("omp_protocol_invalid", "OMP's RPC chunk length does not match its declaration.");
            }
            frame = addon::parse_json(state.chunk,
                addon::Json_parse_limits{k_logical_bytes, 32, 1024, k_result_bytes}, nullptr);
            state.chunk.clear();
            state.chunk_id.clear();
            state.chunk_next = state.chunk_count = state.chunk_length = 0;
            if (!frame || !frame->is_object()) {
                return failed("omp_protocol_invalid", "OMP's reassembled RPC frame is malformed.");
            }
        }
        else
        if (!state.chunk_id.empty()) {
            return failed("omp_protocol_invalid", "OMP interrupted an RPC chunk sequence.");
        }
        if (auto error = consume_frame(state, *frame, input, result)) {
            return *error;
        }
    }
    state.pending.erase(0, offset);
    if (state.pending.size() >= state.frame_limit) {
        return failed("omp_output_too_large", "An unfinished OMP RPC line exceeds its frame bound.");
    }
    if (input.size() > addon::limits::k_max_structured_stream_bytes) {
        return failed("omp_input_too_large", "The queued OMP input exceeds the structured exchange bound.");
    }
    if (inspection.final) {
        if (!state.pending.empty() || !state.chunk_id.empty()) {
            return failed("omp_protocol_truncated", "OMP exited inside a JSONL frame or chunk sequence.");
        }
        if (inspection.exit_code != 0) {
            return failed("omp_process_failed", "OMP exited with code " + std::to_string(inspection.exit_code) +
                ".\n" + diagnostic_text(state.stderr_tail), "process_exit");
        }
        if (!state.acknowledged || !state.completed || !state.settled || !input.empty()) {
            return failed("omp_incomplete", "OMP exited before the task was acknowledged, completed and settled.", "process_exit");
        }
        result.complete = true;
        result.result_bytes = std::move(state.output);
        presented(result, addon::Presentation_event_kind::RESULT_AUTHENTICATED);
        return result;
    }
    if (state.phase == Phase::RUNNING && state.acknowledged && state.completed && state.settled) {
        result.close_stdin = true;
        state.phase = Phase::DRAINING;
    }
    if (!input.empty()) {
        result.stdin_bytes = std::move(input);
    }
    result.protocol_state = save_state(state);
    if (!addon::exchange_protocol_state_valid(result.protocol_state) ||
        !addon::validate_session_exchange_payload(addon::encode_structured_inspect_result(result), nullptr))
    {
        return failed("omp_output_too_large", "OMP's retained RPC state exceeds the structured exchange bound.");
    }
    return result;
}

void handle_structured_session(addon::Invocation_context& context)
{
    addon::Contract_failure failure;
    if (context.request().operation == addon::k_prepare_operation) {
        const auto request = addon::decode_structured_prepare_request(context.request().payload, &failure);
        if (!request) {
            context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
            return;
        }
        const auto result = prepare_structured_session(*request, &failure);
        if (!result) {
            context.fail(addon::Result_status::UNAVAILABLE, failure.reason_code, failure.detail);
            return;
        }
        context.succeed(addon::encode_structured_prepare_result(*result));
    }
    else
    if (context.request().operation == addon::k_inspect_operation) {
        const auto request = addon::decode_structured_inspect_request(context.request().payload, &failure);
        if (!request) {
            context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
            return;
        }
        context.succeed(addon::encode_structured_inspect_result(inspect_rpc(request->inspection), request->report_failure_diagnostics));
    }
    else {
        context.reject_unknown_operation();
    }
}

void handle_delegated_session(addon::Invocation_context& context)
{
    if (context.request().operation != addon::k_prepare_operation) {
        context.reject_unknown_operation();
        return;
    }
    addon::Contract_failure failure;
    const auto request = addon::decode_delegated_prepare_request(context.request().payload, &failure);
    if (!request) {
        context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
        return;
    }
    const auto result = prepare_delegated_session(*request, &failure);
    if (!result) {
        context.fail(addon::Result_status::UNAVAILABLE, failure.reason_code, failure.detail);
        return;
    }
    context.succeed(addon::encode_delegated_prepare_result(*result));
}

} // namespace omp
