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
#if defined(_WIN32)
        !require(event.fd == wakeup, "readiness descriptor was not preserved") ||
#endif
        !require((event.mask & readable) != 0, "readiness mask was not translated")) {
        close(wakeup);
        close(poller);
        return 1;
    }

    consume(wakeup);
    if (!require(wait(poller, &event, 1, 0) == 0,
                 "readiness consume left a pending event") ||
        !require(control(poller, Control::modify, wakeup, readable | writable,
                         token + 1) == 0,
                 "readiness modify failed")) {
        close(wakeup);
        close(poller);
        return 1;
    }

    signal(wakeup);
    if (!require(wait(poller, &event, 1, 1000) == 1,
                 "modified readiness registration did not return wakeup") ||
        !require(event.token == token + 1,
                 "modified readiness token was not preserved") ||
#if defined(_WIN32)
        !require(event.fd == wakeup, "modified readiness descriptor was not preserved") ||
#endif
        !require((event.mask & writable) != 0,
                 "modified readiness mask was not translated")) {
        close(wakeup);
        close(poller);
        return 1;
    }
    consume(wakeup);
    if (!require(control(poller, Control::remove, wakeup, 0, 0) == 0,
                 "readiness remove failed")) {
        close(wakeup);
        close(poller);
        return 1;
    }

    close(wakeup);
    close(poller);
    return 0;
}
