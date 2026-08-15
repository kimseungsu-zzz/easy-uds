#include "../socket_io.hpp"
#include "socket_common.hpp"

#if defined(_WIN32)
#include <array>
#include <climits>
#include <cerrno>
#include <limits>

namespace easy_uds::detail::socket_io {

ssize_t receive(platform_types::NativeSocket fd, void* data, std::size_t size) noexcept {
    const int result = ::recv(platform_windows::to_socket(fd),
                               static_cast<char*>(data),
                               static_cast<int>(size > INT_MAX ? INT_MAX : size), 0);
    if (result == SOCKET_ERROR) {
        platform_windows::last_wsa_error();
        return -1;
    }
    return result;
}

ssize_t send(platform_types::NativeSocket fd, const void* data, std::size_t size) noexcept {
    const int result = ::send(platform_windows::to_socket(fd),
                              static_cast<const char*>(data),
                              static_cast<int>(size > INT_MAX ? INT_MAX : size), 0);
    if (result == SOCKET_ERROR) {
        platform_windows::last_wsa_error();
        return -1;
    }
    return result;
}

namespace {

ssize_t send_buffers(platform_types::NativeSocket fd, iovec* parts,
                      std::size_t part_count) noexcept {
    std::array<WSABUF, 64> buffers{};
    if (part_count > buffers.size()) {
        errno = EINVAL;
        return -1;
    }
    for (std::size_t index = 0; index < part_count; ++index) {
        if (parts[index].iov_len > std::numeric_limits<ULONG>::max()) {
            errno = EMSGSIZE;
            return -1;
        }
        buffers[index].buf = static_cast<char*>(parts[index].iov_base);
        buffers[index].len = static_cast<ULONG>(parts[index].iov_len);
    }
    DWORD sent = 0;
    const int result = ::WSASend(platform_windows::to_socket(fd), buffers.data(),
                                 static_cast<DWORD>(part_count), &sent, 0,
                                 nullptr, nullptr);
    if (result == SOCKET_ERROR) {
        platform_windows::last_wsa_error();
        return -1;
    }
    return static_cast<ssize_t>(sent);
}

} // namespace

ssize_t send_iovecs(platform_types::NativeSocket fd, iovec* parts,
                    std::size_t part_count) noexcept {
    return send_buffers(fd, parts, part_count);
}

ssize_t send_iovecs_nonblocking(platform_types::NativeSocket fd, iovec* parts,
                                std::size_t part_count) noexcept {
    return send_buffers(fd, parts, part_count);
}

int query_socket_error(platform_types::NativeSocket fd, int& socket_error) noexcept {
    int length = sizeof(socket_error);
    const int result = ::getsockopt(platform_windows::to_socket(fd), SOL_SOCKET,
                                    SO_ERROR, reinterpret_cast<char*>(&socket_error),
                                    &length);
    if (result != 0) {
        platform_windows::last_wsa_error();
    }
    return result;
}

} // namespace easy_uds::detail::socket_io
#endif
