# easy-uds wire protocol

This document describes **protocol version 2**, introduced in easy-uds 0.6.0. It is not wire-compatible with protocol version 1 (easy-uds 0.5.x and earlier); a v2 server rejects v1 connections.

## Transport

- Unix Domain Socket
- `AF_UNIX`
- `SOCK_STREAM`
- Multiplexed request/response with per-connection correlation ids (responses may arrive out of order)
- Multi-byte integers use network byte order (big-endian)

## Common header

Every message begins with exactly 20 bytes:

| Offset | Size | Field | Value |
| ---: | ---: | --- | --- |
| 0 | 4 | Magic | ASCII `EUDS` |
| 4 | 1 | Version | `2` |
| 5 | 1 | Message type | See message types below |
| 6 | 2 | Reserved flags | `0` |
| 8 | 4 | Request id | Unsigned 32-bit |
| 12 | 4 | Argument 1 | Type-specific, unsigned 32-bit |
| 16 | 4 | Argument 2 | Type-specific, unsigned 32-bit |

Reserved bytes must be zero. A peer may close the connection when the magic, version, type, or flags are invalid. A v1 header (version byte `1`) is rejected.

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

## Request id and multiplexing

Every request sent on a connection carries a request id chosen by the client. The server echoes it in the response, so a client can have several requests in flight on one connection and correlate each response by id:

```text
client                                  server
  |--- request id=1 --------------------->|
  |--- request id=2 --------------------->|
  |<--- response id=2 --------------------|
  |<--- response id=1 --------------------|
```

Responses may arrive in any order. Stream frames carry the same id as their stream request, keeping concurrent streams distinct. There is no server-side ordering guarantee between concurrent requests.

## Fixed request

For a fixed request header:

- Request id = client-chosen correlation id (nonzero on multiplexed sessions; the one-shot `request()` path uses `0`)
- `Argument 1` = route length in bytes
- `Argument 2` = body length in bytes

The header is immediately followed by:

```text
route bytes | body bytes
```

The route must contain at least one byte. Route and body are length-delimited and may contain NUL (`0x00`) or newline bytes. The configured `max_message_size` limits `route_length + body_length`.

## Fixed response

For a fixed response header:

- Request id = the id of the request being answered
- `Argument 1` = non-negative status code
- `Argument 2` = body length in bytes

The header is immediately followed by the response body. The configured `max_message_size` limits the response body length.

## Chunked stream request

A stream starts with a type `3` header (request id = the stream's id):

- `Argument 1` = route length in bytes (non-zero)
- `Argument 2` = `0`

The route bytes follow immediately. The body is then represented by zero or more type `4` frames. For every chunk header, `Argument 1` is the number of payload bytes that follow (non-zero), `Argument 2` is `0`, and the request id matches the stream's id. A type `5` header (same id, both arguments zero) ends the request. The sum of chunk payload lengths is checked against the receiver's `max_stream_size` unless that option is zero.

```text
stream request(id, route length) | route bytes
stream request chunk(id, N)      | N body bytes
stream request chunk(id, M)      | M body bytes
stream request end(id, 0, 0)
```

Chunk boundaries are transport details, not application record boundaries. A receiver may deliver a wire chunk in several smaller reads.

## Chunked stream response

After the complete request stream, the server sends a type `6` header (request id = the stream's id):

- `Argument 1` = non-negative status code
- `Argument 2` = `0`

Zero or more type `7` chunk frames follow under the same id and length rules as request chunks. A type `8` header with the same id and zero arguments ends the response.

## Connection lifecycle

A regular one-shot exchange:

```text
client                        server
  |                              |
  |--------- connect ----------->|
  |--------- request ----------->|
  |<-------- response -----------|
  |--------- close --------------|
```

A persistent session (`Client::session()`) keeps the connection open and multiplexes fixed requests as shown above. Streams are exclusive per connection: a client must not send another request (fixed or streamed) until the stream response has ended, and `request_stream()` uses its own dedicated connection on the session API. The server will parse only sequential frames from one connection; pipelined fixed requests are supported because the id correlates responses.

A connection ends when:

- the client closes it (the server observes EOF on the next header read);
- either side exceeds a configured timeout (`io_timeout`, server/client `request_timeout`, `stream_timeout`);
- a request's server-side `request_timeout` expires before a worker executed it — the server answers `408` without invoking the handler;
- a stream exceeds the server's `max_concurrent_streams` — the connection is closed, rejecting the stream;
- the server stops.

## Compatibility

Protocol versioning is explicit in byte 4 of the header. Implementations must not interpret an unknown version as version 2. v0.5.x (protocol version 1) used a 16-byte header without a request id and is not interoperable with v2 peers.
