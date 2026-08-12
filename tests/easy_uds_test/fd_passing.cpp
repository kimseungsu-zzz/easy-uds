#include "common.hpp"

#include <cstdio>
#include <dirent.h>

namespace easy_uds::test {

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
