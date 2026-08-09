# easy-uds wire protocol

This document describes protocol version 1, used by easy-uds v0.2.x and v0.3.x.

## Transport

- Unix Domain Socket
- `AF_UNIX`
- `SOCK_STREAM`
- One request followed by one response per connection
- Multi-byte integers use network byte order (big-endian)

## Common header

Every request and response begins with exactly 16 bytes:

| Offset | Size | Field | Value |
| ---: | ---: | --- | --- |
| 0 | 4 | Magic | ASCII `EUDS` |
| 4 | 1 | Version | `1` |
| 5 | 1 | Message type | `1` request, `2` response |
| 6 | 2 | Reserved flags | `0` |
| 8 | 4 | Argument 1 | Type-specific, unsigned 32-bit |
| 12 | 4 | Argument 2 | Type-specific, unsigned 32-bit |

Reserved bytes must be zero. A peer may close the connection when the magic, version, type, or flags are invalid.

## Request

For a request header:

- `Argument 1` = route length in bytes
- `Argument 2` = body length in bytes

The header is immediately followed by:

```text
route bytes | body bytes
```

The route must contain at least one byte. No text encoding is imposed by the wire format; route and body are length-delimited and may contain NUL (`0x00`) or newline bytes.

The configured `max_message_size` limits `route_length + body_length`.

## Response

For a response header:

- `Argument 1` = non-negative status code
- `Argument 2` = body length in bytes

The header is immediately followed by the response body.

The configured `max_message_size` limits the response body length.

## Connection lifecycle

A normal exchange is:

```text
client                        server
  |                              |
  |--------- connect ----------->|
  |--------- request ----------->|
  |<-------- response -----------|
  |--------- close --------------|
```

The current protocol does not multiplex requests and does not keep connections alive for additional requests.

## Compatibility

Protocol versioning is explicit in byte 4 of the header. Implementations must not interpret an unknown version as version 1.

v0.1.x used newline-delimited metadata and is not wire-compatible with protocol version 1.

v0.3.x keeps protocol version 1 unchanged and is wire-compatible with v0.2.x. The v0.3 changes are transport-lifetime, timeout, startup-locking, and implementation hardening changes rather than wire-format changes.
