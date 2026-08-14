# Session state

`Session` owns one persistent, multiplexed connection for fixed requests.
`status()` exposes a small snapshot of that connection's observed state, and
`valid()` is the convenient check for the common case:

```cpp
easy_uds::Session session = client.session();

if (session.valid()) {
    const auto response = session.request("/encoder/read");
}

switch (session.status()) {
case easy_uds::SessionStatus::active:
    break;
case easy_uds::SessionStatus::broken:
    session = client.session(); // explicit application decision
    break;
case easy_uds::SessionStatus::moved_from:
    // Programming/lifecycle error: use the move destination instead.
    break;
}
```

## States

| `SessionStatus` | Meaning |
|---|---|
| `active` | No fixed-connection failure has been observed yet |
| `broken` | An I/O, timeout, peer-close, or protocol failure was observed; the fixed connection cannot be reused |
| `moved_from` | Ownership was transferred to another `Session` object |

`valid()` is exactly `status() == SessionStatus::active`.

`active` is deliberately not named `connected`. The query does not perform a
syscall, heartbeat, or round trip, and another thread or the peer can break the
connection immediately after the snapshot. The result therefore must not be
used as a check-then-act guarantee. Call `request()` and handle its `Error`
even when `valid()` just returned true.

## Transitions and retry

The fixed-request state only moves in these directions:

```text
active ---- observed failure ----> broken

source:      active or broken ---- object move ----> moved_from
destination:                       receives the source's prior state
```

There is no transition from `broken` back to `active`. easy-uds does not
reconnect or replay a request implicitly. If retry is safe for the
application, explicitly create a new Session after inspecting the request
error. A moved-from object also stays moved-from unless a new Session is move
assigned into it.

The request that observes a failure receives the specific `ErrorCode`, such
as `timeout`, `closed`, or `protocol`. Later fixed requests on the same object
receive `ErrorCode::closed`. Calls on a moved-from object remain a local
lifecycle error and throw `std::logic_error`.

`request_stream()` uses a separate one-shot connection. Its success or
failure does not change the persistent fixed-request state.

## Thread safety, lifetime, and cost

`status()` and `valid()` may run concurrently with any number of
`Session::request()` calls. They are lock-free atomic snapshots and perform no
allocation, syscall, or I/O. As with the other Session operations, the same
`Session` object must not be moved or destroyed concurrently with a call.

The status is meaningful only for the lifetime of that Session object. Moving
the object transfers the connection and its current state to the destination.

See the [error model](errors.md) for failure meanings and the
[0.6 to 0.7 migration guide](../migration/0.6-to-0.7.md) for source changes.
