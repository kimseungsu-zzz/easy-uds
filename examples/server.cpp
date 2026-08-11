#include "easy_uds/easy_uds.hpp"

#include <array>
#include <iostream>

int main() {
    easy_uds::Server server("/tmp/easy-uds.sock");

    server.on("ping", [](const easy_uds::Request&) { return easy_uds::Response{200, "pong"}; });
    server.on("echo", [](const easy_uds::Request& request) { return easy_uds::Response{200, request.body}; });
    server.on_stream("discard", [](const easy_uds::StreamReader& body, const easy_uds::Request&) {
        std::array<char, 64 * 1024> buffer{};
        while (body(buffer.data(), buffer.size()) != 0) {
        }
        return easy_uds::StreamResponse{204, {}};
    });

    std::cout << "Server listening on /tmp/easy-uds.sock\n";
    server.run();
    return 0;
}
