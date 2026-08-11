#include <easy_uds/easy_uds.hpp>

#include <string_view>

static_assert(easy_uds::version == std::string_view{"0.6.0"});
static_assert(easy_uds::protocol_version == 2U);

int main() {
    const easy_uds::Client client("/tmp/easy-uds-package-consumer.sock");
    return client.socket_path().empty() ? 1 : 0;
}
