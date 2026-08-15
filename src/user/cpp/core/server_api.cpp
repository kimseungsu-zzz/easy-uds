#include "easy_uds/server.hpp"

#include "../../../system/runtime/server_registration.hpp"

#include <stdexcept>
#include <utility>

namespace easy_uds {

void Server::on(std::string route, Handler handler) {
    detail::registration::exact(state_, std::move(route), std::move(handler), false);
}

void Server::on(std::string route, RouteOptions options) {
    detail::registration::exact_options(
        state_, std::move(route), std::move(options.simple_handler_),
        std::move(options.context_handler_), options.serialized_, options.serialized_,
        std::move(options.serialization_domain_), options.queue_policy_);
}

void Server::on_prefix(std::string prefix, Handler handler) {
    detail::registration::prefix(state_, std::move(prefix), std::move(handler), false);
}

void Server::on_prefix(std::string prefix, RouteOptions options) {
    detail::registration::prefix_options(
        state_, std::move(prefix), std::move(options.simple_handler_),
        std::move(options.context_handler_), options.serialized_, options.serialized_,
        std::move(options.serialization_domain_), options.queue_policy_);
}

void Server::on_serialized(std::string route, Handler handler) {
    detail::registration::exact(state_, std::move(route), std::move(handler), true);
}

void Server::on_serialized(std::string route, RouteOptions options) {
    detail::registration::exact_serialized_options(
        state_, std::move(route), std::move(options.simple_handler_),
        std::move(options.context_handler_), options.serialized_,
        std::move(options.serialization_domain_), options.queue_policy_);
}

void Server::on_stream(std::string route, StreamHandler handler) {
    detail::registration::stream(state_, std::move(route), std::move(handler));
}

void Server::on_stream_prefix(std::string prefix, StreamHandler handler) {
    detail::registration::stream_prefix(state_, std::move(prefix), std::move(handler));
}

} // namespace easy_uds
