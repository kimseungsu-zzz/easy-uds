#include "simple.hpp"

int main() {
    easy_uds::simple::Server server("/tmp/invalid-simple-assignment.sock");
    server.on("/x") = 1234;
}
