#include "simple.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#include <unistd.h>

namespace {

using namespace std::chrono_literals;

void expect(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string socket_path() {
    return "/tmp/easy-uds-simple-test-" +
           std::to_string(static_cast<long long>(::getpid())) + ".sock";
}

void cleanup(const std::string& path) {
    (void)::unlink(path.c_str());
    (void)::unlink((path + ".lock").c_str());
}

void wait_until_running(const easy_uds::simple::Server& server) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!server.is_running()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("Simple API test server did not start");
        }
        std::this_thread::yield();
    }
}

}  // namespace

int main() {
    const std::string path = socket_path();
    try {
        easy_uds::simple::Server server(path);
        server.on("/ping") = "pong";
        server.on("/echo") = [](std::string_view body) {
            return std::string(body);
        };
        const std::string prefix = "captured:";
        server.on("/capture") = [prefix](std::string_view body) {
            return prefix + std::string(body);
        };
        server.on("/hello") = [] { return "hello"; };
        server.on("/null") = []() -> const char* { return nullptr; };
        auto unassigned = server.on("/unassigned");
        (void)unassigned;
        bool duplicate_rejected = false;
        try {
            server.on("/ping") = "replaced";
        } catch (const std::runtime_error&) {
            duplicate_rejected = true;
        }
        expect(duplicate_rejected,
               "duplicate simple route must follow Core rejection semantics");

        // The escape hatch proves that Simple and Core routes can coexist.
        server.core().on("/core", [](const easy_uds::Request&) {
            return easy_uds::Response::ok("core");
        });
        server.core().on("/missing", [](const easy_uds::Request&) {
            return easy_uds::Response{easy_uds::status_not_found,
                                      "motor not found"};
        });

        std::thread runner([&] { server.run(); });
        wait_until_running(server);

        easy_uds::simple::Client client(path);
        expect(client.request("/ping") == "pong",
               "rejected duplicate must leave the original route intact");
        expect(client.request("/echo", "hello") == "hello",
               "std::string_view handler must receive the request body");
        expect(client.request("/capture", "hello") == "captured:hello",
               "capturing simple handler must preserve its captured state");
        expect(client.request("/hello") == "hello",
               "no-argument handler must be supported");
        expect(client.request("/core") == "core",
               "Simple and Core routes must coexist");

        try {
            (void)client.request("/missing");
            throw std::runtime_error("Simple Client accepted an application error");
        } catch (const easy_uds::simple::ResponseError& error) {
            expect(error.status() == easy_uds::status_not_found,
                   "ResponseError must preserve application status");
            expect(error.body() == "motor not found",
                   "ResponseError must preserve application body");
        }

        try {
            (void)client.request("/null");
            throw std::runtime_error("null C string did not fail");
        } catch (const easy_uds::simple::ResponseError& error) {
            expect(error.status() == easy_uds::status_internal_error,
                   "null C string must become a deterministic handler error");
            expect(error.body().find("null C string") != std::string_view::npos,
                   "null C string diagnostic must identify the cause");
        }

        server.stop();
        server.stop();
        runner.join();
        cleanup(path);
        std::cout << "Simple API tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        cleanup(path);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
