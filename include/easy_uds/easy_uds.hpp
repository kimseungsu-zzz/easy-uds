#include "easy_uds/easy_uds.hpp"
#include <iostream>
#include <thread>
#include <chrono>

int main() {
    easy_uds::Server server("/tmp/easy-uds.sock");
    server.on("ping", [](const easy_uds::Request& req) {
        return easy_uds::Response{200, "pong"};
    });
    server.on("echo", [](const easy_uds::Request& req) {
        return easy_uds::Response{200, req.body};
    });
    std::cout << "Server starting..." << std::endl;
    server.run();
    std::cout << "Server stopped." << std::endl;
    return 0;
}