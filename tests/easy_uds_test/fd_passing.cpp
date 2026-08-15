#include "common.hpp"

#include <easy_uds/posix.hpp>

#include <cstdio>
#include <dirent.h>
#include <future>
#include <type_traits>
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

    static_assert(!std::is_copy_constructible_v<OwnedFd>);
    static_assert(!std::is_copy_assignable_v<OwnedFd>);
    static_assert(std::is_nothrow_move_constructible_v<OwnedFd>);
    static_assert(std::is_nothrow_move_assignable_v<OwnedFd>);
    static_assert(!std::is_copy_constructible_v<Request>);
    static_assert(std::is_move_constructible_v<Request>);
    static_assert(std::is_move_assignable_v<Request>);

    // Ownership transfer invalidates the source and release() makes the
    // caller responsible for closing the descriptor again.
    {
        int descriptors[2] = {-1, -1};
        if (::pipe(descriptors) < 0) {
            throw std::system_error(errno, std::generic_category(), "pipe failed");
        }
        OwnedFd first = OwnedFd::adopt(descriptors[0]);
        OwnedFd second = std::move(first);
        expect(!first.valid() && second.valid(),
               "OwnedFd move should transfer unique ownership");
        const int released = second.release();
        expect(!second.valid() && released == descriptors[0],
               "OwnedFd release should relinquish ownership");
        (void)::close(released);
        (void)::close(descriptors[1]);
    }
    try {
        (void)OwnedFd{}.duplicate();
        throw std::runtime_error(
            "test failed: duplicating an empty OwnedFd should report EBADF");
    } catch (const Error& error) {
        expect(error.kind() == ErrorCode::invalid_request,
               "empty OwnedFd should report invalid_request");
        expect(error.system_code() ==
                   std::error_code(EBADF, std::generic_category()),
               "empty OwnedFd should preserve EBADF");
    }
    try {
        (void)BorrowedFd{}.duplicate();
        throw std::runtime_error(
            "test failed: duplicating an empty BorrowedFd should report EBADF");
    } catch (const Error& error) {
        expect(error.kind() == ErrorCode::invalid_request,
               "empty BorrowedFd should report invalid_request");
        expect(error.system_code() ==
                   std::error_code(EBADF, std::generic_category()),
               "empty BorrowedFd should preserve EBADF");
    }

    const std::string payload = "fd-passing-content";
    const std::string path = socket_path("fd-passing");

    ServerOptions server_options;
    server_options.worker_threads = 2;
    server_options.max_connections = 16;
    server_options.stale_socket_grace_period = 0ms;
    Server server(path, server_options);
    server.on("read-fd", RouteOptions{[payload](const Request&,
                                                 const RequestContext& context) {
        const auto fd = posix::request_capabilities(context).received_fd();
        if (!fd.valid()) {
            return Response{500, "no-descriptor"};
        }
        std::string content;
        std::array<char, 32> buffer{};
        while (const ssize_t count = ::read(fd.get(), buffer.data(), buffer.size())) {
            if (count < 0) {
                return Response{500, "read-failed"};
            }
            content.append(buffer.data(), static_cast<std::size_t>(count));
        }
        return content == payload ? Response{200, "ok"} : Response{500, "content-mismatch"};
    }});
    std::promise<OwnedFd> retained_promise;
    std::future<OwnedFd> retained_future = retained_promise.get_future();
    server.on("retain-fd", RouteOptions{[&retained_promise](const Request&,
                                                             const RequestContext& context) {
        try {
            retained_promise.set_value(
                posix::request_capabilities(context).received_fd().duplicate());
            return Response{200, "retained"};
        } catch (...) {
            retained_promise.set_exception(std::current_exception());
            return Response{500, "duplicate-failed"};
        }
    }});
    server.on("no-fd", RouteOptions{[](const Request&, const RequestContext& context) {
        return posix::request_capabilities(context).received_fd().valid()
                   ? Response{500, "unexpected-descriptor"}
                   : Response{200, "no-descriptor"};
    }});
    server.on("ignore-fd", [](const Request&) {
        return Response{200, "ignored"};
    });
    server.on("throw-fd", RouteOptions{[](const Request&, const RequestContext& context) {
        const auto fd = posix::request_capabilities(context).received_fd();
        if (!fd.valid()) {
            return Response{500, "no-descriptor"};
        }
        throw std::runtime_error("intentional descriptor handler failure");
    }});

    std::thread server_thread([&] { server.run(); });
    wait_until_running(server);

    ClientOptions client_options;
    client_options.connect_timeout = 500ms;
    client_options.io_timeout = 2s;
    client_options.request_timeout = 5s;
    Client client(path, client_options);
    OwnedFd retained;

    // A failing expectation must stop and join the server first, or the
    // joinable thread destructor would std::terminate before the message is
    // reported; the wrapper keeps failures diagnosable.
    try {
        expect_throws<std::invalid_argument>(
            [&client] { (void)client.request_fd("read-fd", BorrowedFd{}); },
            "request_fd should reject an empty BorrowedFd");

        expect(client.request("no-fd").status == 200,
               "a request without SCM_RIGHTS should expose an invalid borrowed view");

        // A legacy one-argument handler need not opt into the POSIX view; the
        // internal owner is still released when the normal job is destroyed.
        {
            FILE* const file = ::tmpfile();
            if (file == nullptr) {
                throw std::runtime_error("prepare ignored tmpfile failed");
            }
            const Response response =
                client.request_fd("ignore-fd", borrow_fd(::fileno(file)));
            expect(response.status == 200 && response.body == "ignored",
                   "normal handlers should safely release an unobserved descriptor");
            (void)::fclose(file);
        }

        // Round trip: the server receives a duplicate and reads its content.
        {
            FILE* const file = ::tmpfile();
            if (file == nullptr || ::write(::fileno(file), payload.data(), payload.size()) !=
                                       static_cast<ssize_t>(payload.size()) ||
                ::lseek(::fileno(file), 0, SEEK_SET) < 0) {
                throw std::runtime_error("prepare tmpfile failed");
            }
            const int caller_fd = ::fileno(file);
            const Response response = client.request_fd("read-fd", borrow_fd(caller_fd));
            expect(response.status == 200 && response.body == "ok",
                   "request_fd should deliver a readable descriptor");
            expect(::fcntl(caller_fd, F_GETFD) >= 0,
                   "request_fd should not consume the caller's descriptor");
            (void)::fclose(file);
        }

        // Missing route: the descriptor is still closed by the server.
        {
            FILE* const file = ::tmpfile();
            if (file == nullptr ||
                ::write(::fileno(file), payload.data(), payload.size()) < 0) {
                throw std::runtime_error("write tmpfile failed");
            }
            const Response response = client.request_fd("missing-route", borrow_fd(::fileno(file)));
            expect(response.status == 404, "request_fd to a missing route should return 404");
            (void)::fclose(file);
        }

        // Handler exceptions still destroy the job-local owner after the
        // response path has converted the exception to status 500.
        {
            FILE* const file = ::tmpfile();
            if (file == nullptr) {
                throw std::runtime_error("prepare throwing tmpfile failed");
            }
            const Response response =
                client.request_fd("throw-fd", borrow_fd(::fileno(file)));
            expect(response.status == 500,
                   "descriptor handler exceptions should become a 500 response");
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
            const Response response = client.request_fd("read-fd", borrow_fd(::fileno(file)));
            expect(response.status == 200, "repeated request_fd should succeed");
            (void)::fclose(file);
        }
        const std::size_t fds_after = fd_passing_count_open_fds();
        expect(fds_after <= fds_before + 20, "server should close passed descriptors");

        // Retaining a descriptor beyond the handler is explicit. The
        // duplicate must survive both the request-owned and caller-owned
        // descriptors being closed.
        {
            FILE* const file = ::tmpfile();
            if (file == nullptr || ::write(::fileno(file), payload.data(), payload.size()) !=
                                       static_cast<ssize_t>(payload.size()) ||
                ::lseek(::fileno(file), 0, SEEK_SET) < 0) {
                throw std::runtime_error("prepare retained tmpfile failed");
            }
            const Response response =
                client.request_fd("retain-fd", borrow_fd(::fileno(file)));
            expect(response.status == 200 && response.body == "retained",
                   "handler should be able to duplicate its request descriptor");
            retained = retained_future.get();
            expect((::fcntl(retained.get(), F_GETFD) & FD_CLOEXEC) != 0,
                   "duplicated request descriptor should have close-on-exec set");
            (void)::fclose(file);
        }

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
        if (server_thread.joinable()) {
            server_thread.join();
        }
        std::array<char, 32> retained_buffer{};
        const ssize_t retained_size =
            ::read(retained.get(), retained_buffer.data(), retained_buffer.size());
        expect(retained_size == static_cast<ssize_t>(payload.size()) &&
                   std::string_view(retained_buffer.data(), payload.size()) == payload,
               "duplicated request descriptor should outlive the handler and server");
        cleanup_socket_artifacts(path);
    } catch (...) {
        server.stop();
        if (server_thread.joinable()) {
            server_thread.join();
        }
        cleanup_socket_artifacts(path);
        throw;
    }
}

} // namespace easy_uds::test
