#include "omp_provider.h"

#include <logonomic_cli_addon/adapter_runtime.h>

#include <exception>
#include <utility>

namespace {
namespace addon = logonomic::cli_addon;

void invoke(addon::Invocation_context& context)
{
    try {
        if (context.cancel_requested()) {
            context.fail(addon::Result_status::CANCELLED, "cancelled");
            return;
        }
        const auto& capability = context.request().capability.capability_id;
        if (capability == "launcher-discovery") {
            omp::handle_discovery(context);
        }
        else
        if (capability == "launch-preparation") {
            omp::handle_launch_preparation(context);
        }
        else {
            context.reject_unknown_operation();
        }
    }
    catch (const std::exception&) {
        context.fail(addon::Result_status::FAILED, "omp_operation_failed");
    }
}

} // namespace

int main()
{
    addon::Adapter_runtime_options options;
    options.adapter_name    = "omp-addon";
    options.adapter_version = "1.0.0";
    options.capabilities = {
        {"presentation", 1}, {"launcher-discovery", 1}, {"launch-preparation", 3},
    };
    addon::Adapter_runtime runtime(std::move(options));
    runtime.set_invoke_handler(invoke);
    return runtime.run();
}
