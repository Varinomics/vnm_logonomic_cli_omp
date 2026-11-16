#include "omp_wake.h"

#include <logonomic_cli_addon/json_utilities.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/session_wake.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace omp {
namespace addon = logonomic::cli_addon;
namespace json = addon::json;
namespace fs = std::filesystem;
namespace {

constexpr const char* registration_environment = "VNM_OMP_WAKE_REGISTRATION";
constexpr const char* wake_asset = "assets/native_wake.ts";

bool private_object(const fs::path& path, bool directory)
{
#ifdef _WIN32
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
        bool(attributes & FILE_ATTRIBUTE_DIRECTORY) == directory;
#else
    struct stat status {};
    return ::lstat(path.c_str(), &status) == 0 && status.st_uid == ::geteuid() &&
        (status.st_mode & 0077) == 0 && (directory ? S_ISDIR(status.st_mode) : S_ISREG(status.st_mode));
#endif
}

bool make_private_directory(const fs::path& path)
{
#ifdef _WIN32
    // The host's profile-storage root owns the inherited user ACL.
    return ::CreateDirectoryW(path.c_str(), nullptr) && private_object(path, true);
#else
    return ::mkdir(path.c_str(), 0700) == 0 && private_object(path, true);
#endif
}

std::optional<addon::Json_value> read_private_json(const fs::path& path)
{
    if (!private_object(path, false)) {
        return std::nullopt;
    }
    std::error_code error;
    const auto size = fs::file_size(path, error);
    if (error || size == 0 || size > 2048) {
        return std::nullopt;
    }
    std::ifstream input(path, std::ios::binary);
    std::string data(static_cast<std::size_t>(size), '\0');
    if (!input.read(data.data(), static_cast<std::streamsize>(data.size())) || input.peek() != EOF) {
        return std::nullopt;
    }
    return json::parse(data);
}

struct Registration
{
    std::string id;
    std::string token;
    std::string instance;
    std::uint16_t port = 0;
};

std::optional<Registration> registration(const addon::Json_value& binding)
{
    if (!json::exact_keys(binding, {"registration_id", "registration_directory"}) ||
        !addon::is_valid_generation_nonce_hex(json::text(binding, "registration_id")) ||
        !json::bounded_string(json::member(binding, "registration_directory"), 4096)) {
        return std::nullopt;
    }
    const auto id = json::text(binding, "registration_id");
    const auto directory = addon::path_from_utf8(json::text(binding, "registration_directory"));
    std::error_code error;
    if (!directory.is_absolute() || directory.filename() != "wake-" + id ||
        fs::canonical(directory, error) != directory || error || !private_object(directory, true)) {
        return std::nullopt;
    }
    const auto secret = read_private_json(directory / "registration.json");
    const auto endpoint = read_private_json(directory / "endpoint.json");
    if (!secret || !endpoint ||
        !json::exact_keys(*secret, {"version", "registration_id", "token"}) ||
        !json::exact_keys(*endpoint, {"version", "registration_id", "instance_id", "port"}) ||
        !json::member(*secret, "version").is_integer() || json::member(*secret, "version").as_integer() != 1 ||
        !json::member(*endpoint, "version").is_integer() || json::member(*endpoint, "version").as_integer() != 1 ||
        json::text(*secret, "registration_id") != id || json::text(*endpoint, "registration_id") != id ||
        !addon::is_valid_sha256_hex(json::text(*secret, "token")) ||
        !addon::is_valid_generation_nonce_hex(json::text(*endpoint, "instance_id"))) {
        return std::nullopt;
    }
    const auto& port = json::member(*endpoint, "port");
    if (!port.is_integer() || port.as_integer() < 1 || port.as_integer() > 65535) {
        return std::nullopt;
    }
    return Registration{id, json::text(*secret, "token"), json::text(*endpoint, "instance_id"),
        static_cast<std::uint16_t>(port.as_integer())};
}

#ifdef _WIN32
using Socket_handle = SOCKET;
constexpr Socket_handle invalid_socket = INVALID_SOCKET;
bool would_block() { return ::WSAGetLastError() == WSAEWOULDBLOCK; }
#else
using Socket_handle = int;
constexpr Socket_handle invalid_socket = -1;
bool would_block() { return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR; }
#endif

class Socket
{
public:
    Socket()
    {
#ifdef _WIN32
        WSADATA data {};
        m_started = ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
        if (!m_started) {
            return;
        }
#endif
        m_handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (m_handle == invalid_socket) {
            return;
        }
#ifdef _WIN32
        u_long enabled = 1;
        if (::ioctlsocket(m_handle, FIONBIO, &enabled) != 0) {
            close();
        }
#else
        const int flags = ::fcntl(m_handle, F_GETFL, 0);
        if (flags < 0 || ::fcntl(m_handle, F_SETFL, flags | O_NONBLOCK) < 0) {
            close();
        }
#ifdef SO_NOSIGPIPE
        const int enabled = 1;
        ::setsockopt(m_handle, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
#endif
    }
    ~Socket()
    {
        close();
#ifdef _WIN32
        if (m_started) {
            ::WSACleanup();
        }
#endif
    }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket_handle get() const { return m_handle; }

private:
    void close()
    {
        if (m_handle != invalid_socket) {
#ifdef _WIN32
            ::closesocket(m_handle);
#else
            ::close(m_handle);
#endif
            m_handle = invalid_socket;
        }
    }
    Socket_handle m_handle = invalid_socket;
#ifdef _WIN32
    bool m_started = false;
#endif
};

using Clock = std::chrono::steady_clock;

bool wait_socket(Socket_handle socket, bool writing, addon::Invocation_context& context, Clock::time_point deadline)
{
#ifndef _WIN32
    if (socket >= FD_SETSIZE) {
        return false;
    }
#endif
    while (!context.cancel_requested() && Clock::now() < deadline) {
        fd_set ready;
        FD_ZERO(&ready);
        FD_SET(socket, &ready);
        timeval timeout{0, 20000};
#ifdef _WIN32
        const int selected = ::select(0, writing ? nullptr : &ready, writing ? &ready : nullptr, nullptr, &timeout);
#else
        const int selected = ::select(socket + 1, writing ? nullptr : &ready, writing ? &ready : nullptr, nullptr, &timeout);
#endif
        if (selected > 0) {
            return true;
        }
        if (selected < 0 && !would_block()) {
            return false;
        }
    }
    return false;
}

bool connect_socket(Socket_handle socket, std::uint16_t port, addon::Invocation_context& context, Clock::time_point deadline)
{
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) {
        return true;
    }
#ifdef _WIN32
    if (!would_block()) {
        return false;
    }
    int length = sizeof(int);
#else
    if (errno != EINPROGRESS) {
        return false;
    }
    socklen_t length = sizeof(int);
#endif
    int error = 0;
    return wait_socket(socket, true, context, deadline) &&
        ::getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length) == 0 && error == 0;
}

struct Exchange
{
    std::optional<addon::Json_value> reply;
    bool dispatched = false;
};

Exchange exchange(const Registration& registration, const addon::Json_value& request, addon::Invocation_context& context)
{
    Exchange result;
    const auto deadline = Clock::now() + std::chrono::milliseconds(1000);
    const Socket socket;
    if (context.cancel_requested() || socket.get() == invalid_socket ||
        !connect_socket(socket.get(), registration.port, context, deadline)) {
        return result;
    }
    const auto bytes = addon::serialize_json(request) + "\n";
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        if (!wait_socket(socket.get(), true, context, deadline)) {
            return result;
        }
#ifdef MSG_NOSIGNAL
        constexpr int flags = MSG_NOSIGNAL;
#else
        constexpr int flags = 0;
#endif
        const auto sent = ::send(socket.get(), bytes.data() + offset, static_cast<int>(bytes.size() - offset), flags);
        if (sent < 0 && would_block()) {
            continue;
        }
        if (sent <= 0) {
            return result;
        }
        result.dispatched = true;
        offset += static_cast<std::size_t>(sent);
    }
    std::string response;
    while (response.size() <= 4096 && wait_socket(socket.get(), false, context, deadline)) {
        char chunk[1024];
        const auto count = ::recv(socket.get(), chunk, sizeof(chunk), 0);
        if (count < 0 && would_block()) {
            continue;
        }
        if (count <= 0) {
            return result;
        }
        response.append(chunk, static_cast<std::size_t>(count));
        const auto newline = response.find('\n');
        if (newline != std::string::npos) {
            if (newline == response.size() - 1 && response.size() <= 4096) {
                result.reply = json::parse(std::string_view(response).substr(0, newline));
            }
            return result;
        }
    }
    return result;
}

addon::Json_value native_request(const Registration& registration, const addon::Session_wake_target& target,
    const std::string& operation)
{
    return json::object({{"operation", addon::Json_value(operation)},
        {"registration_id", addon::Json_value(registration.id)}, {"instance_id", addon::Json_value(registration.instance)},
        {"token", addon::Json_value(registration.token)},
        {"provider_session_id", addon::Json_value(target.provider_session_id)},
        {"transition_nonce", addon::Json_value(target.correlation.transition_nonce)}});
}

} // namespace

void prepare_wake(const addon::Prepare_request& request, addon::Launch_patch& patch)
{
    if (!request.session_wake) {
        return;
    }
    std::erase_if(patch.set, [](const auto& item) { return item.first == registration_environment; });
    if (std::find(patch.unset.begin(), patch.unset.end(), registration_environment) == patch.unset.end()) {
        patch.unset.emplace_back(registration_environment);
    }
    patch.session_wake = addon::Session_wake_preparation{false, addon::Json_value::object(), "native_wake_unavailable"};
    if (!patch.disposition || !patch.disposition->prepared || !request.profile_storage ||
        std::find(request.declared_assets.begin(), request.declared_assets.end(), wake_asset) == request.declared_assets.end()) {
        return;
    }
    fs::path directory;
    try {
        const auto root = addon::path_from_utf8(request.profile_storage->root);
        if (!root.is_absolute() || fs::canonical(root) != root || !fs::is_directory(root)) {
            return;
        }
        const auto id = addon::random_nonce_hex();
        directory = root / ("wake-" + id);
        if (!make_private_directory(directory)) {
            return;
        }
        const auto secret = json::object({{"version", addon::Json_value(1)}, {"registration_id", addon::Json_value(id)},
            {"token", addon::Json_value(addon::random_nonce_hex() + addon::random_nonce_hex())}});
        const auto path = directory / "registration.json";
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << addon::serialize_json(secret);
        output.close();
#ifndef _WIN32
        fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write);
#endif
        if (!output || !private_object(path, false)) {
            throw std::runtime_error("private_registration_failed");
        }
        patch.session_wake = addon::Session_wake_preparation{true,
            json::object({{"registration_id", addon::Json_value(id)},
                {"registration_directory", addon::Json_value(addon::path_to_utf8(directory))}}), {}};
        std::erase(patch.unset, registration_environment);
        patch.set.emplace_back(registration_environment, addon::path_to_utf8(directory));
    }
    catch (const std::exception&) {
        patch.session_wake = addon::Session_wake_preparation{false, addon::Json_value::object(), "native_wake_unavailable"};
        if (!directory.empty()) {
            std::error_code error;
            fs::remove(directory / "registration.json", error);
            fs::remove(directory, error);
        }
    }
}

void handle_session_wake(addon::Invocation_context& context)
{
    addon::Contract_failure failure;
    if (context.request().operation == "inspect") {
        const auto request = addon::decode_session_wake_inspect_request(context.request().payload, &failure);
        if (!request) {
            context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
            return;
        }
        addon::Session_wake_readiness readiness{false, "unbound_session"};
        if (const auto binding = registration(request->target.binding)) {
            const auto response = exchange(*binding, native_request(*binding, request->target, "inspect"), context);
            if (response.reply) {
                readiness = addon::decode_session_wake_readiness(*response.reply, nullptr).value_or(readiness);
            }
        }
        context.succeed(addon::encode_session_wake_readiness(readiness));
    }
    else
    if (context.request().operation == "notify") {
        const auto request = addon::decode_session_wake_notify_request(context.request().payload, &failure);
        if (!request) {
            context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
            return;
        }
        addon::Session_wake_notify_result outcome{addon::Session_wake_disposition::UNAVAILABLE, "unbound_session", {}};
        if (const auto binding = registration(request->target.binding)) {
            auto message = native_request(*binding, request->target, "notify");
            message.set("notification_id", addon::Json_value(request->notification_id));
            message.set("instruction", addon::Json_value(request->instruction));
            const auto response = exchange(*binding, message, context);
            if (response.dispatched) {
                outcome = {addon::Session_wake_disposition::UNCERTAIN, "native_admission_unacknowledged", {}};
            }
            if (response.reply) {
                outcome = addon::decode_session_wake_notify_result(*response.reply, nullptr).value_or(outcome);
            }
        }
        context.succeed(addon::encode_session_wake_notify_result(outcome));
    }
    else {
        context.reject_unknown_operation();
    }
}

} // namespace omp
