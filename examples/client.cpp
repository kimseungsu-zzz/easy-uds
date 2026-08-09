#include "easy_uds/easy_uds.hpp"

#include <exception>
#include <iostream>

int main() {
    try {
        easy_uds::Client client("/tmp/easy-uds.sock");

        const auto ping = client.request("ping");
        std::cout << "ping: " << ping.status_code << " " << ping.body << '\n';

        const auto echo = client.request("echo", "Hello, easy-uds!");
        std::cout << "echo: " << echo.status_code << " " << echo.body << '\n';
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
