#include "easy_uds/easy_uds.hpp"
#include <cassert>
#include <thread>
#include <chrono>
#include <iostream>

int main() {
    using namespace easy_uds;
    using namespace std::chrono_literals;

    Server server("/tmp/test-uds.sock");
    server.on("ping", [](const Request&) { return Response{200, "pong"}; });
    server.on("echo", [](const Request& req) { return Response{200, req.body}; });

    std::thread server_thread([&server]() { server.run(); });
    std::this_thread::sleep_for(100ms);

    Client client("/tmp/test-uds.sock");

    auto resp = client.request("ping", "");
    assert(resp.status_code == 200);
    assert(resp.body == "pong");

    resp = client.request("echo", "hello");
    assert(resp.status_code == 200);
    assert(resp.body == "hello");

    resp = client.request("unknown", "");
    assert(resp.status_code == 404);
    assert(resp.body == "Not Found");

    server.stop();
    server_thread.join();

    bool exception_thrown = false;
    try {
        client.request("ping", "");
    } catch (const std::exception&) {
        exception_thrown = true;
    }
    assert(exception_thrown);

    std::cout << "All tests passed.\n";
    return 0;
}