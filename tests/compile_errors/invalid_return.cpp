#include <easy_uds/easy_uds.hpp>

int main() {
    easy_uds::Server server("/tmp/easy-uds-invalid-return.sock");
    server.on("/bad", [](const easy_uds::Request&) { return 42; });
}
