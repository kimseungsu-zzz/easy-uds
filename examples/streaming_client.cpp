#include "easy_uds/easy_uds.hpp"

#include <algorithm>
#include <cstring>
#include <exception>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    const std::string socket_path =
        argc > 1 ? argv[1] : "/tmp/easy-uds-stream.sock";
    try {
        easy_uds::Client client(socket_path);
        std::size_t remaining = 2U * 1024U * 1024U;
        easy_uds::StreamReader upload = [&remaining](char* buffer,
                                                     std::size_t capacity) {
            const std::size_t size = std::min(capacity, remaining);
            std::memset(buffer, 'x', size);
            remaining -= size;
            return size;
        };
        const int status = client.request_stream("/discard", upload, {});
        std::cout << "stream status: " << status << '\n';
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
