#include "omp_provider.h"
#include "omp_wake.h"

#include <logonomic_cli_addon/json_utilities.h>

#include <chrono>
#include <cstdio>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>

namespace {
namespace addon = logonomic::cli_addon;
namespace json = addon::json;

class Invocation_fixture : public addon::Invocation_context
{
public:
    explicit Invocation_fixture(const addon::Json_value& value)
    {
        m_request.operation = json::text(value, "operation");
        m_request.capability = m_request.operation == "prepare" ? addon::Capability_pair{"launch-preparation", 3} :
            addon::Capability_pair{"session-wake", 1};
        m_request.payload = json::member(value, "payload");
        if (const auto* delay = value.find("cancel_after_ms"); delay && delay->is_integer()) {
            m_cancel_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(delay->as_integer());
        }
    }

    const addon::Invoke_request& request() const override { return m_request; }
    bool cancel_requested() const override
    {
        return m_cancel_at && std::chrono::steady_clock::now() >= *m_cancel_at;
    }
    bool wait_for_cancel(int) override { return cancel_requested(); }
    void report_progress(std::uint64_t, std::optional<std::uint64_t>, std::string) override {}
    void succeed(addon::Json_value payload) override
    {
        m_result = json::object({{"status", addon::Json_value("success")}, {"payload", std::move(payload)}});
    }
    void fail(addon::Result_status, std::string reason, std::string) override
    {
        m_result = json::object({{"status", addon::Json_value("failure")}, {"reason", addon::Json_value(std::move(reason))}});
    }
    bool finished() const override { return m_result.has_value(); }
    const addon::Json_value& result() const { return m_result.value(); }

private:
    addon::Invoke_request m_request;
    std::optional<addon::Json_value> m_result;
    std::optional<std::chrono::steady_clock::time_point> m_cancel_at;
};

} // namespace

int main()
{
    try {
        const std::string input{std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>()};
        const auto value = input.size() <= 128 * 1024 ? json::parse(input) : std::nullopt;
        if (!value) {
            return 2;
        }
        Invocation_fixture context(*value);
        if (context.request().operation == "prepare") {
            omp::handle_launch_preparation(context);
        }
        else {
            omp::handle_session_wake(context);
        }
        std::puts(addon::serialize_json(context.result()).c_str());
        return 0;
    }
    catch (const std::exception&) {
        return 1;
    }
}
