#include "simple.hpp"

#include <iostream>
#include <string>

int main(int argc, char** argv) {
    const std::string socket_path =
        argc > 1 ? argv[1] : "/tmp/easy-uds-simple.sock";
    easy_uds::simple::Server server(socket_path);

    server.on("/ping") = "pong";
    server.on("/echo") = [](std::string_view body) {
        return std::string(body);
    };
    server.on("/version") = [] { return "0.7-simple-experiment"; };

    std::cout << "simple experiment server listening on " << socket_path << '\n';
    server.run();
    return 0;
}
