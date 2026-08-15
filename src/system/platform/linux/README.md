# Linux platform boundary

This directory owns concrete Linux capability functions. `endpoint.*` contains
pathname `AF_UNIX` endpoint validation and socket lifecycle calls
(`socket`, `connect`, `bind`, `listen`, `accept4`, `unlink`, and `chmod`).
Higher layers keep their existing value/timeout semantics and call these
functions directly; there is no virtual transport or type-erased backend.

The remaining readiness/wakeup, peer-identity, descriptor-passing, and error
translation capabilities are intentionally extracted in later small phases.
