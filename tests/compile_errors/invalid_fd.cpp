#include <easy_uds/easy_uds.hpp>

int main() {
    easy_uds::OwnedFd first;
    easy_uds::OwnedFd copied = first;
    (void)copied;
}
