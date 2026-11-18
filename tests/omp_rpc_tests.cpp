#include "omp_rpc.h"
#include "omp_profiles.h"

#include <logonomic_cli_addon/json_utilities.h>
#include <logonomic_cli_addon/package_source.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
namespace addon = logonomic::cli_addon;
namespace json = addon::json;
using Json = addon::Json_value;

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

Json parse(std::string_view text)
{
    auto value = addon::parse_json(text, {}, nullptr);
    require(value.has_value(), "Test JSON must parse");
    return std::move(*value);
}

std::string line(const Json& value)
{
    return addon::serialize_json(value) + '\n';
}

Json response(const char* id, const char* command, Json data = Json::object())
{
    return json::object({{"type", Json("response")}, {"id", Json(id)}, {"command", Json(command)},
        {"success", Json(true)}, {"data", std::move(data)}});
}

const Json k_ready = parse(R"({"type":"ready","protocolVersion":1,"supportedProtocolVersions":[1,2],"maxFrameBytes":1048576,"maxReassembledFrameBytes":67108864,"supportsBashExcludeFromContext":true})");
const Json k_idle = parse(R"({"isSettled":true,"isStreaming":false,"hasPendingAsyncWork":false,"queuedMessageCount":0,"model":{"provider":"test","id":"exact-model"},"thinkingLevel":"high","dumpTools":[]})");

Json prompt_result(bool settled = true, const char* status = "completed")
{
    return json::object({{"type", Json("prompt_result")}, {"id", Json("lg-prompt")},
        {"agentInvoked", Json(true)}, {"status", Json(status)}, {"sessionSettled", Json(settled)}});
}

Json assistant(const char* text)
{
    return json::object({{"type", Json("message_end")}, {"messageId", Json("msg-2")},
        {"message", json::object({{"role", Json("assistant")}, {"stopReason", Json("stop")},
            {"provider", Json("test")}, {"model", Json("exact-model")},
            {"content", Json(Json::Array{json::object({{"type", Json("thinking")}, {"thinking", Json("private reasoning")}}),
                json::object({{"type", Json("text")}, {"text", Json(text)}})})}})}});
}

class Temporary_directory
{
public:
    explicit Temporary_directory(const char* executable)
    {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        m_path = std::filesystem::temp_directory_path() / addon::path_from_utf8("omp rpc test \xc3\xa9 " + std::to_string(nonce));
        std::filesystem::create_directory(m_path);
#ifdef _WIN32
        m_program = m_path / "omp.exe";
#else
        m_program = m_path / "omp";
#endif
        std::filesystem::copy_file(std::filesystem::absolute(executable), m_program);
    }

    ~Temporary_directory()
    {
        std::error_code error;
        std::filesystem::remove_all(m_path, error);
    }

    std::string path() const { return addon::path_to_utf8(m_path); }
    std::string program() const { return addon::path_to_utf8(m_program); }

private:
    std::filesystem::path m_path;
    std::filesystem::path m_program;
};

addon::Structured_prepare_request request_for(const Temporary_directory& directory)
{
    addon::Structured_prepare_request request;
    request.purpose = addon::Structured_purpose::IMPLEMENTATION;
    request.prompt = "Implement the requested task.\nPreserve the selected configuration.";
    request.context.launcher = {'"' + directory.program() + "\" --profile saved --approval-mode write --model test/exact-model", "direct", ""};
    request.context.launcher_argv = {directory.program(), "--profile", "saved", "--approval-mode", "write", "--model", "test/exact-model"};
    request.context.working_directory = directory.path();
#ifdef _WIN32
    const char* platform = "windows-x86_64";
#elif defined(__APPLE__)
    const char* platform = "macos-arm64";
#else
    const char* platform = "linux-x86_64";
#endif
    request.context.launch_environment = addon::Launch_environment{platform,
        json::object({{"PI_CODING_AGENT_DIR", Json(directory.path())}, {"CUSTOM_SAVED_TOKEN", Json("kept-in-environment")},
            {"LOGONOMIC_PROVIDER_HELPERS_DIR", Json("host-owned")}}), std::nullopt};
    request.context.launch_environment->effective_environment.set(
#ifdef _WIN32
        "USERPROFILE",
#else
        "HOME",
#endif
        Json(directory.path()));
    const auto profile = omp::resolve_native_profile(request.context.launcher, *request.context.launch_environment,
        directory.path(), {}, request.context.launcher_argv, {}, nullptr);
    require(profile.has_value(), "The saved native profile fixture must resolve");
    request.context.account_context = addon::Account_context{2, profile->account_key,
        addon::path_to_utf8(profile->source_profile)};
    request.context.profile_storage = addon::Profile_storage_context{1, directory.path()};
    return request;
}

class Conversation
{
public:
    explicit Conversation(std::string state, std::uint32_t model_state_version = 0)
    :
        m_state(std::move(state)),
        m_model_state_version(model_state_version)
    {}

    addon::Structured_inspect_result bytes(std::string output, bool final = false, int exit_code = 0)
    {
        addon::Structured_inspect_request request;
        request.model_state_version = m_model_state_version;
        auto& inspection = request.inspection;
        inspection.protocol_state = m_state;
        inspection.stdout_bytes = std::move(output);
        inspection.final = final;
        inspection.exit_code = exit_code;
        auto result = omp::inspect_rpc(request);
        const auto encoded = addon::encode_structured_inspect_result(result, true);
        addon::Contract_failure failure;
        require(addon::decode_structured_inspect_result(encoded, final, &failure).has_value(),
            "Every parser reply must conform to the SDK inspection contract");
        if (!result.complete) {
            m_state = result.protocol_state;
        }
        return result;
    }

    addon::Structured_inspect_result send(const Json& value)
    {
        return bytes(line(value));
    }

    addon::Structured_inspect_result start(Json native = k_idle)
    {
        const auto ready = send(k_ready);
        require(ready.stdin_bytes && json::text(parse(*ready.stdin_bytes), "type") == "negotiate_protocol",
            "Readiness must negotiate lossless v2 before any task is submitted");
        const auto negotiated = send(response("lg-protocol", "negotiate_protocol", json::object({{"protocolVersion", Json(2)}})));
        require(negotiated.stdin_bytes && json::text(parse(*negotiated.stdin_bytes), "type") == "set_event_filter",
            "Negotiation must configure delta projection");
        const auto filtered = send(response("lg-filter", "set_event_filter"));
        require(filtered.stdin_bytes && json::text(parse(*filtered.stdin_bytes), "type") == "get_state",
            "Task submission must wait for native readiness and catalog inspection");
        return send(response("lg-state", "get_state", std::move(native)));
    }

    const std::string& state() const { return m_state; }

private:
    std::string m_state;
    std::uint32_t m_model_state_version;
};

std::string prepared_state(const addon::Structured_prepare_request& request)
{
    addon::Contract_failure failure;
    const auto prepared = omp::prepare_structured_session(request, &failure);
    if (!prepared) {
        throw std::runtime_error("Preparation failed: " + failure.reason_code + " " + failure.detail);
    }
    return prepared->inspection.protocol_state;
}

void preparation_contract(const Temporary_directory& directory)
{
    auto request = request_for(directory);
    addon::Contract_failure failure;
    const auto result = omp::prepare_structured_session(request, &failure);
    require(result.has_value(), "A selected native launcher must prepare");
    require(result->process.argv.front() == directory.program(), "The bridge must use the exact selected native program");
    require(std::equal(request.context.launcher_argv.begin() + 1, request.context.launcher_argv.end(),
        result->process.argv.begin() + 1), "Saved model, profile and permission arguments must remain byte-exact");
    require(result->process.argv[result->process.argv.size() - 3] == "--mode" &&
        result->process.argv[result->process.argv.size() - 2] == "rpc" &&
        result->process.argv.back() == "--no-ui", "The adapter must choose headless native RPC");
    require(result->process.stdin_payload.empty() && result->inspection.keep_stdin_open && result->inspection.incremental,
        "Preparation must await native ready before writing the prompt");
    require(result->process.environment.empty(), "Preparation must not invent credential or permission environment overrides");
    require(std::find(result->process.environment_names.begin(), result->process.environment_names.end(), "CUSTOM_SAVED_TOKEN") !=
        result->process.environment_names.end(), "Explicit saved environment must survive replacement");
    require(std::find(result->process.environment_names.begin(), result->process.environment_names.end(), "LOGONOMIC_PROVIDER_HELPERS_DIR") ==
        result->process.environment_names.end(), "The adapter must not propose host-reserved environment names");
    require(result->account_context && result->account_context->adapter_account_key == request.context.account_context->adapter_account_key &&
        result->account_context->version == 2 && result->account_context->profile_path == request.context.account_context->profile_path &&
        result->profile_storage && result->profile_storage->root == directory.path(), "Selected account and storage must be echoed");
    require(addon::decode_structured_prepare_result(addon::encode_structured_prepare_result(*result), &failure).has_value(),
        "Preparation must round-trip the public SDK contract");

    request.purpose = addon::Structured_purpose::READ_ONLY_REVIEW;
    require(!omp::prepare_structured_session(request, &failure) && failure.reason_code == "read_only_unavailable",
        "A review cannot launch without the host-adopted managed guard");
    request = request_for(directory);
    request.prompt = std::string("bad\xff", 4);
    require(!omp::prepare_structured_session(request, &failure), "A binary task must not corrupt the native JSON protocol");
    for (const auto* argument : {"--resume", "--mode=rpc-ui", "--cwd=/elsewhere", "--alias=saved", "startup message", "--unknown"}) {
        request = request_for(directory);
        request.context.launcher.script_body += " " + std::string(argument);
        request.context.launcher_argv.push_back(argument);
        require(!omp::prepare_structured_session(request, &failure), "Ambiguous or task-bearing launchers must be refused");
    }
}

void review_contract(const Temporary_directory& directory)
{
    auto request = request_for(directory);
    request.purpose = addon::Structured_purpose::READ_ONLY_REVIEW;
    request.context.declared_assets = {"assets/native_review.ts"};
    addon::Contract_failure failure;
    const auto prepared = omp::prepare_structured_session(request, &failure);
    if (!prepared) {
        throw std::runtime_error("A native read-only review must prepare: " + failure.reason_code + " " + failure.detail);
    }
    require(prepared->process.executable.path.find("helpers/omp_review_bridge") == 0 &&
        prepared->process.assets == request.context.declared_assets,
        "Only the review bridge may translate the materialized guard into native argv");
    const auto& environment = prepared->process.environment;
    const auto asset = std::find_if(environment.begin(), environment.end(), [](const auto& pair) {
        return pair.first == "OMP_LOGONOMIC_REVIEW_ASSET";
    });
    const auto policy = std::find_if(environment.begin(), environment.end(), [](const auto& pair) {
        return pair.first == "OMP_LOGONOMIC_REVIEW_POLICY";
    });
    require(asset != environment.end() && asset->second == "{{asset:assets/native_review.ts}}" &&
        policy != environment.end(), "Managed asset substitution belongs in the native bridge environment");
    const auto state = parse(prepared->inspection.protocol_state);
    require(json::text(parse(policy->second), "scope") == json::text(state, "review_scope") &&
        json::member(parse(policy->second), "tools") == Json::array(),
        "A nondelegated review receives no task-control execution authority");
    auto idle = k_idle;
    idle.set("sessionId", Json("review-native-session"));
    const auto marker = json::object({{"type", Json("custom")}, {"customType", Json("logonomic-read-only")},
        {"data", json::object({{"version", Json(1)}, {"scope", json::member(state, "review_scope")},
            {"session_id", Json("review-native-session")}})}});
    for (const auto& entries : {
        Json::array(),
        Json(Json::Array{marker, marker}),
        Json(Json::Array{json::object({{"type", Json("custom")}, {"customType", Json("logonomic-read-only")},
            {"data", json::object({{"version", Json(1)}, {"scope", json::member(state, "review_scope")},
                {"session_id", Json("other-session")}})}})}),
        Json(Json::Array{json::object({{"type", Json("custom")}, {"customType", Json("logonomic-read-only")},
            {"data", json::object({{"version", Json(1)}, {"scope", Json(std::string(32, 'b'))},
                {"session_id", Json("review-native-session")}})}})}),
    }) {
        Conversation refused(prepared->inspection.protocol_state);
        const auto readiness = refused.start(idle);
        require(readiness.stdin_bytes && json::text(parse(*readiness.stdin_bytes), "type") == "get_entries",
            "Native idle state alone cannot release a review prompt");
        const auto result = refused.send(response("lg-tools", "get_entries", json::object({{"entries", entries}})));
        require(result.failure_reason == "read_only_unavailable" && !result.stdin_bytes,
            "Only one exact active guard acknowledgment may release a review task");
    }
    Conversation admitted(prepared->inspection.protocol_state);
    admitted.start(idle);
    const auto ready = admitted.send(response("lg-tools", "get_entries",
        json::object({{"entries", Json(Json::Array{marker})}})));
    require(ready.stdin_bytes && json::text(parse(*ready.stdin_bytes), "type") == "prompt",
        "An exact active native read boundary must release the unchanged review prompt");

    request.prompt = " \n/config change";
    require(!omp::prepare_structured_session(request, &failure) && failure.reason_code == "purpose_unavailable",
        "Native slash-command dispatch is not a read-only model task");
    request.prompt = "Review the files.";
    for (const auto* argument : {"--extension=unsafe.ts", "--trusted-extension=unsafe.ts", "--tools=bash", "--plan-yolo", "--advisor"}) {
        auto conflicting = request;
        conflicting.context.launcher.script_body += " " + std::string(argument);
        conflicting.context.launcher_argv.push_back(argument);
        require(!omp::prepare_structured_session(conflicting, &failure),
            "The review policy must refuse a conflicting explicit launcher option");
    }
}

void ambient_profile_contract(const Temporary_directory& directory)
{
    const auto sidecars = std::filesystem::path(__FILE__).parent_path().parent_path() / "package" / "capabilities";
    for (const auto* capability : {"structured-session@3.json", "delegated-session@3.json"}) {
        std::ifstream stream(sidecars / capability, std::ios::binary);
        require(stream.good(), "The tested capability declaration must be available");
        const auto sidecar = parse(std::string(std::istreambuf_iterator<char>(stream), {}));
        addon::Contract_failure failure;
        const auto declaration = addon::decode_purposes_declaration(json::member(sidecar, "payload"), &failure);
        require(declaration.has_value(), "The environment declaration must satisfy SDK bounds");
        const std::vector<std::pair<std::string, std::string>> ambient = {
            {"OMP_PROFILE", ""}, {"PI_PROFILE", "ignored-legacy-profile"}, {"PI_CONFIG_DIR", ".omp-task-profile"},
            {"PI_CODING_AGENT_DIR", directory.path()}, {"XDG_STATE_HOME", directory.path()},
            {"XDG_CACHE_HOME", directory.path()}, {"OPENAI_API_KEY", "fixture-key"},
            {"OPENAI_BASE_URL", "http://127.0.0.1:1"}, {"ANTHROPIC_BASE_URL", "http://127.0.0.1:2"},
            {"AZURE_OPENAI_BASE_URL", "http://127.0.0.1:3"}, {"AWS_DEFAULT_REGION", "us-east-2"},
        };
        auto request = request_for(directory);
        request.context.launcher = {'"' + directory.program() + '"', "direct", ""};
        request.context.launcher_argv = {directory.program()};
        request.context.account_context.reset();
        auto& environment = *request.context.launch_environment;
        environment.effective_environment = addon::ambient_view(ambient,
            addon::Launcher_discovery_declaration{declaration->environment_names}, environment.platform_id);
        for (const auto& [name, value] : ambient) {
            require(json::member(environment.effective_environment, name) == Json(value),
                "Ambient profile, path and model routing selectors must survive SDK allowlisting exactly");
        }
        const auto prepared = omp::prepare_structured_session(request, &failure);
        require(prepared.has_value(), "An ambient-only selected profile must prepare without launcher assignments");
        for (const auto& [name, value] : ambient) {
            static_cast<void>(value);
            require(std::find(prepared->process.environment_names.begin(), prepared->process.environment_names.end(), name) !=
                prepared->process.environment_names.end(), "The process must inherit every frozen ambient selector");
        }
    }
}

void serving_model_contract(const addon::Structured_prepare_request& request)
{
    Conversation conversation(prepared_state(request), 1);
    require(!conversation.start().model_observation, "The configured model does not identify a serving model");
    conversation.send(response("lg-prompt", "prompt"));

    auto fallback = assistant("retry answer");
    fallback.find("message")->set("model", Json("served-fallback"));
    const auto sample = conversation.bytes(line(assistant("first attempt")) + line(fallback));
    require(sample.model_observation && sample.model_observation->provider == "test" &&
        sample.model_observation->model == "served-fallback",
        "The latest native assistant message identifies the serving model even when it differs from the launcher");
    fallback.find("message")->set("model", Json("invalid\nlabel"));
    const auto malformed = conversation.send(fallback);
    require(!malformed.model_observation && !malformed.failure_reason,
        "An invalid optional model label leaves the native task usable");
    const auto final = conversation.bytes(line(assistant("finished")) + line(prompt_result()), true);
    require(final.result_bytes == "finished" && final.model_observation && final.model_observation->model == "exact-model",
        "A final inspection carries the native assistant model alongside its answer");
}

void completion_contract(const addon::Structured_prepare_request& request)
{
    Conversation conversation(prepared_state(request));
    const auto started = conversation.start();
    require(started.stdin_bytes && json::text(parse(*started.stdin_bytes), "message") == request.prompt,
        "The exact task must be submitted only after readiness");
    const auto admitted = conversation.send(response("lg-prompt", "prompt"));
    require(!admitted.complete && !admitted.close_stdin, "Prompt admission is not task completion");
    conversation.send(parse(R"({"type":"message_update","messageId":"msg-2","assistantMessageEvent":{"type":"thinking_delta","delta":"secret thought"}})"));
    conversation.send(parse(R"({"type":"message_update","messageId":"msg-2","assistantMessageEvent":{"type":"text_delta","delta":"partial answer"}})"));
    require(!conversation.send(assistant("first yield")).model_observation,
        "An inspection without model-state negotiation carries only the ordinary task result");
    const auto ended = conversation.send(parse(R"({"type":"agent_end","messages":[],"messageCount":3,"isTerminal":true,"awaitingAsyncWork":true})"));
    require(!ended.complete && !ended.close_stdin, "An agent_end must never authenticate completion");
    const auto yielded = conversation.send(prompt_result(false));
    require(!yielded.complete && !yielded.close_stdin, "Pending async work must keep the native session alive");
    conversation.send(assistant("final answer after background work"));
    const auto settled = conversation.send(parse(R"({"type":"session_settled"})"));
    require(!settled.complete && settled.close_stdin && !settled.protocol_state.empty(),
        "Settled work requests EOF while retaining state for final native inspection");
    const auto final = conversation.bytes({}, true);
    require(final.complete && final.result_bytes == "final answer after background work" && final.protocol_state.empty(),
        "Only the authoritative final assistant text may become the task result");
    require(final.presentation.back().kind == addon::Presentation_event_kind::RESULT_AUTHENTICATED,
        "Authenticated result presentation belongs after native process completion");

    Conversation early_result(prepared_state(request));
    early_result.start();
    early_result.send(assistant("fast"));
    const auto before_ack = early_result.send(prompt_result());
    require(!before_ack.complete && !before_ack.close_stdin, "Even a prompt result must wait for its admission response");
    require(early_result.send(response("lg-prompt", "prompt")).close_stdin, "Either response ordering must settle correctly");

    Conversation local(prepared_state(request));
    local.start();
    local.send(parse(R"({"type":"command_output","text":"local status"})"));
    require(local.send(response("lg-prompt", "prompt", json::object({{"agentInvoked", Json(false)}}))).close_stdin,
        "Synchronous local commands explicitly report that no agent was invoked");
    require(local.bytes({}, true).result_bytes == "local status", "Local command output must be preserved");
}

void failure_contract(const addon::Structured_prepare_request& request)
{
    const auto state = prepared_state(request);
    Conversation old(state);
    auto ready = k_ready;
    ready.set("supportedProtocolVersions", Json(Json::Array{Json(1)}));
    require(old.send(ready).failure_reason == "update_required", "Lossy RPC v1 must be refused");
    Conversation busy(state);
    auto idle = k_idle;
    idle.set("hasPendingAsyncWork", Json(true));
    require(busy.start(idle).failure_reason == "omp_session_not_idle", "Prior native work must not be confused with the submitted task");

    for (const auto* status : {"error", "aborted"}) {
        Conversation failed_task(state);
        failed_task.start();
        failed_task.send(response("lg-prompt", "prompt"));
        auto frame = prompt_result(true, status);
        frame.set("error", json::object({{"message", Json("native provider failure")}, {"retryable", Json(false)}}));
        const auto result = failed_task.send(frame);
        require(result.complete && result.failure_reason && !result.result_bytes,
            "A native failed or aborted prompt must never return success after a successful admission");
    }
    Conversation question(state);
    question.start();
    const auto declined = question.send(parse(R"({"type":"extension_ui_request","id":"ask-1","method":"confirm","title":"Approve tool?","message":"Run a command"})"));
    require(declined.failure_reason == "hook_approval_required" && !declined.stdin_bytes,
        "Headless UI requests must fail without fabricating approval");

    for (int mode = 0; mode < 4; ++mode) {
        Conversation tail(state);
        tail.start();
        tail.send(response("lg-prompt", "prompt"));
        tail.send(assistant("looks successful"));
        require(tail.send(prompt_result()).close_stdin, "A settled prompt must enter final drain");
        addon::Structured_inspect_result result;
        if (mode == 0) {
            result = tail.bytes({}, true, 1);
        }
        else
        if (mode == 1) {
            result = tail.bytes("{\"type\":\"notice\"", true);
        }
        else
        if (mode == 2) {
            result = tail.bytes(line(parse(R"({"type":"notice","level":"error","source":"session-persistence","message":"Could not persist session"})")), true);
        }
        else {
            result = tail.bytes("invalid json\n", true);
        }
        require(result.failure_reason.has_value() && !result.result_bytes,
            "Late output, persistence errors and failed exits must invalidate apparent success");
    }
    Conversation wrong_id(state);
    wrong_id.start();
    auto terminal = prompt_result();
    terminal.set("id", Json("other-task"));
    require(wrong_id.send(terminal).failure_reason == "omp_protocol_invalid", "Completion must correlate the exact prompt ID");
    Conversation incomplete(state);
    incomplete.start();
    incomplete.send(response("lg-prompt", "prompt"));
    incomplete.send(assistant("partial"));
    require(incomplete.bytes({}, true).failure_reason == "omp_incomplete", "Exit zero alone must not authenticate completion");
    addon::Structured_inspect_request timeout_request;
    auto& timed = timeout_request.inspection;
    timed.protocol_state = state;
    timed.final = true;
    timed.timed_out = true;
    require(omp::inspect_rpc(timeout_request).failure_reason == "omp_timed_out", "The host task timeout remains authoritative");

    Conversation persistent(state);
    persistent.start();
    persistent.send(response("lg-prompt", "prompt"));
    persistent.send(assistant("A background service is running"));
    require(!persistent.send(prompt_result(false)).close_stdin,
        "An unsuppressed background service remains native pending work");
    require(!persistent.bytes({}).complete && !persistent.bytes({}).close_stdin,
        "Silence must not fabricate settlement or suppress background delivery");
    timed.protocol_state = persistent.state();
    require(omp::inspect_rpc(timeout_request).failure_reason == "omp_timed_out",
        "A persistent native job must not defeat the original host task deadline");
}

void framing_contract(const addon::Structured_prepare_request& request)
{
    Conversation fragmented(prepared_state(request));
    fragmented.start();
    fragmented.send(response("lg-prompt", "prompt"));
    const auto text = line(assistant("UTF-8: \xc3\xa9"));
    const auto split = text.find('\xc3') + 1;
    require(!fragmented.bytes(text.substr(0, split)).complete, "A split UTF-8 sequence must be retained as raw bytes");
    require(!fragmented.bytes(text.substr(split)).complete, "UTF-8 validation belongs at the complete frame boundary");
    fragmented.send(prompt_result());
    require(fragmented.bytes({}, true).result_bytes == "UTF-8: \xc3\xa9", "Multibyte output must survive fragmented transport");

    Conversation chunks(prepared_state(request));
    chunks.start();
    chunks.send(response("lg-prompt", "prompt"));
    const auto content = Json(Json::Array{
        json::object({{"type", Json("text")}, {"text", Json(std::string(550000, 'a'))}}),
        json::object({{"type", Json("text")}, {"text", Json(std::string(550000, 'b'))}}),
    });
    const auto logical = addon::serialize_json(json::object({{"type", Json("tool_execution_end")},
        {"toolCallId", Json("call-1")}, {"toolName", Json("read")}, {"result", json::object({{"content", content}})}}));
    constexpr std::size_t chunk_bytes = 256 * 1024;
    const auto count = (logical.size() + chunk_bytes - 1) / chunk_bytes;
    for (std::size_t index = 0; index < count; ++index) {
        const auto chunk = json::object({{"type", Json("rpc_chunk")}, {"chunkId", Json("rpc-1")},
            {"index", Json(static_cast<std::uint64_t>(index))}, {"count", Json(static_cast<std::uint64_t>(count))},
            {"byteLength", Json(static_cast<std::uint64_t>(logical.size()))},
            {"data", Json(addon::encode_base64(std::string_view(logical).substr(index * chunk_bytes, chunk_bytes)))}});
        const auto part = chunks.send(chunk);
        require(!part.complete && !part.failure_reason, "Valid v2 chunks must reassemble within the host continuation bound");
    }
    chunks.send(assistant("small final answer"));
    chunks.send(prompt_result());
    require(chunks.bytes({}, true).result_bytes == "small final answer", "Large tool output must not become the answer");

    const auto first = json::object({{"type", Json("rpc_chunk")}, {"chunkId", Json("rpc-2")},
        {"index", Json(0)}, {"count", Json(5)}, {"byteLength", Json(1100000)},
        {"data", Json(addon::encode_base64(std::string(chunk_bytes, 'a')))}});
    Conversation interrupted(prepared_state(request));
    interrupted.start();
    require(!interrupted.send(first).complete, "A first chunk may be retained");
    require(interrupted.send(parse(R"({"type":"agent_end","messages":[]})")).failure_reason == "omp_protocol_invalid",
        "Chunk sequences must not be interrupted by unrelated frames");
    Conversation oversized(prepared_state(request));
    oversized.start();
    auto huge = first;
    huge.set("byteLength", Json(64 * 1024 * 1024));
    require(oversized.send(huge).failure_reason == "omp_output_too_large", "Host bounds must reject lossless frames they cannot retain");
}

void delegation_contract(const Temporary_directory& directory)
{
    const auto structured = request_for(directory);
    addon::Delegated_prepare_request request;
    request.purpose = structured.purpose;
    request.context = structured.context;
    request.prompt = structured.prompt;
    request.control.task_id = "task-one";
    request.control.private_artifact_root = directory.path();
    request.control.connection = addon::Delegated_connection{directory.program(), directory.path() + "/descriptor.json"};
    request.control.servers.push_back({"tasks", "must-not-launch-original-server", {"report_result", "send_message"}, {{"ORIGINAL_SECRET", "must-not-copy"}}});
    request.model_selection = addon::Delegated_model_selection{std::string("test/exact-model"), std::string("high")};
    addon::Contract_failure failure;
    const auto result = omp::prepare_delegated_session(request, &failure);
    require(result.has_value(), "Delegation must prepare the verified native plugin route");
    require(result->private_artifacts.size() == 4 && result->private_artifacts[0].relative_path == ".claude-plugin/plugin.json" &&
        result->private_artifacts[1].relative_path == ".mcp.json", "The host relay must use OMP's plugin format that accepts an absolute command");
    const auto plugin_flag = std::find(result->process.argv.begin(), result->process.argv.end(), "--plugin-dir");
    require(plugin_flag != result->process.argv.end() && *(plugin_flag + 1) == directory.path(),
        "The exact private-artifact root must be passed to native plugin discovery");
    const auto config = parse(result->private_artifacts[1].content);
    const auto& server = json::member(json::member(config, "mcpServers"), "s0");
    require(json::text(server, "command") == directory.program() && !server.find("env"),
        "Delegation must launch only the host relay and must not copy original server credentials");
    require(json::member(server, "args") == parse("[\"--descriptor\"," + addon::serialize_json(Json(request.control.connection->descriptor_path)) +
        ",\"--server\",\"tasks\"]"), "Relay descriptor and original server ID must remain exact");
    require(addon::validate_delegated_acknowledgment(request.control, result->acknowledgment, &failure) &&
        addon::validate_delegated_model_selection_acknowledgment(request.model_selection, result->model_selection, &failure),
        "Delegation and model acknowledgments must satisfy the SDK's exact echo checks");
    require(addon::decode_delegated_prepare_result(addon::encode_delegated_prepare_result(*result), &failure).has_value(),
        "Delegation preparation must round-trip the SDK wire contract");
    const auto selection = parse(result->private_artifacts.back().content);
    require(json::member(json::member(selection, "retry"), "modelFallback") == Json(false) &&
        json::member(json::member(selection, "contextPromotion"), "enabled") == Json(false) && !selection.find("tools"),
        "Explicit model selection must prevent native automatic replacement without changing tool or approval settings");

    Conversation missing(result->inspection.protocol_state);
    require(json::text(parse(*missing.start().stdin_bytes), "type") == "get_entries",
        "Delegated readiness must use native enabled MCP metadata, including xd devices");
    const auto empty_inventory = json::object({{"entries", Json::array()}, {"leafId", Json()}});
    require(missing.send(response("lg-tools", "get_entries", empty_inventory)).failure_reason == "task_control_unavailable",
        "A configured relay is insufficient without task-scoped native action visibility");
    auto idle = k_idle;
    auto tools = Json::array();
    const auto state = parse(result->inspection.protocol_state);
    for (const auto& wanted : json::member(state, "expected_tools").as_array()) {
        auto observed = wanted;
        observed.set("name", Json("mcp__native_" + json::text(wanted, "action")));
        tools.push_back(std::move(observed));
    }
    const auto inventory_entry = json::object({{"type", Json("custom")}, {"customType", Json("logonomic-task-control")},
        {"id", Json("entry-1")}, {"data", json::object({{"version", Json(1)}, {"scope", json::member(state, "tool_scope")}, {"tools", tools}})}});
    const auto inventory = json::object({{"entries", Json(Json::Array{inventory_entry})}, {"leafId", Json("entry-1")}});
    Conversation admitted(result->inspection.protocol_state);
    admitted.start(idle);
    require(json::text(parse(*admitted.send(response("lg-tools", "get_entries", inventory)).stdin_bytes), "type") == "prompt",
        "Enabled MCP actions mounted under xd must unlock the prompt with no top-level schemas");
    admitted.send(response("lg-prompt", "prompt"));
    auto substituted = assistant("wrong model");
    substituted.find("message")->set("model", Json("fallback-model"));
    require(admitted.send(substituted).failure_reason == "model_selection_unavailable",
        "Later assistant messages must retain the selected native provider and model");

    for (const auto& change : {
        parse(R"({"type":"retry_fallback_applied","from":"test/exact-model","to":"test/fallback-model","role":"default"})"),
        parse(R"({"type":"thinking_level_changed","thinkingLevel":"medium"})")})
    {
        Conversation changed(result->inspection.protocol_state);
        changed.start(idle);
        changed.send(response("lg-tools", "get_entries", inventory));
        require(changed.send(change).failure_reason == "model_selection_unavailable",
            "Native fallback or thinking changes must fail an explicit task selection");
    }
    auto unrelated_entry = inventory_entry;
    unrelated_entry.find("data")->set("scope", Json("unrelated-task"));
    Conversation stale(result->inspection.protocol_state);
    stale.start(idle);
    require(stale.send(response("lg-tools", "get_entries", json::object({{"entries", Json(Json::Array{unrelated_entry})}}))).failure_reason ==
        "task_control_unavailable", "An unrelated or stale task inventory must not establish readiness");
    auto duplicate = inventory;
    duplicate.find("entries")->push_back(inventory_entry);
    Conversation ambiguous(result->inspection.protocol_state);
    ambiguous.start(idle);
    require(ambiguous.send(response("lg-tools", "get_entries", duplicate)).failure_reason == "task_control_unavailable",
        "Duplicate task inventory records must not be accepted ambiguously");
    idle.set("thinkingLevel", Json("medium"));
    Conversation clamped(result->inspection.protocol_state);
    require(clamped.start(idle).failure_reason == "model_selection_unavailable", "Silent native thinking-level clamping must not be acknowledged");

    request.model_selection.reset();
    const auto inherited = omp::prepare_delegated_session(request, &failure);
    require(inherited && inherited->private_artifacts.size() == 3,
        "Omitted selection must leave the user's native retry and context-promotion defaults intact");

    request.control.connection.reset();
    require(!omp::prepare_delegated_session(request, &failure), "Delegation must not launch original servers without a host relay");
}

// The native acceptance runner uses the same public SDK payloads and parser
// as the adapter, with a fresh process for each host inspection.
int exchange_driver(std::string_view operation)
{
    const std::string input(std::istreambuf_iterator<char>(std::cin), {});
    const auto payload = parse(input);
    addon::Contract_failure failure;
    Json output;
    if (operation == "--inspect") {
        const auto request = addon::decode_structured_inspect_request(payload, &failure);
        require(request.has_value(), "Native inspection request must match the SDK contract");
        output = addon::encode_structured_inspect_result(omp::inspect_rpc(*request), true);
    }
    else
    if (operation == "--prepare") {
        const auto request = addon::decode_structured_prepare_request(payload, &failure);
        require(request.has_value(), "Native preparation request must match the SDK contract");
        const auto result = omp::prepare_structured_session(*request, &failure);
        if (!result) {
            throw std::runtime_error(failure.reason_code + ": " + failure.detail);
        }
        output = addon::encode_structured_prepare_result(*result);
    }
    else {
        const auto request = addon::decode_delegated_prepare_request(payload, &failure);
        require(request.has_value(), "Native delegation request must match the SDK contract");
        const auto result = omp::prepare_delegated_session(*request, &failure);
        if (!result) {
            throw std::runtime_error(failure.reason_code + ": " + failure.detail);
        }
        output = addon::encode_delegated_prepare_result(*result);
    }
    std::cout << addon::serialize_json(output) << '\n';
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        if (argc == 2 && (std::string_view(argv[1]) == "--prepare" || std::string_view(argv[1]) == "--prepare-delegated" ||
            std::string_view(argv[1]) == "--inspect"))
        {
            return exchange_driver(argv[1]);
        }
        Temporary_directory directory(argv[0]);
        preparation_contract(directory);
        review_contract(directory);
        ambient_profile_contract(directory);
        const auto request = request_for(directory);
        serving_model_contract(request);
        completion_contract(request);
        failure_contract(request);
        framing_contract(request);
        delegation_contract(directory);
        std::puts("PASS OMP RPC preparation, settlement, framing and delegation contracts");
        return 0;
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
