#include <easy_uds/easy_uds.hpp>

#include <iostream>
#include <string>

int main(int argc, char** argv) {
    const std::string socket_path =
        argc > 1 ? argv[1] : "/tmp/easy-uds-beginner.sock";
    const easy_uds::Response response =
        easy_uds::Client(socket_path).request("/echo", "hello");
    if (response.status != easy_uds::status_ok || response.body != "hello") {
        std::cerr << "unexpected response: " << response.status << ' '
                  << response.body << '\n';
        return 1;
    }
    std::cout << response.body << '\n';
    return 0;
}
