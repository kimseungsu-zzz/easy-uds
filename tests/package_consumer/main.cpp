#include <easy_uds/easy_uds.hpp>

#include <string_view>
#include <type_traits>

static_assert(easy_uds::version == std::string_view{"0.7.0"});
static_assert(easy_uds::protocol_version == 2U);
static_assert(sizeof(easy_uds::BorrowedFd) == sizeof(int));
static_assert(sizeof(easy_uds::OwnedFd) == sizeof(int));
static_assert(!std::is_copy_constructible_v<easy_uds::OwnedFd>);

int main() {
    const easy_uds::Client client("/tmp/easy-uds-package-consumer.sock");
    const easy_uds::BorrowedFd empty;
    return client.socket_path().empty() || empty.valid() ? 1 : 0;
}
