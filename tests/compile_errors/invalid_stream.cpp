#include <easy_uds/easy_uds.hpp>

int main() {
    easy_uds::Server server("/tmp/easy-uds-invalid-stream.sock");
    server.on_stream("/bad", [](int) { return easy_uds::StreamResponse{}; });
}
