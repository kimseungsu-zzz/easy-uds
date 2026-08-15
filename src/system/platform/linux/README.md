# Linux platform boundary

This directory owns concrete Linux capability functions. `endpoint.cpp`
implements the contract in `../endpoint.hpp`, containing pathname `AF_UNIX`
endpoint validation and socket lifecycle calls
(`socket`, `connect`, `bind`, `listen`, `accept4`, `unlink`, and `chmod`).
`readiness.cpp` translates the platform-neutral readiness contract to
`epoll_create1`, `epoll_ctl`, `epoll_wait`, and the `eventfd` wakeup
signal/consume pair. Reactor code sees only readiness masks, tokens, and
control operations.
`peer_identity.cpp` contains the connected-peer identity capability and is the
only owner of the Linux `SO_PEERCRED`/`getsockopt` capture.
`descriptor_passing.cpp` contains the descriptor-bearing `sendmsg`/`recvmsg`
operations and ancillary validation. It preserves first-successful-send-only
attachment, close-on-exec reception, fatal malformed/truncated control data,
and the existing one-descriptor/frame ordering rules.
`socket_lifecycle.cpp` owns internal descriptor close/shutdown and fcntl/
setsockopt setup, while `socket_io.cpp` owns raw byte I/O, ordinary gathered
write, and the `SO_ERROR` connect-completion query. Both report raw syscall
results; transport code retains retry, deadline, and public error semantics.
`socket_wait.cpp` owns one synchronous `poll` attempt and translates
`POLLIN`/`POLLOUT`/`POLLERR`/`POLLHUP`/`POLLNVAL` into a small result. It is
separate from the reactor's multi-connection readiness/eventfd contract.
Higher layers keep their existing value/timeout semantics and call these
functions directly; there is no virtual transport or type-erased backend.

Generic error translation remains above these capability seams; this phase does
not change public ErrorCode or system_code semantics.
