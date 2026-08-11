# easy-uds wire protocol

This document describes protocol version 1. The original fixed-size messages are used by easy-uds v0.2.x and later; easy-uds v0.4.x adds the stream frame types described below. easy-uds v0.5.0 keeps protocol version 1 and adds persistent-connection reuse (no new frame types); the connection lifecycle section below describes it.

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
| 5 | 1 | Message type | See message types below |
| 6 | 2 | Reserved flags | `0` |
| 8 | 4 | Argument 1 | Type-specific, unsigned 32-bit |
| 12 | 4 | Argument 2 | Type-specific, unsigned 32-bit |

Reserved bytes must be zero. A peer may close the connection when the magic, version, type, or flags are invalid.

| Type | Value |
| --- | ---: |
| Fixed request | `1` |
| Fixed response | `2` |
| Stream request start | `3` |
| Stream request chunk | `4` |
| Stream request end | `5` |
| Stream response start | `6` |
| Stream response chunk | `7` |
| Stream response end | `8` |

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

## Chunked stream request

A stream starts with a type `3` header:

- `Argument 1` = route length in bytes (non-zero)
- `Argument 2` = `0`

The route bytes follow immediately. The body is then represented by zero or more type `4` frames. For every chunk header:

- `Argument 1` = number of payload bytes following the header (non-zero)
- `Argument 2` = `0`

A type `5` header ends the request body; both arguments must be zero. The sum of chunk payload lengths is checked against the receiver's `max_stream_size` unless that option is zero.

```text
stream request(route length) | route bytes
stream request chunk(N)      | N body bytes
stream request chunk(M)      | M body bytes
stream request end(0, 0)
```

Chunk boundaries are transport details, not application record boundaries. A receiver may deliver a wire chunk in several smaller reads.

## Chunked stream response

After the complete request stream, the server sends a type `6` header:

- `Argument 1` = non-negative status code
- `Argument 2` = `0`

Zero or more type `7` chunk frames follow using the same length rules as request chunks. A type `8` header with both arguments zero ends the response.

The protocol is half-duplex: a client sends the request end frame before waiting for the response start frame. This prevents either side from needing an unbounded staging buffer. `SOCK_STREAM` backpressure provides flow control when the consumer is slower than the producer.

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

A streaming exchange has the same connection lifecycle; only each body is split into explicitly framed pieces. It does not multiplex routes or interleave the response with an unfinished request.

Since v0.5.0, a connection may be kept alive for additional requests (persistent sessions, `Client::session()`):

```text
client                        server
  |                              |
  |--------- connect ----------->|
  |--------- request 1 --------> |
  |<-------- response 1 ---------|
  |--------- request 2 --------> |
  |<-------- response 2 ---------|
  |--------- close --------------|
```

The protocol is strictly lockstep: the server reads the next request header only after the previous response has been written, and a client must wait for each response before sending the next request. There is no request multiplexing or pipelining. Every frame type described above (fixed request/response, or a stream request/response sequence) may appear at any exchange boundary.

A connection ends when:

- the client closes it (the server observes EOF on the next header read);
- either side exceeds a configured timeout (`io_timeout`, server/client `request_timeout`, `stream_timeout`);
- a request is sent to a serialized route — that exchange is served through the exclusive executor and the connection is closed after its response;
- the server-side persistent-session limit (`max_persistent_sessions`) is reached — the connection is closed after its last response, so the client's next request fails explicitly;
- the server stops.

The current protocol does not multiplex requests, and no part of a response may be sent before the complete request (including its stream end frame) has been consumed.

## Compatibility

Protocol versioning is explicit in byte 4 of the header. Implementations must not interpret an unknown version as version 1.

v0.1.x used newline-delimited metadata and is not wire-compatible with protocol version 1.

v0.3.x keeps protocol version 1 unchanged and is wire-compatible with v0.2.x. v0.4.x retains those fixed request/response types and adds types 3 through 8. Therefore regular `request()` calls remain compatible across v0.2+ peers, while `request_stream()` requires v0.4+ on both sides. Unknown message types must not be interpreted as fixed messages.
