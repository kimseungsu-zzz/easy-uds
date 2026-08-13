# File descriptor passing

easy-uds can attach one Linux file descriptor to a one-shot fixed request. The
API distinguishes the client's non-owning input from the server's owning
request value:

```cpp
const easy_uds::Response response =
    client.request_fd("config/load", easy_uds::borrow_fd(fd));

server.on("config/load", [](const easy_uds::Request& request) {
    if (!request.fd.valid()) {
        return easy_uds::Response{400, "descriptor required"};
    }
    const int fd = request.fd.get();
    // Read from fd while the handler is running.
    return easy_uds::Response{200, "ok"};
});
```

## Ownership and lifetime

- `BorrowedFd` is a non-owning view. `Client::request_fd()` never closes or
  consumes it; the caller must keep it open until the call finishes.
- `Request::fd` is an `OwnedFd`. It owns the descriptor received through
  `SCM_RIGHTS` and closes it automatically when the request is destroyed.
- `OwnedFd` is move-only. Moving transfers ownership and leaves the source
  empty. It occupies one `int` and performs no allocation.
- A handler that needs the descriptor after returning must call
  `request.fd.duplicate()` and move the returned `OwnedFd` into longer-lived
  storage.
- `duplicate()` and the descriptor sent by `SCM_RIGHTS` refer to the same open
  file description as the original. File offset and status flags are shared.

`OwnedFd::adopt(fd)` is available when code already owns a raw descriptor and
wants RAII cleanup. After adoption, that code must not close the raw value or
adopt it a second time. `release()` performs the inverse transfer and makes the
caller responsible for closing the returned descriptor.

## Scope and errors

Descriptor passing is supported only by one-shot fixed requests. It is not
available on persistent `Session` requests or streams. The v2 wire protocol
allows exactly one descriptor and rejects the FD flag on nonzero request IDs.

- Passing an empty `BorrowedFd` throws `std::invalid_argument` before connect.
- `OwnedFd::duplicate()` throws `Error`. An empty value is classified as
  `invalid_request` and preserves `EBADF` in `system_code()`.
- Connection, timeout, and protocol errors follow the normal one-shot request
  behavior. The caller still owns its original descriptor after any error.

## Thread safety and performance

Separate wrapper values may be used by separate threads. Concurrent operations
that mutate the same `OwnedFd` object require external synchronization, and
access to the underlying file description follows the descriptor type's normal
Linux semantics.

The wrappers add no allocation, lock, or syscall to ordinary access. Passing a
descriptor still requires the existing one-shot connection and `sendmsg` /
`recvmsg` ancillary-data path; `duplicate()` adds one `fcntl` syscall only when
the handler explicitly requests a longer lifetime.

See also [wire protocol](../PROTOCOL.md) and the
[0.6 to 0.7 migration guide](../migration/0.6-to-0.7.md).
