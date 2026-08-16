// Include every installed common header explicitly.  The umbrella include is
// convenient, but this consumer also acts as the Windows public-header
// portability smoke: no common header may accidentally pull in a POSIX-only
// capability.
#include <easy_uds/client.hpp>
#include <easy_uds/easy_uds.hpp>
#include <easy_uds/error.hpp>
#include <easy_uds/options.hpp>
#include <easy_uds/request.hpp>
#include <easy_uds/request_context.hpp>
#include <easy_uds/response.hpp>
#include <easy_uds/server.hpp>
#include <easy_uds/session.hpp>
#include <easy_uds/simple.hpp>
#include <easy_uds/stats.hpp>
#include <easy_uds/stream.hpp>
#include <easy_uds/version.hpp>

#include <string_view>
#include <type_traits>

static_assert(easy_uds::version == std::string_view{"0.8.0"});
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
