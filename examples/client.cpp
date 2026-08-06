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
    const std::string route = argc > 2 ? argv[2] : "hello";
    const std::string body = argc > 3 ? argv[3] : "easy-uds";

    try {
        const easy_uds::Client client(socket_path);
        const auto response = client.request(route, body);
        std::cout << "status: " << response.status << '\n';
        std::cout << "body: " << response.body << '\n';
        return response.ok() ? 0 : 2;
    } catch (const std::exception& error) {
        std::cerr << "client error: " << error.what() << '\n';
        return 1;
    }
}

