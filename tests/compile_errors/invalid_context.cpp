#include <easy_uds/easy_uds.hpp>

int main() {
    easy_uds::Server server("/tmp/easy-uds-invalid-context.sock");
    server.on("/bad", easy_uds::RouteOptions{
                            [](const easy_uds::Request&,
                               const easy_uds::RequestContext& context) {
                                const easy_uds::RequestContext saved = context;
                                (void)saved;
                                return easy_uds::Response::ok();
                            }});
}
