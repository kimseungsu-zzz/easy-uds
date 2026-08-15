#include <easy_uds/simple.hpp>

#include <iostream>
#include <string>

int main(int argc, char** argv) {
    const std::string socket_path =
        argc > 1 ? argv[1] : "/tmp/easy-uds-simple-consumer.sock";
    easy_uds::simple::Client client(socket_path);
    if (client.request("/ping") != "pong" ||
        client.request("/echo", "hello") != "hello") {
        return 1;
    }
    try {
        (void)client.request("/missing");
        return 1;
    } catch (const easy_uds::simple::ResponseError& error) {
        if (error.status() != easy_uds::status_not_found ||
            error.body() != "missing") {
            return 1;
        }
    }
    std::cout << "simple package consumer passed\n";
    return 0;
}
