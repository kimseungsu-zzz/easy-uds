#include <easy_uds/easy_uds.hpp>
#include <easy_uds/simple.hpp>

#include <cassert>
#include <type_traits>

static_assert(std::is_move_constructible_v<easy_uds::Request>);
static_assert(!std::is_copy_constructible_v<easy_uds::Request>);

int main() {
    easy_uds::ClientOptions client_options{};
    easy_uds::ServerOptions server_options{};
    (void)client_options;
    (void)server_options;
    easy_uds::Request request;
    request.route = "/portable";
    request.body = "header-only";
    assert(request.route == "/portable");
    return 0;
}
