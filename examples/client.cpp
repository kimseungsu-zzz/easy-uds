#include "easy_uds/easy_uds.hpp"
#include <iostream>

int main() {
    easy_uds::Client client("/tmp/easy-uds.sock");
    try {
        auto resp = client.request("ping", "");
        std::cout << "Ping response: " << resp.body << std::endl;
        resp = client.request("echo", "Hello, easy-uds!");
        std::cout << "Echo response: " << resp.body << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
    return 0;
}