#include <easy_uds/easy_uds.hpp>

int main() {
    easy_uds::Server server("/tmp/easy-uds-invalid-handler.sock");
    server.on("/bad", [](int) { return easy_uds::Response::ok(); });
}
