#include "omp_conversation.h"

#include <logonomic_cli_addon/package_source.h>

#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace omp {
namespace addon = logonomic::cli_addon;

namespace {

using Json = addon::Json_value;
using Block = addon::Conversation_block;
using Kind = addon::Conversation_block_kind;
using Message = addon::Conversation_message;

bool reject(addon::Contract_failure* failure, std::string reason, std::string detail)
{
    if (failure) {
        *failure = {std::move(reason), std::move(detail)};
    }
    return false;
}

const std::string* text(const Json& value, std::string_view name)
{
    const auto* member = value.find(name);
    return member && member->is_string() ? &member->as_string() : nullptr;
}

bool flag(const Json& value, std::string_view name)
{
    const auto* member = value.find(name);
    return member && member->is_boolean() && member->as_boolean();
}

bool one_of(std::string_view value, std::initializer_list<std::string_view> values)
{
    return std::find(values.begin(), values.end(), value) != values.end();
}

bool append_content(Message& result, const Json& content, bool visible, Kind kind,
    std::string_view label, addon::Contract_failure* failure)
{
    if (content.is_string()) {
        result.content.push_back({kind, content.as_string(), std::string(label), visible});
        return true;
    }
    if (!content.is_array()) {
        return reject(failure, "conversation_content_unavailable", "Unsupported native message content.");
    }
    bool encrypted = false;
    for (const auto& block : content.as_array()) {
        const auto* type = text(block, "type");
        if (!type) {
            return reject(failure, "conversation_content_unavailable", "Native content has no type.");
        }
        if (*type == "text") {
            const auto* value = text(block, "text");
            if (!value) {
                return reject(failure, "conversation_content_unavailable", "Native text is not a string.");
            }
            result.content.push_back({kind, *value, std::string(label), visible});
        }
        else
        if (*type == "thinking") {
            const auto* value = text(block, "thinking");
            if (!value) {
                return reject(failure, "conversation_content_unavailable", "Native reasoning is not a string.");
            }
            const bool narration = flag(block, "narration");
            result.content.push_back({Kind::TEXT, *value, narration ? "" : "Reasoning", narration && visible});
        }
        else
        if (*type == "redactedThinking") {
            // The replay signature is opaque, not recoverable conversation text.
            // Retain one explicit placeholder per native message in both projections.
            if (!encrypted) {
                result.content.push_back({Kind::TEXT, "[Encrypted reasoning is not available as text.]", "", true});
                encrypted = true;
            }
        }
        else
        if (*type == "image") {
            const auto* mime = text(block, "mimeType");
            result.content.push_back({Kind::ATTACHMENT,
                "[Native image attachment; binary data is not copied.]", mime ? *mime : "image", visible});
        }
        else
        if (*type == "toolCall") {
            const auto* name = text(block, "name");
            const auto* id = text(block, "id");
            const auto* arguments = block.find("arguments");
            if (!name || !id || !arguments || !arguments->is_object()) {
                return reject(failure, "conversation_content_unavailable", "Malformed native tool call.");
            }
            result.content.push_back({Kind::TOOL_CALL, addon::serialize_json(*arguments), *name + " " + *id, false});
        }
        else
        if (*type == "anthropicServerTool") {
            const auto* native = block.find("block");
            const auto* native_type = native ? text(*native, "type") : nullptr;
            if (!native_type || !one_of(*native_type,
                    {"server_tool_use", "web_search_tool_result", "tool_search_tool_result"})) {
                return reject(failure, "conversation_content_unavailable", "Unsupported native server tool record.");
            }
            result.content.push_back({*native_type == "server_tool_use" ? Kind::TOOL_CALL : Kind::TOOL_RESULT,
                addon::serialize_json(*native), *native_type, false});
        }
        else
        if (*type != "fallback") {
            return reject(failure, "conversation_content_unavailable", "Unsupported native content block: " + *type);
        }
    }
    return true;
}

std::optional<Message> native_message(const Json& native, addon::Contract_failure* failure)
{
    const auto* role = text(native, "role");
    if (!role) {
        reject(failure, "conversation_content_unavailable", "Native message has no role.");
        return std::nullopt;
    }
    Message result;
    result.role = *role;
    if (one_of(*role, {"user", "developer", "assistant", "toolResult", "custom", "hookMessage"})) {
        const auto* content = native.find("content");
        const auto* stop = text(native, "stopReason");
        const bool tool_result = *role == "toolResult";
        const bool custom = *role == "custom" || *role == "hookMessage";
        const bool commentary = *role == "assistant" && stop && *stop == "toolUse";
        // AgentSession persists plan/checkpoint reminders as agent-attributed
        // developer messages. They are internal instructions, not discussion.
        const auto* attribution = text(native, "attribution");
        const bool internal_developer = *role == "developer" && attribution && *attribution == "agent";
        const bool visible = !tool_result && !commentary && !internal_developer && (!custom || flag(native, "display"));
        if (*role == "assistant" && stop && *stop == "stop") {
            result.phase = addon::Conversation_message_phase::FINAL;
        }
        else
        if (commentary) {
            result.phase = addon::Conversation_message_phase::COMMENTARY;
        }
        std::string label;
        if (tool_result) {
            const auto* name = text(native, "toolName");
            const auto* id = text(native, "toolCallId");
            if (!name || !id) {
                reject(failure, "conversation_content_unavailable", "Malformed native tool result.");
                return std::nullopt;
            }
            label = *name + " " + *id;
        }
        else
        if (custom) {
            if (const auto* custom_type = text(native, "customType")) {
                label = *custom_type;
            }
        }
        if (!content || !append_content(result, *content, visible,
                tool_result ? Kind::TOOL_RESULT : Kind::TEXT, label, failure)) {
            if (!content) {
                reject(failure, "conversation_content_unavailable", "Native message has no content.");
            }
            return std::nullopt;
        }
        if (const auto* error = text(native, "errorMessage")) {
            result.content.push_back({Kind::TEXT, *error, "Native error", false});
        }
    }
    else
    if (*role == "bashExecution" || *role == "pythonExecution") {
        const auto* code = text(native, *role == "bashExecution" ? "command" : "code");
        const auto* output = text(native, "output");
        if (!code || !output) {
            reject(failure, "conversation_content_unavailable", "Malformed native user tool execution.");
            return std::nullopt;
        }
        result.content.push_back({Kind::TOOL_CALL, *code, *role, false});
        result.content.push_back({Kind::TOOL_RESULT, *output, *role, false});
        if (const auto* images = native.find("images"); images &&
            !append_content(result, *images, false, Kind::ATTACHMENT, "", failure)) {
            return std::nullopt;
        }
    }
    else
    if (*role == "fileMention") {
        const auto* files = native.find("files");
        if (!files || !files->is_array()) {
            reject(failure, "conversation_content_unavailable", "Malformed native file mention.");
            return std::nullopt;
        }
        for (const auto& file : files->as_array()) {
            const auto* path = text(file, "path");
            const auto* content = text(file, "content");
            if (!path || !content) {
                reject(failure, "conversation_content_unavailable", "Malformed native file mention content.");
                return std::nullopt;
            }
            result.content.push_back({Kind::ATTACHMENT, "[Native file reference; no file is read during export.]", *path, true});
            result.content.push_back({Kind::TEXT, *content, *path, false});
            if (file.find("image")) {
                result.content.push_back({Kind::ATTACHMENT, "[Native image attachment; binary data is not copied.]", *path, true});
            }
        }
    }
    else
    if (*role == "branchSummary" || *role == "compactionSummary") {
        const auto* summary = text(native, "summary");
        if (!summary) {
            reject(failure, "conversation_content_unavailable", "Malformed native summary.");
            return std::nullopt;
        }
        result.content.push_back({Kind::TEXT, *summary, *role, false});
    }
    else {
        reject(failure, "conversation_content_unavailable", "Unsupported native message role: " + *role);
        return std::nullopt;
    }
    return result;
}

struct Entry
{
    std::string parent;
    std::string type;
    Json        payload;
};

} // namespace

std::optional<addon::Conversation_export_result> export_conversation(
    const addon::Conversation_export_request& request, const std::function<bool()>& cancelled,
    addon::Contract_failure* failure)
{
    const auto* identity = request.resume_record ? text(*request.resume_record, "provider_session_id") : nullptr;
    const auto* source = request.resume_record ? text(*request.resume_record, "session_file") : nullptr;
    if (!identity || *identity != request.provider_session_id || !source) {
        reject(failure, "conversation_source_unavailable", "Export needs this session's captured native file and identity.");
        return std::nullopt;
    }
    const auto path = addon::path_from_utf8(*source);
    // Native SessionManager.open retains an exact custom file path without a
    // suffix requirement. The captured identity and parsed v3 header/graph,
    // rather than a filename convention, establish the export source.
    if (!path.is_absolute()) {
        reject(failure, "conversation_format_unavailable", "Export supports absolute native JSONL session files.");
        return std::nullopt;
    }
    std::map<std::string, Entry, std::less<>> entries;
    std::string leaf;
    std::string fallback_leaf;
    bool header = false;
    bool title_slot = false;
    const auto snapshot = addon::read_conversation_jsonl(path, [&](const Json& record, addon::Contract_failure* parse_failure) {
        const auto* type = text(record, "type");
        if (!type) {
            return reject(parse_failure, "conversation_format_unavailable", "Native record has no type.");
        }
        if (!header && !title_slot && *type == "title") {
            title_slot = true;
            return true;
        }
        if (!header) {
            const auto* version = record.find("version");
            const auto* id = text(record, "id");
            if (*type != "session" || !version || !version->is_integer() || version->as_integer() != 3 || !id) {
                return reject(parse_failure, "conversation_format_unavailable", "Expected a native version 3 session header.");
            }
            if (*id != request.provider_session_id) {
                return reject(parse_failure, "conversation_identity_mismatch", "The native file belongs to a different session.");
            }
            header = true;
            return true;
        }
        const auto* id = text(record, "id");
        const auto* parent = record.find("parentId");
        const auto* active_leaf = record.find("activeLeafId");
        if (!id || id->empty() || !parent || (!parent->is_null() && !parent->is_string()) ||
            (active_leaf && !active_leaf->is_null() && !active_leaf->is_string())) {
            return reject(parse_failure, "conversation_branch_unavailable", "Malformed native session graph entry.");
        }
        if (!one_of(*type, {"message", "model_usage", "thinking_level_change", "model_change", "service_tier_change",
                "compaction", "branch_summary", "custom", "custom_message", "label", "title_change", "ttsr_injection",
                "session_init", "mode_change", "credential_pin", "reset_boundary"})) {
            return reject(parse_failure, "conversation_format_unavailable", "Unsupported native session entry: " + *type);
        }
        Entry entry{parent->is_string() ? parent->as_string() : "", *type, {}};
        if (one_of(*type, {"message", "custom_message", "compaction", "branch_summary", "session_init"})) {
            entry.payload = record;
        }
        // Match SessionEntryIndex: a duplicate id replaces its indexed value;
        // late entries use their captured activeLeafId, including explicit null.
        entries.insert_or_assign(*id, std::move(entry));
        leaf = active_leaf ? (active_leaf->is_string() ? active_leaf->as_string() : "") : *id;
        if (leaf == *id) {
            fallback_leaf = *id;
        }
        return true;
    }, cancelled, failure);
    if (!snapshot) {
        return std::nullopt;
    }
    if (!header) {
        reject(failure, "conversation_empty", "No complete native session header was available.");
        return std::nullopt;
    }
    if (!leaf.empty() && !entries.contains(leaf)) {
        leaf = fallback_leaf;
    }
    std::vector<const Entry*> branch;
    std::set<std::string> visited;
    while (!leaf.empty()) {
        if (cancelled && cancelled()) {
            reject(failure, "cancelled", "Conversation export was cancelled.");
            return std::nullopt;
        }
        const auto current = entries.find(leaf);
        if (current == entries.end() || !visited.insert(leaf).second) {
            reject(failure, "conversation_branch_unavailable", "The selected native branch is incomplete or cyclic.");
            return std::nullopt;
        }
        branch.push_back(&current->second);
        leaf = current->second.parent;
    }
    addon::Conversation_writer writer(request, cancelled);
    if (!writer.open(failure)) {
        return std::nullopt;
    }
    bool retained_message = false;
    for (auto current = branch.rbegin(); current != branch.rend(); ++current) {
        const auto& entry = **current;
        std::optional<Message> message;
        if (entry.type == "message") {
            const auto* native = entry.payload.find("message");
            if (!native) {
                reject(failure, "conversation_content_unavailable", "Native message entry has no message.");
                return std::nullopt;
            }
            message = native_message(*native, failure);
        }
        else
        if (entry.type == "custom_message") {
            auto native = entry.payload;
            native.set("role", Json("custom"));
            message = native_message(native, failure);
        }
        else
        if (entry.type == "compaction" || entry.type == "branch_summary") {
            const auto* summary = text(entry.payload, "summary");
            if (summary) {
                message = Message{entry.type, {{Kind::TEXT, *summary, "Native summary", false}}};
            }
            else {
                reject(failure, "conversation_content_unavailable", "Native summary has no text.");
            }
        }
        else
        if (entry.type == "session_init") {
            // Native task/tools metadata does not add another participant message.
            // The initial task is recorded separately as an ordinary user message.
            const auto* prompt = entry.payload.find("systemPrompt");
            if (!prompt) {
                continue;
            }
            message = Message{"system", {}};
            if (prompt->is_string()) {
                message->content.push_back({Kind::TEXT, prompt->as_string(), "Native system prompt", false});
            }
            else
            if (prompt->is_array() && std::all_of(prompt->as_array().begin(), prompt->as_array().end(),
                    [](const Json& part) { return part.is_string(); })) {
                for (const auto& part : prompt->as_array()) {
                    message->content.push_back({Kind::TEXT, part.as_string(), "Native system prompt", false});
                }
            }
            else {
                reject(failure, "conversation_content_unavailable", "Unsupported native system prompt.");
                return std::nullopt;
            }
        }
        else
        if (entry.type == "reset_boundary") {
            message = Message{"system", {{Kind::TEXT, "[Native conversation context was cleared.]", "", false}}};
        }
        else {
            continue;
        }
        if (!message) {
            return std::nullopt;
        }
        if (message->content.empty()) {
            continue;
        }
        if (!writer.append(*message, failure)) {
            return std::nullopt;
        }
        retained_message = retained_message || std::any_of(message->content.begin(), message->content.end(),
            [&](const Block& block) { return !request.compact || block.compact_visible; });
    }
    if (!retained_message) {
        reject(failure, "conversation_empty", "The selected native branch has no exportable conversation messages.");
        return std::nullopt;
    }
    return writer.finish(snapshot->incomplete_tail_omitted, failure);
}

} // namespace omp
