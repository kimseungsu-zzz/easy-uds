#include <easy_uds/easy_uds.hpp>
#include <easy_uds/simple.hpp>

#include <cassert>
#include <chrono>
#include <filesystem>
#include <exception>
#include <thread>
#include <type_traits>

static_assert(std::is_move_constructible_v<easy_uds::Request>);
static_assert(!std::is_copy_constructible_v<easy_uds::Request>);

int main() {
    easy_uds::ClientOptions client_options{};
    easy_uds::ServerOptions server_options{};
    (void)client_options;
    (void)server_options;
    easy_uds::Request request;
    request.route = "/portable";
    request.body = "header-only";
    assert(request.route == "/portable");

    const auto base = std::filesystem::temp_directory_path();
    const auto core_path = (base / "easy-uds-windows-core.sock").string();
    std::error_code cleanup_error;
    std::filesystem::remove(core_path, cleanup_error);
    std::filesystem::remove(core_path + ".lock", cleanup_error);

    easy_uds::Server server(core_path);
    server.on("/ping", [](const easy_uds::Request&) {
        return easy_uds::Response::ok("pong");
    });
    std::exception_ptr server_error;
    std::thread server_thread([&] {
        try {
            server.run();
        } catch (...) {
            server_error = std::current_exception();
        }
    });
    const auto startup_deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(5);
    while (!server.is_running() && std::chrono::steady_clock::now() < startup_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(server.is_running());
    easy_uds::Client client(core_path);
    assert(client.request("/ping").body == "pong");
    auto session = client.session();
    assert(session.request("/ping").body == "pong");
    server.stop();
    server_thread.join();
    if (server_error) {
        std::rethrow_exception(server_error);
    }

    const auto simple_path = (base / "easy-uds-windows-simple.sock").string();
    std::filesystem::remove(simple_path, cleanup_error);
    std::filesystem::remove(simple_path + ".lock", cleanup_error);
    easy_uds::simple::Server simple_server(simple_path);
    simple_server.on("/ping") = "pong";
    std::thread simple_thread([&] { simple_server.run(); });
    const auto simple_deadline = std::chrono::steady_clock::now() +
                                 std::chrono::seconds(5);
    while (!simple_server.is_running() &&
           std::chrono::steady_clock::now() < simple_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(simple_server.is_running());
    easy_uds::simple::Client simple_client(simple_path);
    assert(simple_client.request("/ping") == "pong");
    simple_server.stop();
    simple_thread.join();

    std::filesystem::remove(core_path, cleanup_error);
    std::filesystem::remove(core_path + ".lock", cleanup_error);
    std::filesystem::remove(simple_path, cleanup_error);
    std::filesystem::remove(simple_path + ".lock", cleanup_error);
    return 0;
}
