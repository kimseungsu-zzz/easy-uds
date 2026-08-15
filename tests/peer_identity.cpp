#include "platform/peer_identity.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <stdexcept>

#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

} // namespace

int main() {
    try {
        int sockets[2] = {-1, -1};
        require(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
                "socketpair failed for peer identity");
        const auto identity = easy_uds::detail::peer_identity::capture(sockets[0]);
        require(identity.present, "valid socket did not expose peer identity");
        require(identity.pid == static_cast<std::int64_t>(::getpid()),
                "peer identity pid does not match socketpair peer");
        require(identity.uid == static_cast<std::uint64_t>(::getuid()),
                "peer identity uid does not match getuid");
        require(identity.gid == static_cast<std::uint64_t>(::getgid()),
                "peer identity gid does not match getgid");
        (void)::close(sockets[0]);
        (void)::close(sockets[1]);

        const auto invalid = easy_uds::detail::peer_identity::capture(-1);
        require(!invalid.present, "invalid fd unexpectedly exposed peer identity");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "peer_identity_test: %s\n", error.what());
        return 1;
    }
}
