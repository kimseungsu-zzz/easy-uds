#include <easy_uds/easy_uds.hpp>
#include <easy_uds/simple.hpp>
#include <easy_uds/version.hpp>

#include <string_view>
#include <type_traits>

static_assert(easy_uds::version == std::string_view{"0.8.0-rc.1"});
static_assert(easy_uds::protocol_version == 2U);
static_assert(!std::is_copy_constructible_v<easy_uds::Request>);
static_assert(std::is_move_constructible_v<easy_uds::Request>);

int main() {
    easy_uds::Server core("easy-uds-package-core.sock");
    core.on("/ping", [](const easy_uds::Request&) {
        return easy_uds::Response::ok("pong");
    });
    easy_uds::simple::Server simple("easy-uds-package-simple.sock");
    simple.on("/ping") = "pong";
    return 0;
}
