#include "simple.hpp"

int main() {
    easy_uds::simple::Server server("/tmp/invalid-simple-input.sock");
    server.on("/x") = [](int value) { return value; };
}
