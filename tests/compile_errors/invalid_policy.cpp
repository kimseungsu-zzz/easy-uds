#include <easy_uds/easy_uds.hpp>

int main() {
    auto handler = [](const easy_uds::Request&) {
        return easy_uds::Response::ok();
    };
    (void)easy_uds::RouteOptions{handler}.serialize_in("drive", "latest_wins");
}
