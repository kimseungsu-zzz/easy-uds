# Error model

easy-uds uses one operational exception type and a small set of stable meaning
classes. It does not replace detailed Linux errors or create an exception class
for every failure:

```cpp
try {
    const auto response = client.request("encoder/read");
    use(response);
} catch (const easy_uds::Error& error) {
    if (error.code() == easy_uds::ErrorCode::timeout) {
        // The operation did not finish before its configured deadline.
    }

    if (error.system_code()) {
        log_errno(error.system_code()); // e.g. ETIMEDOUT or ECONNRESET
    }
    log_message(error.what());          // operation plus concrete OS detail
}
```

## Meaning classes

| `ErrorCode` | Meaning |
|---|---|
| `system` | OS failure without a more useful stable class |
| `timeout` | Connect, inactivity, request, or stream deadline expired |
| `closed` | Peer close, reset, broken pipe, or an already-broken Session |
| `protocol` | Invalid magic, version, type, flags, framing, or correlation ID |
| `busy` | Address/resource is already owned or temporarily busy |
| `too_large` | A peer response exceeds the configured receive limit |
| `invalid_request` | An operation uses an invalid easy-uds resource state |
| `unavailable` | Socket path/service is missing, unreachable, or refusing connections |
| `cancelled` | Operation was explicitly cancelled; reserved for cancellation APIs |

The enum is intentionally small. Applications should not infer a specific
Linux cause from it; use `system_code()` when the distinction matters.

## Three views of one error

- `error.kind()` returns `ErrorCode` directly.
- `error.code()` is the inherited `std::system_error` code in the `easy_uds`
  category. It can be compared directly with an `ErrorCode`.
- `error.system_code()` returns the original OS `std::error_code`. It is empty
  for semantic failures that have no corresponding `errno`.
- `error.what()` contains the operation, OS category/message when present, and
  the easy-uds meaning.

`Error` derives from `std::system_error`, so existing broad catch blocks remain
valid:

```cpp
catch (const std::system_error& error) {
    // Also catches easy_uds::Error.
}
```

Code that needs `errno` must catch `easy_uds::Error` and read
`system_code()`. In 0.7, inherited `code()` deliberately represents the stable
easy-uds meaning rather than the platform-specific cause.

## Standard C++ exceptions remain meaningful

Errors detectable before I/O keep standard C++ contract exceptions:

- `std::invalid_argument`: empty route, invalid option, empty borrowed FD
- `std::length_error`: locally supplied request/stream exceeds its configured
  send limit
- `std::logic_error`: invalid object lifecycle, such as using a moved-from
  Session or calling `Server::run()` twice

Peer data that exceeds a receive limit is instead `ErrorCode::too_large`, and
invalid peer framing is `ErrorCode::protocol`. This distinction tells callers
whether their own input was rejected or the connection failed while decoding
remote data.

## Session and retry semantics

An I/O, timeout, or protocol failure permanently breaks the fixed-request
connection of a `Session`. The request that observes the cause receives its
specific `Error`; later calls receive `ErrorCode::closed` without a system code
because they perform no new OS operation.

easy-uds never reconnects or replays a request implicitly. An application may
create a new Session after inspecting the error, but it must decide whether the
operation is safe to retry.

See the [Session state reference](session.md) for the public state snapshot,
one-way transitions, and concurrent-observation contract.

## Thread safety, lifetime, and performance

`Error`, `ErrorCode`, and copied `std::error_code` values have ordinary value
semantics. An exception object remains valid for the lifetime of the catch
scope or any copy made by the application. The error category is a single
library-owned object, including across shared-library boundaries.

The model adds no allocation, syscall, lock, or branch to successful request
paths. Classification and message construction occur only while creating an
exception.

See the [0.6 to 0.7 migration guide](../migration/0.6-to-0.7.md) for changed
catch behavior.
