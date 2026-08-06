#include "easy_uds/easy_uds.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::filesystem::path test_socket_path() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() / ("easy-uds-" + std::to_string(unique) + ".sock");
}

} // namespace

int main() {
    const auto path = test_socket_path();

    try {
        easy_uds::Options options;
        options.timeout = std::chrono::seconds(2);

        easy_uds::Server server(path.string(), options);
        server.on("echo", [](const easy_uds::Request& request) {
            return easy_uds::Response{201, request.body};
        });
        server.on("fail", [](const easy_uds::Request&) -> easy_uds::Response {
            throw std::runtime_error("expected failure");
        });
        server.listen();

        std::exception_ptr server_error;
        std::thread server_thread([&] {
            try {
                for (int index = 0; index < 3; ++index) {
                    server.serve_once();
                }
            } catch (...) {
                server_error = std::current_exception();
            }
        });

        easy_uds::Client client(path.string(), options);

        const auto echo = client.request("echo", std::string("hello\0uds", 9));
        require(echo.status == 201, "echo status should be 201");
        require(echo.body == std::string("hello\0uds", 9), "binary payload should round-trip");

        const auto missing = client.request("missing", "payload");
        require(missing.status == 404, "missing route should return 404");
        require(!missing.ok(), "404 response should not be ok");

        const auto failed = client.request("fail");
        require(failed.status == 500, "throwing handler should return 500");
        require(failed.body.find("expected failure") != std::string::npos, "handler error should be described");

        server_thread.join();
        if (server_error) {
            std::rethrow_exception(server_error);
        }

        server.stop();
        require(!server.running(), "server should stop");
        require(!std::filesystem::exists(path), "socket file should be removed after stop");

        bool empty_route_rejected = false;
        try {
            (void)client.request("");
        } catch (const std::invalid_argument&) {
            empty_route_rejected = true;
        }
        require(empty_route_rejected, "empty route should be rejected");

        {
            std::ofstream file(path, std::ios::binary);
            file << "do not delete";
        }
        bool regular_file_rejected = false;
        try {
            easy_uds::Server unsafe_server(path.string(), options);
            unsafe_server.listen();
        } catch (const std::runtime_error&) {
            regular_file_rejected = true;
        }
        require(regular_file_rejected, "a regular file at the socket path should be rejected");
        require(std::filesystem::is_regular_file(path), "the regular file should be preserved");
        std::filesystem::remove(path);

        easy_uds::Server stoppable_server(path.string(), options);
        stoppable_server.listen();
        std::exception_ptr run_error;
        std::thread run_thread([&] {
            try {
                stoppable_server.run();
            } catch (...) {
                run_error = std::current_exception();
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        stoppable_server.stop();
        run_thread.join();
        if (run_error) {
            std::rethrow_exception(run_error);
        }
        require(!stoppable_server.running(), "run loop should stop safely from another thread");

        std::cout << "all easy-uds tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::cerr << "test failure: " << error.what() << '\n';
        return 1;
    }
}
