#include "easy_uds/easy_uds.hpp"

#include <array>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    const std::string socket_path =
        argc > 1 ? argv[1] : "/tmp/easy-uds-stream.sock";
    easy_uds::Server server(socket_path);

    server.on_stream(
        "/discard",
        [](const easy_uds::StreamReader& body, const easy_uds::Request&) {
            std::array<char, 64 * 1024> buffer{};
            while (body(buffer.data(), buffer.size()) != 0) {
            }
            return easy_uds::StreamResponse{204, {}};
        });

    std::cout << "Streaming server listening on " << socket_path << '\n';
    server.run();
    return 0;
}
