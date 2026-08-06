#include "easy_uds/easy_uds.hpp"

#include <iostream>
#include <string>

int main(int argc, char** argv) {
#ifdef _WIN32
    const std::string default_path = "easy-uds-demo.sock";
#else
    const std::string default_path = "/tmp/easy-uds-demo.sock";
#endif
    const std::string socket_path = argc > 1 ? argv[1] : default_path;

    try {
        easy_uds::Server server(socket_path);
        server.on("echo", [](const easy_uds::Request& request) {
            return easy_uds::Response{200, request.body};
        });
        server.on("hello", [](const easy_uds::Request& request) {
            const auto name = request.body.empty() ? std::string("world") : request.body;
            return easy_uds::Response{200, "Hello, " + name + "!"};
        });

        std::cout << "easy-uds server listening on " << socket_path << '\n';
        std::cout << "Press Ctrl+C to stop.\n";
        server.run();
    } catch (const std::exception& error) {
        std::cerr << "server error: " << error.what() << '\n';
        return 1;
    }
}

