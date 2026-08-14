#include "easy_uds/easy_uds.hpp"

#include <iostream>

int main() {
    easy_uds::Server server("/tmp/easy-uds.sock");

    server.on("/ping", [](const easy_uds::Request&) {
        return easy_uds::Response::ok("pong");
    });
    server.on("/echo", [](const easy_uds::Request& request) {
        return easy_uds::Response::ok(request.body);
    });

    std::cout << "Server listening on /tmp/easy-uds.sock\n";
    server.run();
    return 0;
}
