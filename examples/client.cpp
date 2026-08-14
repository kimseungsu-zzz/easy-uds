#include "easy_uds/easy_uds.hpp"

#include <algorithm>
#include <cstring>
#include <exception>
#include <iostream>

int main() {
    try {
        easy_uds::Client client("/tmp/easy-uds.sock");

        const auto ping = client.request("/ping");
        std::cout << "ping: " << ping.status << " " << ping.body << '\n';

        const auto echo = client.request("/echo", "Hello, easy-uds!");
        std::cout << "echo: " << echo.status << " " << echo.body << '\n';

        std::size_t remaining = 2U * 1024U * 1024U;
        easy_uds::StreamReader upload = [&remaining](char* buffer, std::size_t capacity) {
            const std::size_t size = std::min(capacity, remaining);
            std::memset(buffer, 'x', size);
            remaining -= size;
            return size;
        };
        const int stream_status = client.request_stream("/discard", upload, {});
        std::cout << "stream: " << stream_status << '\n';
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
