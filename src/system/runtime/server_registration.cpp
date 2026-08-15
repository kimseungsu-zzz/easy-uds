#include "server_registration.hpp"

#include "../reactor/core.hpp"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace easy_uds::detail::registration {
namespace {

void validate_route(std::string_view route, std::size_t max_message_size) {
    if (route.empty()) {
        throw std::invalid_argument("route must not be empty");
    }
    if (route.size() > max_message_size) {
        throw std::length_error("route exceeds server max_message_size");
    }
}

template <typename Mutation>
void update_handler_registry(const std::shared_ptr<ServerState>& state,
                             Mutation&& mutation) {
    std::lock_guard<std::mutex> lock(state->handler_registration_mutex);
    const auto current =
        std::atomic_load_explicit(&state->handler_registry, std::memory_order_acquire);
    auto updated = std::make_shared<HandlerRegistry>(*current);
    mutation(*updated);
    std::shared_ptr<const HandlerRegistry> published = std::move(updated);
    std::atomic_store_explicit(&state->handler_registry, std::move(published),
                               std::memory_order_release);
}

std::shared_ptr<const HandlerEntry> make_route_options_entry(
    RouteOptions::SimpleHandler simple_handler,
    RouteOptions::ContextHandler context_handler, bool serialized,
    bool advanced_options, std::string domain, QueuePolicy policy) {
    unsigned char flags = serialized ? handler_serialized_flag : 0U;
    if (advanced_options) {
        flags = static_cast<unsigned char>(flags | handler_advanced_options_flag);
    }
    RouteScheduling scheduling{std::move(domain), policy};
    if (context_handler) {
        flags = static_cast<unsigned char>(flags | handler_contextual_flag);
        return std::make_shared<const HandlerEntry>(HandlerEntry{
            Server::Handler{ContextHandlerAdapter{
                std::move(context_handler), std::move(scheduling)}},
            flags});
    }
    if (advanced_options) {
        return std::make_shared<const HandlerEntry>(HandlerEntry{
            Server::Handler{SimpleRouteOptionsAdapter{
                std::move(simple_handler), std::move(scheduling)}},
            flags});
    }
    return std::make_shared<const HandlerEntry>(
        HandlerEntry{std::move(simple_handler), flags});
}

void validate_handler(bool present, const char* message) {
    if (!present) {
        throw std::invalid_argument(message);
    }
}

} // namespace

void exact(const std::shared_ptr<ServerState>& state, std::string route,
           Server::Handler handler, bool serialized) {
    validate_route(route, state->options.max_message_size);
    validate_handler(static_cast<bool>(handler), "handler must not be empty");
    update_handler_registry(state, [&](HandlerRegistry& registry) {
        if (registry.handlers.find(route) != registry.handlers.end()) {
            throw std::runtime_error("route already exists");
        }
        registry.handlers.emplace(
            std::move(route),
            std::make_shared<const HandlerEntry>(
                HandlerEntry{std::move(handler),
                             static_cast<unsigned char>(serialized
                                                            ? handler_serialized_flag
                                                            : 0U)}));
    });
}

void exact_options(const std::shared_ptr<ServerState>& state, std::string route,
                   RouteOptions::SimpleHandler simple_handler,
                   RouteOptions::ContextHandler context_handler, bool serialized,
                   bool advanced_options, std::string domain, QueuePolicy policy) {
    validate_route(route, state->options.max_message_size);
    validate_handler(static_cast<bool>(simple_handler) || static_cast<bool>(context_handler),
                     "handler must not be empty");
    update_handler_registry(state, [&](HandlerRegistry& registry) {
        if (registry.handlers.find(route) != registry.handlers.end()) {
            throw std::runtime_error("route already exists");
        }
        registry.handlers.emplace(
            std::move(route),
            make_route_options_entry(std::move(simple_handler), std::move(context_handler),
                                     serialized, advanced_options, std::move(domain), policy));
    });
}

void exact_serialized_options(const std::shared_ptr<ServerState>& state,
                              std::string route,
                              RouteOptions::SimpleHandler simple_handler,
                              RouteOptions::ContextHandler context_handler,
                              bool serialized, std::string domain,
                              QueuePolicy policy) {
    validate_route(route, state->options.max_message_size);
    validate_handler(static_cast<bool>(simple_handler) || static_cast<bool>(context_handler),
                     "handler must not be empty");
    if (serialized && (!domain.empty() || policy != QueuePolicy::fifo)) {
        throw std::invalid_argument(
            "on_serialized uses the default FIFO domain; use on with "
            "RouteOptions::serialize_in for advanced scheduling");
    }
    update_handler_registry(state, [&](HandlerRegistry& registry) {
        if (registry.handlers.find(route) != registry.handlers.end()) {
            throw std::runtime_error("route already exists");
        }
        registry.handlers.emplace(
            std::move(route),
            make_route_options_entry(std::move(simple_handler), std::move(context_handler),
                                     true, false, {}, QueuePolicy::fifo));
    });
}

void prefix(const std::shared_ptr<ServerState>& state, std::string prefix_route,
            Server::Handler handler, bool serialized) {
    validate_route(prefix_route, state->options.max_message_size);
    validate_handler(static_cast<bool>(handler), "handler must not be empty");
    update_handler_registry(state, [&](HandlerRegistry& registry) {
        for (const auto& entry : registry.handler_prefixes) {
            if (entry.first == prefix_route) {
                throw std::runtime_error("prefix route already exists");
            }
        }
        registry.handler_prefixes.emplace_back(
            std::move(prefix_route),
            std::make_shared<const HandlerEntry>(
                HandlerEntry{std::move(handler),
                             static_cast<unsigned char>(serialized
                                                            ? handler_serialized_flag
                                                            : 0U)}));
        std::sort(registry.handler_prefixes.begin(), registry.handler_prefixes.end(),
                  [](const auto& left, const auto& right) {
                      return left.first.size() > right.first.size();
                  });
    });
}

void prefix_options(const std::shared_ptr<ServerState>& state, std::string prefix_route,
                    RouteOptions::SimpleHandler simple_handler,
                    RouteOptions::ContextHandler context_handler, bool serialized,
                    bool advanced_options, std::string domain, QueuePolicy policy) {
    validate_route(prefix_route, state->options.max_message_size);
    validate_handler(static_cast<bool>(simple_handler) || static_cast<bool>(context_handler),
                     "handler must not be empty");
    update_handler_registry(state, [&](HandlerRegistry& registry) {
        for (const auto& entry : registry.handler_prefixes) {
            if (entry.first == prefix_route) {
                throw std::runtime_error("prefix route already exists");
            }
        }
        registry.handler_prefixes.emplace_back(
            std::move(prefix_route),
            make_route_options_entry(std::move(simple_handler), std::move(context_handler),
                                     serialized, advanced_options, std::move(domain), policy));
        std::sort(registry.handler_prefixes.begin(), registry.handler_prefixes.end(),
                  [](const auto& left, const auto& right) {
                      return left.first.size() > right.first.size();
                  });
    });
}

void stream(const std::shared_ptr<ServerState>& state, std::string route,
            Server::StreamHandler handler) {
    validate_route(route, state->options.max_message_size);
    validate_handler(static_cast<bool>(handler), "stream handler must not be empty");
    update_handler_registry(state, [&](HandlerRegistry& registry) {
        if (registry.stream_handlers.find(route) != registry.stream_handlers.end()) {
            throw std::runtime_error("stream route already exists");
        }
        registry.stream_handlers.emplace(
            std::move(route),
            std::make_shared<const StreamHandlerEntry>(
                StreamHandlerEntry{std::move(handler)}));
    });
}

void stream_prefix(const std::shared_ptr<ServerState>& state, std::string prefix_route,
                   Server::StreamHandler handler) {
    validate_route(prefix_route, state->options.max_message_size);
    validate_handler(static_cast<bool>(handler), "stream handler must not be empty");
    update_handler_registry(state, [&](HandlerRegistry& registry) {
        for (const auto& entry : registry.stream_prefixes) {
            if (entry.first == prefix_route) {
                throw std::runtime_error("prefix route already exists");
            }
        }
        registry.stream_prefixes.emplace_back(
            std::move(prefix_route),
            std::make_shared<const StreamHandlerEntry>(
                StreamHandlerEntry{std::move(handler)}));
        std::sort(registry.stream_prefixes.begin(), registry.stream_prefixes.end(),
                  [](const auto& left, const auto& right) {
                      return left.first.size() > right.first.size();
                  });
    });
}

} // namespace easy_uds::detail::registration
