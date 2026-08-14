#include <easy_uds/easy_uds.hpp>

#include <iostream>
#include <string>

int main(int argc, char** argv) {
    const std::string socket_path =
        argc > 1 ? argv[1] : "/tmp/easy-uds-beginner.sock";
    easy_uds::Server server(socket_path);
    server.on("/echo", [](const easy_uds::Request& request) {
        return easy_uds::Response::ok(request.body);
    });
    std::cout << "beginner server listening on " << socket_path << '\n';
    server.run();
    return 0;
}
