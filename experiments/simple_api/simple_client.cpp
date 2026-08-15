#include "simple.hpp"

#include <iostream>
#include <string>

int main(int argc, char** argv) {
    const std::string socket_path =
        argc > 1 ? argv[1] : "/tmp/easy-uds-simple.sock";
    easy_uds::simple::Client client(socket_path);

    std::cout << client.request("/ping") << '\n';
    std::cout << client.request("/echo", "hello") << '\n';
    return 0;
}
