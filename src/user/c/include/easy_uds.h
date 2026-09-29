#ifndef EASY_UDS_C_API_H
#define EASY_UDS_C_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EASY_UDS_C_ABI_VERSION 1

typedef struct easy_uds_server easy_uds_server;
typedef struct easy_uds_client easy_uds_client;
typedef struct easy_uds_session easy_uds_session;
typedef struct easy_uds_cancellation easy_uds_cancellation;

typedef struct easy_uds_client_options {
    uint32_t struct_size;
    uint32_t protocol_version; /* 2 by default; 3 enables cancellation. */
} easy_uds_client_options;

typedef struct easy_uds_bytes {
    void* data;
    size_t size;
} easy_uds_bytes;

typedef struct easy_uds_c_response {
    int32_t status;
    /* The body must remain valid until release_body runs. If release_body is
     * null, ownership and lifetime remain with the handler. */
    const void* body;
    size_t body_size;
    void (*release_body)(void* owner, const void* body);
    void* release_owner;
} easy_uds_c_response;

typedef struct easy_uds_request_context {
    uint32_t struct_size;
    uint32_t request_id;
    int64_t peer_pid; /* -1 when unavailable */
    uint64_t peer_uid; /* UINT64_MAX when unavailable */
    uint64_t peer_gid; /* UINT64_MAX when unavailable */
    const char* peer_sid; /* callback-scoped UTF-8 SID, or null */
    uint32_t peer_identity_present;
    /* Borrowed for the duration of this callback; null when unavailable. */
    void* received_handle; /* callback-scoped HANDLE on Windows, otherwise null */
    uint32_t stop_requested;
    /* Query the live cooperative stop state during this callback. */
    int (*is_stop_requested)(const void* context);
    const void* opaque_context;
} easy_uds_request_context;

typedef int (*easy_uds_c_handler)(
    void* user_data, const void* route, size_t route_size,
    const void* body, size_t body_size, easy_uds_c_response* response);
typedef int (*easy_uds_c_context_handler)(
    void* user_data, const easy_uds_request_context* context,
    const void* route, size_t route_size, const void* body, size_t body_size,
    easy_uds_c_response* response);

/* Error text and code are thread-local and remain valid until the next C API
 * call on the same thread. Functions return zero on success unless noted. */
const char* easy_uds_last_error(void);
int32_t easy_uds_last_error_code(void);
uint32_t easy_uds_c_abi_version(void);

easy_uds_server* easy_uds_server_create(const char* socket_path);
int easy_uds_server_on(easy_uds_server* server, const void* route,
                       size_t route_size, easy_uds_c_handler handler,
                       void* user_data);
int easy_uds_server_on_context(easy_uds_server* server, const void* route,
                               size_t route_size,
                               easy_uds_c_context_handler handler,
                               void* user_data);
int easy_uds_server_run(easy_uds_server* server);
int easy_uds_server_stop(easy_uds_server* server);
void easy_uds_server_destroy(easy_uds_server* server);

easy_uds_client* easy_uds_client_create(const char* socket_path);
easy_uds_client* easy_uds_client_create_with_options(
    const char* socket_path, const easy_uds_client_options* options);
int easy_uds_client_request(easy_uds_client* client, const void* route,
                            size_t route_size, const void* body,
                            size_t body_size, easy_uds_bytes* response_body,
                            int32_t* response_status);
#if defined(_WIN32)
int easy_uds_client_request_handle(
    easy_uds_client* client, const void* route, size_t route_size,
    void* handle, const void* body, size_t body_size,
    easy_uds_bytes* response_body, int32_t* response_status);
#endif
int easy_uds_client_request_idempotent(
    easy_uds_client* client, const void* route, size_t route_size,
    const void* body, size_t body_size, uint32_t max_attempts,
    easy_uds_bytes* response_body, int32_t* response_status);
easy_uds_session* easy_uds_session_create(easy_uds_client* client);
int easy_uds_session_request(easy_uds_session* session, const void* route,
                             size_t route_size, const void* body,
                             size_t body_size, easy_uds_bytes* response_body,
                             int32_t* response_status);
easy_uds_cancellation* easy_uds_cancellation_create(void);
void easy_uds_cancellation_cancel(easy_uds_cancellation* cancellation);
int easy_uds_session_request_cancellable(
    easy_uds_session* session, const easy_uds_cancellation* cancellation,
    const void* route, size_t route_size, const void* body, size_t body_size,
    easy_uds_bytes* response_body, int32_t* response_status);
void easy_uds_cancellation_destroy(easy_uds_cancellation* cancellation);
void easy_uds_session_destroy(easy_uds_session* session);
void easy_uds_bytes_free(easy_uds_bytes* bytes);
void easy_uds_client_destroy(easy_uds_client* client);

#ifdef __cplusplus
}
#endif

#endif /* EASY_UDS_C_API_H */
