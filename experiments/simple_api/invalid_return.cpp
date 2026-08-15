#include "simple.hpp"

int main() {
    easy_uds::simple::Server server("/tmp/invalid-simple-return.sock");
    server.on("/x") = [](std::string_view) { return 1234; };
}
