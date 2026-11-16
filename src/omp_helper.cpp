#include <logonomic_cli_addon/helper_client.h>
#include <logonomic_cli_addon/json_utilities.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <poll.h>
#include <unistd.h>
#endif

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

namespace {
namespace addon = logonomic::cli_addon;
namespace json = addon::json;

/// The native caller keeps its input pipe open until this helper exits. EOF
/// cancels the SDK-owned wrapper, which is killed and reaped by Helper_client.
class Input_watch
{
public:
    Input_watch() : m_deadline(std::chrono::steady_clock::now() + std::chrono::milliseconds(750)) {}

    bool cancelled() const
    {
        return std::chrono::steady_clock::now() >= m_deadline || pipe_closed();
    }

    std::optional<std::string> read_line() const
    {
        std::string result;
        while (!cancelled() && result.size() < 32u * 1024u) {
            if (!readable()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            char value = 0;
#ifdef _WIN32
            const auto count = _read(_fileno(stdin), &value, 1);
#else
            const auto count = ::read(STDIN_FILENO, &value, 1);
#endif
            if (count != 1) {
                return std::nullopt;
            }
            if (value == '\n') {
                return result;
            }
            result += value;
        }
        return std::nullopt;
    }

private:
    bool pipe_closed() const
    {
#ifdef _WIN32
        DWORD available = 0;
        return !PeekNamedPipe(GetStdHandle(STD_INPUT_HANDLE), nullptr, 0, nullptr, &available, nullptr);
#else
        pollfd input{STDIN_FILENO, POLLIN, 0};
        return ::poll(&input, 1, 0) < 0 || (input.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0;
#endif
    }

    bool readable() const
    {
#ifdef _WIN32
        DWORD available = 0;
        return PeekNamedPipe(GetStdHandle(STD_INPUT_HANDLE), nullptr, 0, nullptr, &available, nullptr) && available > 0;
#else
        pollfd input{STDIN_FILENO, POLLIN, 0};
        return ::poll(&input, 1, 0) > 0 && (input.revents & POLLIN) != 0;
#endif
    }

    std::chrono::steady_clock::time_point m_deadline;
};

bool write_result(const addon::Json_value& result)
{
    const auto output = addon::serialize_json(result) + '\n';
    return std::fwrite(output.data(), 1, output.size(), stdout) == output.size() && std::fflush(stdout) == 0;
}

int run()
{
    addon::Contract_failure failure;
    const auto environment = addon::read_helper_environment(&failure);
    if (!environment) {
        return 0;
    }
    const Input_watch input;
    const auto line = input.read_line();
    const auto request = line ? json::parse(*line) : std::nullopt;
    if (!request || !json::exact_keys(*request, {"operation", "name", "body"})) {
        return 1;
    }
    const auto& body = json::member(*request, "body");
    const auto name = json::text(*request, "name");
    const auto session_id = json::text(body, "session_id");
    const auto nonce = json::text(body, "activation_nonce");
    if (name.empty() || name.size() > 128 || !body.is_object() || session_id.empty() || session_id.size() > 256 ||
        !addon::is_valid_transition_nonce_hex(nonce)) {
        return 1;
    }
    addon::Helper_client client(*environment, [&] { return input.cancelled(); });
    if (json::text(*request, "operation") == "context") {
        if (name != "before_agent_start" || !json::exact_keys(body, {"session_id", "activation_nonce"})) {
            return 1;
        }
        addon::Model_context_delivery delivery;
        delivery.activation = addon::Model_context_activation{session_id, nonce};
        const auto outcome = client.request_title_instruction("omp", name, false, delivery);
        return write_result(json::object({
            {"committed", addon::Json_value(outcome.completed)},
            {"accepted", addon::Json_value(outcome.completed && outcome.accepted)},
            {"instruction", addon::Json_value(outcome.completed && outcome.accepted ? outcome.instruction : "")}})) ? 0 : 1;
    }
    if (json::text(*request, "operation") != "event") {
        return 1;
    }
    const auto key = addon::random_nonce_hex();
    auto outcome = client.deliver_provider_event(name, body, key, nonce);
    if (!outcome.committed && !input.cancelled() &&
        (outcome.exit_code == addon::k_helper_exit_transport_failure || outcome.exit_code == addon::k_helper_exit_pending)) {
        // An unknown transport outcome retains its exact journal identity.
        outcome = client.deliver_provider_event(name, body, key, nonce);
    }
    auto result = json::object({
        {"committed", addon::Json_value(outcome.committed)},
        {"accepted", addon::Json_value(outcome.committed && outcome.accepted)}});
    if (outcome.committed && outcome.accepted && outcome.helper_response) {
        for (const auto* name : {"child_state_version", "model_catalog_version"}) {
            const auto& version = json::member(*outcome.helper_response, name);
            if (version.is_integer() && version.as_integer() == 1) {
                result.set(name, addon::Json_value(1));
            }
        }
    }
    const bool written = write_result(result);
    if (written && outcome.committed) {
        (void)client.acknowledge_provider_event(key);
    }
    return written ? 0 : 1;
}

} // namespace

int main()
{
#ifdef _WIN32
    if (_setmode(_fileno(stdin), _O_BINARY) == -1 || _setmode(_fileno(stdout), _O_BINARY) == -1) {
        return 1;
    }
#endif
    try {
        return run();
    }
    catch (...) {
        // Native hooks remain noninteractive; authority and payloads never
        // enter CLI diagnostics when the authenticated bridge is unavailable.
        return 1;
    }
}
