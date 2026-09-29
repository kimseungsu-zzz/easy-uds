#include "easy_uds.h"

#include "easy_uds/client.hpp"
#include "easy_uds/error.hpp"
#include "easy_uds/server.hpp"
#if !defined(_WIN32)
#include "easy_uds/posix.hpp"
#else
#include "easy_uds/windows.hpp"
#endif

#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <array>
#include <exception>
#include <new>
#include <string>
#include <string_view>
#include <utility>

struct easy_uds_server {
    explicit easy_uds_server(const char* path) : value(path) {}
    easy_uds::Server value;
};

struct easy_uds_client {
    explicit easy_uds_client(const char* path,
                             easy_uds::ClientOptions options = {})
        : value(path, options) {}
    easy_uds::Client value;
};

struct easy_uds_session {
    explicit easy_uds_session(easy_uds::Session session)
        : value(std::move(session)) {}
    easy_uds::Session value;
};

struct easy_uds_cancellation {
    easy_uds::CancellationSource value;
};

namespace {
thread_local std::array<char, 1024> last_error_message{};
thread_local int32_t last_error_code = 0;

int context_stop_requested(const void* context) noexcept {
    if (context == nullptr) {
        return 0;
    }
    return static_cast<const easy_uds::RequestContext*>(context)
                   ->stop_requested()
               ? 1
               : 0;
}

void clear_error() noexcept {
    last_error_message[0] = '\0';
    last_error_code = 0;
}

int fail(const char* message, int32_t code = -1) noexcept {
    last_error_code = code;
    try {
        const auto size = std::min(std::strlen(message), last_error_message.size() - 1);
        std::memcpy(last_error_message.data(), message, size);
        last_error_message[size] = '\0';
    } catch (...) {
        constexpr char fallback[] = "easy-uds C API failure";
        std::memcpy(last_error_message.data(), fallback, sizeof(fallback));
    }
    return -1;
}

int fail_current_exception() noexcept {
    try {
        throw;
    } catch (const easy_uds::Error& error) {
        return fail(error.what(), static_cast<int32_t>(error.kind()));
    } catch (const std::exception& error) {
        return fail(error.what());
    } catch (...) {
        return fail("unknown C++ exception");
    }
}

bool valid_bytes(const void* value, size_t size) noexcept {
    return size == 0 || value != nullptr;
}

easy_uds::Response invoke_c_handler(easy_uds_c_handler handler,
                                    void* user_data,
                                    const easy_uds::Request& request) {
    easy_uds_c_response response{500, nullptr, 0, nullptr, nullptr};
    const int result = handler(user_data, request.route.data(),
                               request.route.size(), request.body.data(),
                               request.body.size(), &response);
    struct ResponseBodyGuard {
        easy_uds_c_response& value;
        ~ResponseBodyGuard() {
            if (value.release_body != nullptr) {
                value.release_body(value.release_owner, value.body);
            }
        }
    } guard{response};
    if (result != 0) {
        return {500, "C handler returned an error"};
    }
    if (!valid_bytes(response.body, response.body_size)) {
        return {500, "C handler returned a null body with nonzero size"};
    }
    std::string body;
    if (response.body_size != 0) {
        body.assign(static_cast<const char*>(response.body), response.body_size);
    }
    return {response.status, std::move(body)};
}

easy_uds::Response invoke_c_context_handler(
    easy_uds_c_context_handler handler, void* user_data,
    const easy_uds::Request& request,
    const easy_uds::RequestContext& context) {
    easy_uds_request_context public_context{};
    public_context.struct_size = sizeof(public_context);
    public_context.request_id = context.request_id();
    public_context.peer_pid = -1;
    public_context.peer_uid = UINT64_MAX;
    public_context.peer_gid = UINT64_MAX;
    public_context.stop_requested = context.stop_requested() ? 1U : 0U;
    public_context.peer_pid = context.peer_process_id();
    const std::string_view peer_sid = context.peer_sid();
    public_context.peer_sid = peer_sid.empty() ? nullptr : peer_sid.data();
    public_context.received_handle = nullptr;
    public_context.is_stop_requested = &context_stop_requested;
    public_context.opaque_context = &context;
    if (public_context.peer_pid >= 0 || !peer_sid.empty()) {
        public_context.peer_identity_present = 1;
    }
#if !defined(_WIN32)
    const auto capabilities =
        easy_uds::posix::request_capabilities(context);
    const auto peer = capabilities.peer_credentials();
    if (peer.present) {
        if (peer.pid >= 0) {
            public_context.peer_pid = static_cast<int64_t>(peer.pid);
        }
        public_context.peer_uid = static_cast<uint64_t>(peer.uid);
        public_context.peer_gid = static_cast<uint64_t>(peer.gid);
        public_context.peer_identity_present = 1;
    }
#else
    public_context.received_handle =
        easy_uds::windows::request_capabilities(context)
            .received_handle()
            .get();
#endif
    easy_uds_c_response response{500, nullptr, 0, nullptr, nullptr};
    const int result = handler(
        user_data, &public_context, request.route.data(), request.route.size(),
        request.body.data(), request.body.size(), &response);
    struct ResponseBodyGuard {
        easy_uds_c_response& value;
        ~ResponseBodyGuard() {
            if (value.release_body != nullptr) {
                value.release_body(value.release_owner, value.body);
            }
        }
    } guard{response};
    if (result != 0) {
        return {500, "C context handler returned an error"};
    }
    if (!valid_bytes(response.body, response.body_size)) {
        return {500, "C context handler returned a null body with nonzero size"};
    }
    std::string body;
    if (response.body_size != 0) {
        body.assign(static_cast<const char*>(response.body), response.body_size);
    }
    return {response.status, std::move(body)};
}

int write_response(const easy_uds::Response& response,
                   easy_uds_bytes* response_body,
                   int32_t* response_status) {
    if (response_body == nullptr || response_status == nullptr) {
        return fail("response output pointers must not be null");
    }
    response_body->data = nullptr;
    response_body->size = 0;
    if (!response.body.empty()) {
        response_body->data = std::malloc(response.body.size());
        if (response_body->data == nullptr) {
            return fail("could not allocate response buffer");
        }
        std::memcpy(response_body->data, response.body.data(),
                    response.body.size());
        response_body->size = response.body.size();
    }
    *response_status = response.status;
    return 0;
}
} // namespace

extern "C" {

const char* easy_uds_last_error(void) {
    return last_error_message.data();
}

int32_t easy_uds_last_error_code(void) {
    return last_error_code;
}

uint32_t easy_uds_c_abi_version(void) {
    return EASY_UDS_C_ABI_VERSION;
}

easy_uds_server* easy_uds_server_create(const char* socket_path) {
    clear_error();
    if (socket_path == nullptr) {
        fail("socket_path must not be null");
        return nullptr;
    }
    try {
        return new easy_uds_server(socket_path);
    } catch (...) {
        fail_current_exception();
        return nullptr;
    }
}

int easy_uds_server_on(easy_uds_server* server, const void* route,
                       size_t route_size, easy_uds_c_handler handler,
                       void* user_data) {
    clear_error();
    if (server == nullptr || handler == nullptr || route_size == 0 ||
        !valid_bytes(route, route_size)) {
        return fail("server, route, and handler must be valid");
    }
    try {
        const auto* route_data = static_cast<const char*>(route);
        server->value.on(std::string(route_data, route_size),
                         [handler, user_data](const easy_uds::Request& request) {
                             return invoke_c_handler(handler, user_data, request);
                         });
        return 0;
    } catch (...) {
        return fail_current_exception();
    }
}

int easy_uds_server_on_context(easy_uds_server* server, const void* route,
                               size_t route_size,
                               easy_uds_c_context_handler handler,
                               void* user_data) {
    clear_error();
    if (server == nullptr || handler == nullptr || route_size == 0 ||
        !valid_bytes(route, route_size)) {
        return fail("server, route, and contextual handler must be valid");
    }
    try {
        const auto* route_data = static_cast<const char*>(route);
        server->value.on(
            std::string(route_data, route_size),
            easy_uds::RouteOptions{
                [handler, user_data](const easy_uds::Request& request,
                                     const easy_uds::RequestContext& context) {
                    return invoke_c_context_handler(handler, user_data, request,
                                                    context);
                }});
        return 0;
    } catch (...) {
        return fail_current_exception();
    }
}

int easy_uds_server_run(easy_uds_server* server) {
    clear_error();
    if (server == nullptr) {
        return fail("server must not be null");
    }
    try {
        server->value.run();
        return 0;
    } catch (...) {
        return fail_current_exception();
    }
}

int easy_uds_server_stop(easy_uds_server* server) {
    clear_error();
    if (server == nullptr) {
        return fail("server must not be null");
    }
    server->value.stop();
    return 0;
}

void easy_uds_server_destroy(easy_uds_server* server) {
    delete server;
}

easy_uds_client* easy_uds_client_create(const char* socket_path) {
    return easy_uds_client_create_with_options(socket_path, nullptr);
}

easy_uds_client* easy_uds_client_create_with_options(
    const char* socket_path, const easy_uds_client_options* options) {
    clear_error();
    if (socket_path == nullptr) {
        fail("socket_path must not be null");
        return nullptr;
    }
    try {
        easy_uds::ClientOptions client_options;
        if (options != nullptr) {
            if (options->struct_size < sizeof(easy_uds_client_options)) {
                fail("client options struct_size is too small");
                return nullptr;
            }
            if (options->protocol_version != 2 &&
                options->protocol_version != 3) {
                fail("protocol_version must be 2 or 3");
                return nullptr;
            }
            client_options.protocol_version =
                static_cast<std::uint8_t>(options->protocol_version);
        }
        return new easy_uds_client(socket_path, client_options);
    } catch (...) {
        fail_current_exception();
        return nullptr;
    }
}

easy_uds_session* easy_uds_session_create(easy_uds_client* client) {
    clear_error();
    if (client == nullptr) {
        fail("client must not be null");
        return nullptr;
    }
    try {
        return new easy_uds_session(client->value.session());
    } catch (...) {
        fail_current_exception();
        return nullptr;
    }
}

int easy_uds_session_request(easy_uds_session* session, const void* route,
                             size_t route_size, const void* body,
                             size_t body_size, easy_uds_bytes* response_body,
                             int32_t* response_status) {
    clear_error();
    if (session == nullptr || response_body == nullptr ||
        response_status == nullptr || route_size == 0 ||
        !valid_bytes(route, route_size) || !valid_bytes(body, body_size)) {
        return fail("session, route, and body must be valid");
    }
    try {
        const auto* route_data = static_cast<const char*>(route);
        const auto* body_data = static_cast<const char*>(body);
        return write_response(session->value.request(
            std::string_view(route_data, route_size),
            std::string_view(body_data == nullptr ? "" : body_data, body_size)),
            response_body, response_status);
    } catch (...) {
        return fail_current_exception();
    }
}

easy_uds_cancellation* easy_uds_cancellation_create(void) {
    clear_error();
    try {
        return new easy_uds_cancellation();
    } catch (...) {
        fail_current_exception();
        return nullptr;
    }
}

void easy_uds_cancellation_cancel(easy_uds_cancellation* cancellation) {
    clear_error();
    if (cancellation != nullptr) {
        cancellation->value.cancel();
    }
}

int easy_uds_session_request_cancellable(
    easy_uds_session* session, const easy_uds_cancellation* cancellation,
    const void* route, size_t route_size, const void* body, size_t body_size,
    easy_uds_bytes* response_body, int32_t* response_status) {
    clear_error();
    if (session == nullptr || cancellation == nullptr ||
        response_body == nullptr || response_status == nullptr ||
        route_size == 0 || !valid_bytes(route, route_size) ||
        !valid_bytes(body, body_size)) {
        return fail("session, cancellation, route, and body must be valid");
    }
    try {
        const auto* route_data = static_cast<const char*>(route);
        const auto* body_data = static_cast<const char*>(body);
        const easy_uds::CancellationSource token = cancellation->value;
        return write_response(session->value.request(
            std::string_view(route_data, route_size),
            std::string_view(body_data == nullptr ? "" : body_data, body_size),
            token), response_body, response_status);
    } catch (...) {
        return fail_current_exception();
    }
}

void easy_uds_cancellation_destroy(easy_uds_cancellation* cancellation) {
    delete cancellation;
}

void easy_uds_session_destroy(easy_uds_session* session) {
    delete session;
}

int easy_uds_client_request(easy_uds_client* client, const void* route,
                            size_t route_size, const void* body,
                            size_t body_size, easy_uds_bytes* response_body,
                            int32_t* response_status) {
    clear_error();
    if (client == nullptr || response_body == nullptr ||
        response_status == nullptr || route_size == 0 ||
        !valid_bytes(route, route_size) || !valid_bytes(body, body_size)) {
        return fail("client, route, and body must be valid");
    }
    try {
        const auto* route_data = static_cast<const char*>(route);
        const auto* body_data = static_cast<const char*>(body);
        const auto response = client->value.request(
            std::string_view(route_data, route_size),
            std::string_view(body_data == nullptr ? "" : body_data, body_size));
        return write_response(response, response_body, response_status);
    } catch (...) {
        return fail_current_exception();
    }
}

#if defined(_WIN32)
int easy_uds_client_request_handle(
    easy_uds_client* client, const void* route, size_t route_size,
    void* handle, const void* body, size_t body_size,
    easy_uds_bytes* response_body, int32_t* response_status) {
    clear_error();
    if (client == nullptr || handle == nullptr || response_body == nullptr ||
        response_status == nullptr || route_size == 0 ||
        !valid_bytes(route, route_size) || !valid_bytes(body, body_size)) {
        return fail("client, route, HANDLE, and body must be valid");
    }
    try {
        const auto* route_data = static_cast<const char*>(route);
        const auto* body_data = static_cast<const char*>(body);
        const auto response = client->value.request_handle(
            std::string_view(route_data, route_size),
            easy_uds::windows::BorrowedHandle{static_cast<HANDLE>(handle)},
            std::string_view(body_data == nullptr ? "" : body_data, body_size));
        return write_response(response, response_body, response_status);
    } catch (...) {
        return fail_current_exception();
    }
}
#endif

int easy_uds_client_request_idempotent(
    easy_uds_client* client, const void* route, size_t route_size,
    const void* body, size_t body_size, uint32_t max_attempts,
    easy_uds_bytes* response_body, int32_t* response_status) {
    clear_error();
    if (client == nullptr || response_body == nullptr ||
        response_status == nullptr || route_size == 0 ||
        !valid_bytes(route, route_size) || !valid_bytes(body, body_size) ||
        max_attempts == 0) {
        return fail("client, route, body, and retry count must be valid");
    }
    try {
        const auto* route_data = static_cast<const char*>(route);
        const auto* body_data = static_cast<const char*>(body);
        easy_uds::RetryOptions retry;
        retry.max_attempts = max_attempts;
        const auto response = client->value.request_idempotent(
            std::string_view(route_data, route_size),
            std::string_view(body_data == nullptr ? "" : body_data, body_size),
            retry);
        return write_response(response, response_body, response_status);
    } catch (...) {
        return fail_current_exception();
    }
}

void easy_uds_bytes_free(easy_uds_bytes* bytes) {
    if (bytes != nullptr) {
        std::free(bytes->data);
        bytes->data = nullptr;
        bytes->size = 0;
    }
}

void easy_uds_client_destroy(easy_uds_client* client) {
    delete client;
}

} // extern "C"
