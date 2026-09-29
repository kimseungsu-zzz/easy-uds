#include "../peer_identity.hpp"

#if defined(_WIN32)
#include <winsock2.h>
#include <afunix.h>
#include <windows.h>
#include <sddl.h>

#include <vector>
#include <utility>

namespace easy_uds::detail::peer_identity {
namespace {

struct HandleGuard {
    HANDLE value = nullptr;
    ~HandleGuard() {
        if (value != nullptr && value != INVALID_HANDLE_VALUE) {
            ::CloseHandle(value);
        }
    }
    HANDLE release() noexcept { return std::exchange(value, nullptr); }
};

void capture_sid(HANDLE process, Identity& identity) {
    HANDLE raw_token = nullptr;
    if (!::OpenProcessToken(process, TOKEN_QUERY, &raw_token)) {
        return;
    }
    HandleGuard token{raw_token};
    DWORD required = 0;
    (void)::GetTokenInformation(token.value, TokenUser, nullptr, 0, &required);
    if (required == 0) {
        return;
    }
    std::vector<unsigned char> storage(required);
    if (!::GetTokenInformation(token.value, TokenUser, storage.data(), required,
                               &required)) {
        return;
    }
    const auto* user = reinterpret_cast<const TOKEN_USER*>(storage.data());
    LPWSTR sid_text = nullptr;
    if (!::ConvertSidToStringSidW(user->User.Sid, &sid_text)) {
        return;
    }
    struct LocalStringGuard {
        LPWSTR value;
        ~LocalStringGuard() { ::LocalFree(value); }
    } sid_guard{sid_text};
    const int required_utf8 = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, sid_text, -1, nullptr, 0, nullptr, nullptr);
    if (required_utf8 <= 1) {
        return;
    }
    std::string sid(static_cast<std::size_t>(required_utf8), '\0');
    if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, sid_text, -1,
                              sid.data(), required_utf8, nullptr, nullptr) == 0) {
        return;
    }
    sid.pop_back();
    identity.sid = std::move(sid);
}

} // namespace

bool query_peer_process_id(platform_types::NativeSocket fd,
                           std::uint32_t& pid) noexcept {
#if defined(SIO_AF_UNIX_GETPEERPID)
    ULONG native_pid = 0;
    DWORD bytes_returned = 0;
    if (::WSAIoctl(static_cast<SOCKET>(fd), SIO_AF_UNIX_GETPEERPID, nullptr, 0,
                   &native_pid, static_cast<DWORD>(sizeof(native_pid)),
                   &bytes_returned, nullptr, nullptr) != 0 || native_pid == 0) {
        return false;
    }
    pid = static_cast<std::uint32_t>(native_pid);
    return true;
#else
    (void)fd;
    (void)pid;
    return false;
#endif
}

Identity capture(platform_types::NativeSocket fd) noexcept {
    Identity identity;
#if defined(SIO_AF_UNIX_GETPEERPID)
    ULONG pid = 0;
    DWORD bytes_returned = 0;
    const SOCKET socket = static_cast<SOCKET>(fd);
    if (::WSAIoctl(socket, SIO_AF_UNIX_GETPEERPID, nullptr, 0, &pid,
                   static_cast<DWORD>(sizeof(pid)), &bytes_returned, nullptr,
                   nullptr) == 0 && pid != 0) {
        identity.pid = static_cast<std::int64_t>(pid);
        identity.present = true;
        HANDLE process = ::OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_DUP_HANDLE, FALSE,
            static_cast<DWORD>(pid));
        if (process == nullptr) {
            process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                    static_cast<DWORD>(pid));
        }
        if (process != nullptr) {
            HandleGuard process_guard{process};
            try {
                capture_sid(process, identity);
            } catch (...) {
                // Keep the kernel-observed PID when token lookup is unavailable.
            }
            HANDLE owned_process = process_guard.release();
            try {
                identity.process_handle = std::shared_ptr<void>(
                    owned_process, [](void* value) {
                        ::CloseHandle(static_cast<HANDLE>(value));
                    });
            } catch (...) {
                // shared_ptr's pointer/deleter constructor closes on failure.
            }
        }
    }
#else
    (void)fd;
#endif
    return identity;
}

} // namespace easy_uds::detail::peer_identity
#endif
