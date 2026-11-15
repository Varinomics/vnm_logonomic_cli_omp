#include "omp_provider.h"

#include <logonomic_cli_addon/launcher_utilities.h>

#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace addon = logonomic::cli_addon;

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void recognition()
{
    const auto direct = omp::recognize_launcher({"omp --model anthropic/claude-sonnet-4", "direct", ""},
        "linux-x86_64");
    require(direct && direct->program == "omp", "The native omp executable must be recognized");
    require(direct->arguments == std::vector<std::string>{"--model", "anthropic/claude-sonnet-4"},
        "Native options must retain their values");
    require(!omp::recognize_launcher({"pi", "direct", ""}, "linux-x86_64"),
        "The distinct Pi CLI must not be attributed to oh-my-pi");
    require(!omp::recognize_launcher({"omp && echo done", "bash", ""}, "linux-x86_64"),
        "A compound shell script must not be replaced as one literal invocation");
    require(!omp::recognize_launcher({"OMP", "direct", ""}, "linux-x86_64"),
        "POSIX executable recognition is case-sensitive");
    require(omp::recognize_launcher({"OMP.EXE", "direct", ""}, "windows-x86_64").has_value(),
        "Windows executable recognition is case-insensitive");
    require(omp::recognize_launcher({"omp.cmd --model fast", "cmd", ""}, "windows-x86_64").has_value(),
        "The npm command launcher must retain its Windows identity");
    require(omp::recognize_launcher({"& 'C:\\Tools\\omp.exe'", "powershell", ""}, "windows-x86_64").has_value(),
        "Literal PowerShell launcher paths must be recognized");

    const addon::Launcher_request launcher{"omp --model fast", "direct", ""};
    const std::vector<std::string> matching{"omp", "--model", "fast"};
    const std::vector<std::string> different{"omp", "--model", "other"};
    require(omp::recognize_launcher(launcher, "linux-x86_64", matching).has_value(),
        "Matching frozen host arguments must be accepted");
    require(!omp::recognize_launcher(launcher, "linux-x86_64", different),
        "Replacement preparation must reject mismatched frozen host arguments");

    const auto assigned = omp::recognize_launcher({"PI_CODING_AGENT_DIR='/tmp/omp profile' omp", "bash", ""},
        "linux-x86_64");
    require(assigned && assigned->environment_names == std::vector<std::string>{"PI_CODING_AGENT_DIR"},
        "Shell assignments must be obtained from the frozen environment instead of re-evaluated");
}

} // namespace

int main()
{
    try {
        recognition();
        std::puts("PASS omp provider launcher contracts");
        return 0;
    }
    catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
