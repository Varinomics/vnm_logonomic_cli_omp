#include "omp_conversation.h"

#include <logonomic_cli_addon/limits.h>
#include <logonomic_cli_addon/package_source.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace {
namespace addon = logonomic::cli_addon;

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string read(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}

class Fixture
{
public:
    Fixture()
    {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        directory = std::filesystem::temp_directory_path() / ("omp-export-" + std::to_string(nonce));
        std::filesystem::create_directory(directory);
        directory = std::filesystem::canonical(directory);
        source = directory / addon::path_from_utf8("conversation-é.jsonl");
    }

    ~Fixture()
    {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }

    void write(std::string_view body)
    {
        std::ofstream stream(source, std::ios::binary);
        stream << body;
    }

    addon::Conversation_export_request request(std::string_view output = "export")
    {
        auto path = directory / output;
        std::filesystem::create_directory(path);
        addon::Conversation_export_request request;
        request.provider_session_id = "session-exact";
        request.launch_environment.platform_id = "linux-x86_64";
        request.output_directory = addon::path_to_utf8(path);
        request.resume_record = addon::Json_value::object();
        request.resume_record->set("provider_session_id", addon::Json_value(request.provider_session_id));
        request.resume_record->set("session_file", addon::Json_value(addon::path_to_utf8(source)));
        return request;
    }

    std::filesystem::path directory;
    std::filesystem::path source;
};

constexpr std::string_view header = "{\"type\":\"session\",\"version\":3,\"id\":\"session-exact\",\"cwd\":\"/workspace\"}\n";

void branch_selection()
{
    // Oracles: native session-entries.ts resolveSessionLeafId and
    // SessionEntryIndex.#walk. The physically last usage record is off-branch;
    // /clear resets model context while retaining earlier transcript history.
    Fixture fixture;
    const std::string body = std::string(header) + R"({"type":"message","id":"u","parentId":null,"message":{"role":"user","content":"original prompt"}}
{"type":"message","id":"old","parentId":"u","message":{"role":"assistant","stopReason":"stop","content":[{"type":"text","text":"abandoned answer"}]}}
{"type":"message","id":"a","parentId":"u","message":{"role":"assistant","stopReason":"stop","content":[{"type":"text","text":"selected answer"}]}}
{"type":"reset_boundary","id":"reset","parentId":"a"}
{"type":"message","id":"u2","parentId":"reset","message":{"role":"user","content":"after clear"}}
{"type":"model_usage","id":"late","parentId":"old","activeLeafId":"u2","purpose":"title","usage":{"input":10}}
)";
    fixture.write(body);
    auto request = fixture.request();
    addon::Contract_failure failure;
    const auto result = omp::export_conversation(request, {}, &failure);
    require(result && result->message_count == 4, "Selected native branch must include its reset and earlier discussion");
    const auto exported = read(fixture.directory / "export" / result->file_name);
    require(exported.find("original prompt") < exported.find("selected answer") &&
        exported.find("selected answer") < exported.find("after clear") &&
        exported.find("abandoned answer") == std::string::npos && exported.find("title") == std::string::npos,
        "A late auxiliary record must not select the abandoned branch or appear as a participant");
    require(read(fixture.source) == body, "Export cannot mutate native history");

    fixture.write(std::string(header) + R"({"type":"message","id":"a","parentId":null,"message":{"role":"user","content":"stale duplicate"}}
{"type":"message","id":"a","parentId":null,"message":{"role":"user","content":"current duplicate"}}
{"type":"model_usage","id":"late","parentId":null,"activeLeafId":"removed-target"}
)");
    request = fixture.request("duplicate");
    const auto duplicate = omp::export_conversation(request, {}, &failure);
    require(duplicate && duplicate->message_count == 1, "Native duplicate ids replace indexed entries without duplicating output");
    const auto replaced = read(fixture.directory / "duplicate" / duplicate->file_name);
    require(replaced.find("current duplicate") != std::string::npos && replaced.find("stale duplicate") == std::string::npos,
        "A dangling late activeLeafId must recover the native last self-selected leaf");
}

void content_and_projection()
{
    // Native types.ts and tui/chat/messages.ts define the exported participant,
    // tool, reasoning, attachment and user execution records below.
    Fixture fixture;
    const std::string body = R"({"type":"title","v":1,"title":"Title slot","pad":"padding"}
)" + std::string(header) + R"({"type":"session_init","id":"init","parentId":null,"systemPrompt":["native policy"],"task":"not another user message"}
{"type":"message","id":"u","parentId":"init","message":{"role":"user","content":[{"type":"text","text":"café 東京\n```cpp\nint x;\n```"},{"type":"image","mimeType":"image/png","data":"NEVER-COPY-IMAGE-BYTES"}]}}
{"type":"message","id":"t","parentId":"u","message":{"role":"assistant","stopReason":"toolUse","content":[{"type":"text","text":"intermediate update"},{"type":"thinking","thinking":"private visible reasoning"},{"type":"toolCall","name":"read","id":"call","arguments":{"path":"file.txt"}}]}}
{"type":"message","id":"r","parentId":"t","message":{"role":"toolResult","toolName":"read","toolCallId":"call","content":[{"type":"text","text":"tool output ```\n\u001b[31m"}]}}
{"type":"message","id":"b","parentId":"r","message":{"role":"bashExecution","command":"echo local","output":"local output"}}
{"type":"custom_message","id":"h","parentId":"b","customType":"hidden-context","content":"internal context","display":false}
{"type":"message","id":"a","parentId":"h","message":{"role":"assistant","stopReason":"stop","content":[{"type":"redactedThinking","data":"NEVER-COPY-ENCRYPTED"},{"type":"redactedThinking","data":"NEVER-COPY-ENCRYPTED-2"},{"type":"text","text":"final answer"}]}}
{"type":"compaction","id":"c","parentId":"a","summary":"native compaction summary","firstKeptEntryId":"a"}
)";
    fixture.write(body + "{\"incomplete");
    addon::Contract_failure failure;
    for (const auto format : {addon::Conversation_export_format::TEXT, addon::Conversation_export_format::MARKDOWN,
             addon::Conversation_export_format::CONVERSATION}) {
        for (const bool compact : {false, true}) {
            const auto output = std::string(addon::conversation_export_format_name(format)) + (compact ? "-compact" : "-full");
            auto request = fixture.request(output);
            request.format = format;
            request.compact = compact;
            request.profile_storage = addon::Profile_storage_context{1, addon::path_to_utf8(fixture.directory)};
            request.account_context = addon::Account_context{2, "source-account", addon::path_to_utf8(fixture.directory)};
            const auto result = omp::export_conversation(request, {}, &failure);
            require(result && result->incomplete_tail_omitted && result->attachment_count == 1 && result->compact == compact,
                "All SDK formats must report the frozen tail and exact projection");
            require(addon::decode_conversation_export_result(addon::encode_conversation_export_result(*result), &failure).has_value(),
                "Export descriptors must satisfy the actual SDK wire contract");
            require(addon::encode_conversation_export_result(*result).find("account_context") &&
                addon::encode_conversation_export_result(*result).find("profile_storage"),
                "Historical account and profile context must remain exactly echoed by the shared writer");
            const auto bytes = read(fixture.directory / output / result->file_name);
            require(bytes.size() == result->bytes && bytes.find("café 東京") != std::string::npos &&
                bytes.find("final answer") != std::string::npos && bytes.find("Encrypted reasoning") != std::string::npos &&
                bytes.find("NEVER-COPY") == std::string::npos && bytes.find("not another user message") == std::string::npos,
                "Exports preserve participant text and opaque placeholders without copying private replay/binary payloads");
            require((bytes.find("private visible reasoning") != std::string::npos) == !compact &&
                (bytes.find("intermediate update") != std::string::npos) == !compact &&
                (bytes.find("tool output") != std::string::npos) == !compact &&
                (bytes.find("native policy") != std::string::npos) == !compact &&
                (bytes.find("native compaction summary") != std::string::npos) == !compact,
                "Compact projection removes only known reasoning, tools, internal records and commentary");
            require(result->message_count == (compact ? 2 : 8), "Message counts must describe retained source messages");
        }
    }
}

void rejection_and_cancellation()
{
    Fixture fixture;
    addon::Contract_failure failure;
    const auto rejected = [&](const std::string& body, const char* expected) {
        fixture.write(body);
        auto request = fixture.request();
        const auto result = omp::export_conversation(request, {}, &failure);
        require(!result && failure.reason_code == expected, "Malformed/foreign native sources must fail explicitly");
        require(std::filesystem::is_empty(fixture.directory / "export"), "A failed export must leave no partial document");
    };
    rejected("{\"type\":\"session\",\"version\":3,\"id\":\"foreign\"}\n", "conversation_identity_mismatch");
    rejected("{\"type\":\"session\",\"version\":2,\"id\":\"session-exact\"}\n", "conversation_format_unavailable");
    rejected(std::string(header) + "{\"type\":\"message\",\"id\":\"a\",\"parentId\":\"missing\"}\n", "conversation_branch_unavailable");
    rejected(std::string(header) + "{\"type\":\"message\",\"id\":\"a\",\"parentId\":\"a\"}\n", "conversation_branch_unavailable");
    rejected(std::string(header) + "{\"type\":\"message\",\"id\":\"a\",\"parentId\":null,\"message\":{\"role\":\"future-role\"}}\n",
        "conversation_content_unavailable");
    rejected(std::string(header) + "{\"type\":\"model_usage\",\"id\":\"a\",\"parentId\":null,\"activeLeafId\":null}\n", "conversation_empty");
    fixture.write(std::string(header) + "{\"type\":\"message\",\"id\":\"a\",\"parentId\":null,\"message\":{\"role\":\"user\",\"content\":\"valid\"}}\n");
    auto request = fixture.request();
    require(!omp::export_conversation(request, [] { return true; }, &failure) && failure.reason_code == "cancelled",
        "Cancellation must not publish a partial document");
    request.resume_record.reset();
    require(!omp::export_conversation(request, {}, &failure) && failure.reason_code == "conversation_source_unavailable",
        "Export cannot guess a session file when no captured locator exists");
}

void native_record_size()
{
    Fixture fixture;
    const std::string large(addon::limits::k_max_json_string_bytes + 100, 'x');
    fixture.write(std::string(header) + "{\"type\":\"message\",\"id\":\"a\",\"parentId\":null,\"message\":{\"role\":\"user\",\"content\":\"" + large + "\"}}\n");
    auto request = fixture.request();
    addon::Contract_failure failure;
    const auto result = omp::export_conversation(request, {}, &failure);
    require(result && read(fixture.directory / "export" / result->file_name).find(large) != std::string::npos,
        "Native history must not inherit the capability protocol's small string bound");
}

} // namespace

int main()
{
    try {
        branch_selection();
        content_and_projection();
        rejection_and_cancellation();
        native_record_size();
        return 0;
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
