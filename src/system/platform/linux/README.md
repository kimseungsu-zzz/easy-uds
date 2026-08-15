# Linux platform boundary

This directory owns concrete Linux capability functions. `endpoint.*` contains
pathname `AF_UNIX` endpoint validation and socket lifecycle calls
(`socket`, `connect`, `bind`, `listen`, `accept4`, `unlink`, and `chmod`).
`readiness.cpp` translates the platform-neutral readiness contract to
`epoll_create1`, `epoll_ctl`, `epoll_wait`, and the `eventfd` wakeup
signal/consume pair. Reactor code sees only readiness masks, tokens, and
control operations.
Higher layers keep their existing value/timeout semantics and call these
functions directly; there is no virtual transport or type-erased backend.

The remaining peer-identity, descriptor-passing, and error translation
capabilities are intentionally extracted in later small phases.
