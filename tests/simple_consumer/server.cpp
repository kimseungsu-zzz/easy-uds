#include <easy_uds/simple.hpp>

#include <iostream>
#include <string>

int main(int argc, char** argv) {
    const std::string socket_path =
        argc > 1 ? argv[1] : "/tmp/easy-uds-simple-consumer.sock";
    easy_uds::simple::Server server(socket_path);
    server.on("/ping") = "pong";
    server.on("/echo") = [](std::string_view body) {
        return std::string(body);
    };
    server.core().on("/missing", [](const easy_uds::Request&) {
        return easy_uds::Response{easy_uds::status_not_found, "missing"};
    });
    std::cout << "simple consumer server listening\n";
    server.run();
    return 0;
}
