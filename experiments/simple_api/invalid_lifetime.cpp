#include "simple.hpp"

int main() {
    easy_uds::simple::Server server("/tmp/invalid-simple-lifetime.sock");
    auto route = server.on("/x");
    route = "pong";
}
