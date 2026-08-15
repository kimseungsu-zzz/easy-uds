#include "../descriptor_passing.hpp"
#include "../socket_io.hpp"

#if defined(_WIN32)
#include <cerrno>

namespace easy_uds::detail::descriptor_passing {

ssize_t send_iovecs(platform_types::NativeSocket fd, iovec* parts,
                    std::size_t part_count, platform_types::NativeSocket passed_fd,
                    bool attach_fd) noexcept {
    (void)passed_fd;
    if (attach_fd) {
        errno = EOPNOTSUPP;
        return -1;
    }
    return socket_io::send_iovecs(fd, parts, part_count);
}

ReceiveResult receive(platform_types::NativeSocket fd, void* data,
                      std::size_t size) {
    ReceiveResult result;
    result.bytes = socket_io::receive(fd, data, size);
    return result;
}

} // namespace easy_uds::detail::descriptor_passing
#endif
