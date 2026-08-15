#pragma once

#include "easy_uds/server.hpp"

#include <memory>
#include <string>

namespace easy_uds::detail {

struct ServerState;

namespace registration {

void exact(const std::shared_ptr<ServerState>& state, std::string route,
           Server::Handler handler, bool serialized);

void exact_options(const std::shared_ptr<ServerState>& state, std::string route,
                   RouteOptions::SimpleHandler simple_handler,
                   RouteOptions::ContextHandler context_handler, bool serialized,
                   bool advanced_options, std::string domain, QueuePolicy policy);

void exact_serialized_options(const std::shared_ptr<ServerState>& state,
                              std::string route,
                              RouteOptions::SimpleHandler simple_handler,
                              RouteOptions::ContextHandler context_handler,
                              bool serialized, std::string domain,
                              QueuePolicy policy);

void prefix(const std::shared_ptr<ServerState>& state, std::string prefix,
            Server::Handler handler, bool serialized);

void prefix_options(const std::shared_ptr<ServerState>& state, std::string prefix,
                    RouteOptions::SimpleHandler simple_handler,
                    RouteOptions::ContextHandler context_handler, bool serialized,
                    bool advanced_options, std::string domain, QueuePolicy policy);

void stream(const std::shared_ptr<ServerState>& state, std::string route,
            Server::StreamHandler handler);

void stream_prefix(const std::shared_ptr<ServerState>& state, std::string prefix,
                   Server::StreamHandler handler);

} // namespace registration
} // namespace easy_uds::detail
