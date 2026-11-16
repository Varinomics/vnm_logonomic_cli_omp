// Opt-in native acceptance driver. The stock simulator intentionally redacts
// wake bindings in its printable transcript; this uses its typed outcome API
// to apply the actual package response in the private test process instead.
#include <logonomic_cli_addon/host_simulator.h>
#include <logonomic_cli_addon/json_utilities.h>

#include <cstdio>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <string>

namespace addon = logonomic::cli_addon;
namespace json = addon::json;

int main(int argc, char** argv)
{
    try {
        if (argc != 2) {
            return 2;
        }
        std::string input;
        char byte = 0;
        while (std::cin.get(byte)) {
            if (input.size() == 1024 * 1024) {
                return 2;
            }
            input.push_back(byte);
        }
        const auto request = json::parse(input);
        if (!request || !request->is_object()) {
            return 2;
        }
        const auto* version = request->find("version");
        if (!version || !version->is_integer() || version->as_integer() < 1 || version->as_integer() > 65535) {
            return 2;
        }
        addon::Simulator_options options;
        options.package_directory = addon::path_from_utf8(argv[1]);
        options.validation.platform_id = "linux-x86_64";
        addon::Transcript_recorder recorder;
        addon::Host_simulator simulator(std::move(options), &recorder);
        addon::Contract_failure failure;
        if (!simulator.start(&failure)) {
            std::fprintf(stderr, "Native fixture package start failed: %s\n", failure.reason_code.c_str());
            return 1;
        }
        const auto id = simulator.invoke(
            {json::text(*request, "capability_id"), static_cast<std::uint16_t>(version->as_integer())},
            json::text(*request, "operation"), json::member(*request, "payload"), 5000);
        const auto result = simulator.wait_outcome(id, 8000);
        int exit_code = -1;
        if (!result || !simulator.shutdown(5000, &exit_code) || exit_code != 0) {
            return 1;
        }
        const auto output = json::object({
            {"status", addon::Json_value(addon::host_status_name(result->status))},
            {"reason_code", addon::Json_value(result->reason_code)},
            {"payload", result->payload}});
        std::puts(addon::serialize_json(output).c_str());
        return 0;
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "Native fixture host failed: %s\n", error.what());
        return 1;
    }
}
