#include "common.hpp"

#include <cstdio>
#include <dirent.h>
#include <sys/uio.h>

namespace easy_uds::test {
namespace {

void send_frame_with_fd(int socket, const std::vector<unsigned char>& bytes, int passed_fd) {
    iovec vector{const_cast<unsigned char*>(bytes.data()), bytes.size()};
    msghdr message{};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    char control[CMSG_SPACE(sizeof(int))]{};
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    cmsghdr* const header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(header), &passed_fd, sizeof(passed_fd));

    ssize_t sent = -1;
    do {
        sent = ::sendmsg(socket, &message, MSG_NOSIGNAL);
    } while (sent < 0 && errno == EINTR);
    if (sent <= 0) {
        throw std::system_error(errno, std::generic_category(), "sendmsg test frame failed");
    }
    const std::size_t consumed = static_cast<std::size_t>(sent);
    if (consumed < bytes.size()) {
        send_exact(socket, bytes.data() + consumed, bytes.size() - consumed);
    }
}

} // namespace

std::size_t fd_passing_count_open_fds() {
    std::size_t count = 0;
    if (DIR* dir = ::opendir("/proc/self/fd")) {
        while (::readdir(dir) != nullptr) {
            ++count;
        }
        (void)::closedir(dir);
    }
    return count;
}

void test_fd_passing() {
    using namespace easy_uds;

    const std::string payload = "fd-passing-content";
    const std::string path = socket_path("fd-passing");

    ServerOptions server_options;
    server_options.worker_threads = 2;
    server_options.max_connections = 16;
    server_options.stale_socket_grace_period = 0ms;
    Server server(path, server_options);
    server.on("read-fd", [payload](const Request& request) {
        if (request.fd < 0) {
            return Response{500, "no-descriptor"};
        }
        std::string content;
        std::array<char, 32> buffer{};
        while (const ssize_t count = ::read(request.fd, buffer.data(), buffer.size())) {
            if (count < 0) {
                return Response{500, "read-failed"};
            }
            content.append(buffer.data(), static_cast<std::size_t>(count));
        }
        return content == payload ? Response{200, "ok"} : Response{500, "content-mismatch"};
    });

    std::thread server_thread([&] { server.run(); });
    wait_until_running(server);

    ClientOptions client_options;
    client_options.connect_timeout = 500ms;
    client_options.io_timeout = 2s;
    client_options.request_timeout = 5s;
    Client client(path, client_options);

    // A failing expectation must stop and join the server first, or the
    // joinable thread destructor would std::terminate before the message is
    // reported; the wrapper keeps failures diagnosable.
    try {
        // Round trip: the server receives a duplicate and reads its content.
        {
            FILE* const file = ::tmpfile();
            if (file == nullptr || ::write(::fileno(file), payload.data(), payload.size()) !=
                                       static_cast<ssize_t>(payload.size()) ||
                ::lseek(::fileno(file), 0, SEEK_SET) < 0) {
                throw std::runtime_error("prepare tmpfile failed");
            }
            const Response response = client.request_fd("read-fd", ::fileno(file));
            expect(response.status == 200 && response.body == "ok",
                   "request_fd should deliver a readable descriptor");
            (void)::fclose(file);
        }

        // Missing route: the descriptor is still closed by the server.
        {
            FILE* const file = ::tmpfile();
            if (::write(::fileno(file), payload.data(), payload.size()) < 0) {
                throw std::runtime_error("write tmpfile failed");
            }
            const Response response = client.request_fd("missing-route", ::fileno(file));
            expect(response.status == 404, "request_fd to a missing route should return 404");
            (void)::fclose(file);
        }

        // The server must close its copy of every passed descriptor after the
        // handler returns, so rapid exchanges must not grow the fd table.
        const std::size_t fds_before = fd_passing_count_open_fds();
        for (std::size_t iteration = 0; iteration < 200; ++iteration) {
            FILE* const file = ::tmpfile();
            if (file == nullptr || ::write(::fileno(file), payload.data(), payload.size()) < 0 ||
                ::lseek(::fileno(file), 0, SEEK_SET) < 0) {
                throw std::runtime_error("prepare tmpfile failed");
            }
            const Response response = client.request_fd("read-fd", ::fileno(file));
            expect(response.status == 200, "repeated request_fd should succeed");
            (void)::fclose(file);
        }
        const std::size_t fds_after = fd_passing_count_open_fds();
        expect(fds_after <= fds_before + 20, "server should close passed descriptors");

        // FD passing is deliberately one-shot only. A nonzero request id is a
        // multiplexed/session frame and must be rejected even when the peer
        // supplies a syntactically valid descriptor.
        {
            FILE* const file = ::tmpfile();
            if (file == nullptr) {
                throw std::runtime_error("prepare nonzero-id tmpfile failed");
            }
            const int raw = connect_raw(path);
            auto request = fixed_request(7, "read-fd");
            request[7] = 1; // carries_fd_flag, big-endian bit 0
            try {
                send_frame_with_fd(raw, request, ::fileno(file));
                expect_peer_closed(raw, 2s,
                                   "nonzero request-id descriptor frame should be rejected");
            } catch (...) {
                (void)::close(raw);
                (void)::fclose(file);
                throw;
            }
            (void)::close(raw);
            (void)::fclose(file);
        }

        server.stop();
        server_thread.join();
        cleanup_socket_artifacts(path);
    } catch (...) {
        server.stop();
        server_thread.join();
        cleanup_socket_artifacts(path);
        throw;
    }
}

} // namespace easy_uds::test
