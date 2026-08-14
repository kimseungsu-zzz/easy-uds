#include <cerrno>
#include <cstring>
#include <iostream>

#if defined(__linux__)
#include <linux/io_uring.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

int main() {
#if defined(__linux__) && defined(__NR_io_uring_setup)
    io_uring_params parameters{};
    const int ring_fd = static_cast<int>(::syscall(__NR_io_uring_setup, 1U, &parameters));
    if (ring_fd >= 0) {
        (void)::close(ring_fd);
        std::cout << "io_uring_setup: supported\n";
        return 0;
    }
    if (errno == ENOSYS || errno == EPERM) {
        std::cout << "io_uring_setup: unavailable (" << std::strerror(errno) << ")\n";
        return 0;
    }
    std::cerr << "io_uring_setup failed: " << std::strerror(errno) << '\n';
    return 1;
#else
    std::cout << "io_uring_setup: unavailable on this platform\n";
    return 0;
#endif
}
