#include "platform/readiness.hpp"

#include <cstdint>
#include <iostream>

int main() {
    using namespace easy_uds::detail::readiness;

    const auto require = [](bool condition, const char* message) {
        if (!condition) {
            std::cerr << message << '\n';
            return false;
        }
        return true;
    };

    const int poller = create_poller();
    const int wakeup = create_wakeup();
    if (!require(poller >= 0, "create_poller failed") ||
        !require(wakeup >= 0, "create_wakeup failed")) {
        close(wakeup);
        close(poller);
        return 1;
    }

    constexpr std::uint64_t token = 0xfeedbeefULL;
    if (!require(control(poller, Control::add, wakeup, readable, token) == 0,
                 "readiness add failed")) {
        close(wakeup);
        close(poller);
        return 1;
    }
    signal(wakeup);

    Event event{};
    if (!require(wait(poller, &event, 1, 1000) == 1,
                 "readiness wait did not return wakeup") ||
        !require(event.token == token, "readiness token was not preserved") ||
        !require((event.mask & readable) != 0, "readiness mask was not translated")) {
        close(wakeup);
        close(poller);
        return 1;
    }

    consume(wakeup);
    if (!require(wait(poller, &event, 1, 0) == 0,
                 "readiness consume left a pending event") ||
        !require(control(poller, Control::remove, wakeup, 0, 0) == 0,
                 "readiness remove failed")) {
        close(wakeup);
        close(poller);
        return 1;
    }

    close(wakeup);
    close(poller);
    return 0;
}
