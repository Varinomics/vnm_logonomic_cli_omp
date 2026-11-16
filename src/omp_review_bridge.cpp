// The host substitutes the managed asset in an environment value. Process
// argv is literal, so this attached bridge supplies that path to OMP itself.
// Custody and stream inheritance follow the SDK's native launch bridge.
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
namespace {

std::wstring quote(const std::wstring& argument)
{
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++slashes;
            continue;
        }
        result.append(character == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        result += character;
    }
    result.append(slashes * 2, L'\\');
    return result + L"\"";
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    const wchar_t* asset = _wgetenv(L"OMP_LOGONOMIC_REVIEW_ASSET");
    std::error_code error;
    if (argc < 2 || !asset || !std::filesystem::path(asset).is_absolute() ||
        !std::filesystem::is_regular_file(asset, error) || error)
    {
        return 2;
    }
    std::vector<std::wstring> arguments(argv + 1, argv + argc);
    arguments.insert(arguments.end(), {L"--trusted-extension", asset, L"--tools", L"read,grep,glob",
        L"--no-session", L"--no-lsp", L"--no-judge", L"--no-spotlight", L"--no-pty"});
    std::wstring command;
    for (const auto& argument : arguments) {
        if (!command.empty()) command += L' ';
        command += quote(argument);
    }
    HANDLE inherited[3]{};
    const DWORD streams[3] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
    for (int index = 0; index < 3; ++index) {
        if (!DuplicateHandle(GetCurrentProcess(), GetStdHandle(streams[index]), GetCurrentProcess(),
                &inherited[index], 0, TRUE, DUPLICATE_SAME_ACCESS))
        {
            for (int previous = 0; previous < index; ++previous) CloseHandle(inherited[previous]);
            return 3;
        }
    }
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<unsigned char> storage(bytes);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    const bool initialized = InitializeProcThreadAttributeList(attributes, 1, 0, &bytes) != FALSE;
    PROCESS_INFORMATION process{};
    bool started = false;
    if (initialized && UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inherited, sizeof(inherited), nullptr, nullptr))
    {
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb          = sizeof(startup);
        startup.StartupInfo.dwFlags     = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput   = inherited[0];
        startup.StartupInfo.hStdOutput  = inherited[1];
        startup.StartupInfo.hStdError   = inherited[2];
        startup.lpAttributeList        = attributes;
        const DWORD console = GetConsoleCP() != 0 ? 0 : CREATE_NO_WINDOW;
        started = CreateProcessW(argv[1], command.data(), nullptr, nullptr, TRUE,
            EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | console,
            nullptr, nullptr, &startup.StartupInfo, &process) != FALSE;
    }
    if (initialized) DeleteProcThreadAttributeList(attributes);
    for (const HANDLE handle : inherited) CloseHandle(handle);
    if (!started) return 3;
    CloseHandle(process.hThread);
    DWORD exit_code = 3;
    if (WaitForSingleObject(process.hProcess, INFINITE) == WAIT_OBJECT_0) {
        GetExitCodeProcess(process.hProcess, &exit_code);
    }
    else {
        TerminateProcess(process.hProcess, 3);
        WaitForSingleObject(process.hProcess, INFINITE);
    }
    CloseHandle(process.hProcess);
    ExitProcess(exit_code);
}
#else
int main(int argc, char** argv)
{
    const char* asset = std::getenv("OMP_LOGONOMIC_REVIEW_ASSET");
    std::error_code error;
    if (argc < 2 || !asset || !std::filesystem::path(asset).is_absolute() ||
        !std::filesystem::is_regular_file(asset, error) || error)
    {
        return 2;
    }
    std::vector<std::string> arguments(argv + 1, argv + argc);
    arguments.insert(arguments.end(), {"--trusted-extension", asset, "--tools", "read,grep,glob",
        "--no-session", "--no-lsp", "--no-judge", "--no-spotlight", "--no-pty"});
    std::vector<char*> native;
    for (auto& argument : arguments) native.push_back(argument.data());
    native.push_back(nullptr);
    execv(argv[1], native.data());
    return 3;
}
#endif
