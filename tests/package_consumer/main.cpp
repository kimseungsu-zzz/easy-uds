#include <easy_uds/easy_uds.hpp>

#include <chrono>
#include <string_view>
#include <type_traits>

#include <unistd.h>

static_assert(easy_uds::version == std::string_view{"0.7.0"});
static_assert(easy_uds::protocol_version == 2U);
static_assert(sizeof(easy_uds::BorrowedFd) == sizeof(int));
static_assert(sizeof(easy_uds::OwnedFd) == sizeof(int));
static_assert(!std::is_copy_constructible_v<easy_uds::OwnedFd>);

int main() {
    constexpr const char* socket_path =
        "/tmp/easy-uds-package-consumer-never-created.sock";
    (void)::unlink(socket_path);
    easy_uds::ClientOptions options;
    options.connect_timeout = std::chrono::milliseconds{50};
    options.request_timeout = std::chrono::milliseconds{100};
    const easy_uds::Client client(socket_path, options);
    const easy_uds::BorrowedFd empty;
    if (client.socket_path().empty() || empty.valid()) {
        return 1;
    }
    try {
        (void)client.request("ping");
    } catch (const easy_uds::Error& error) {
        return error.code() == easy_uds::ErrorCode::unavailable &&
                       error.system_code()
                   ? 0
                   : 1;
    }
    return 1;
}
