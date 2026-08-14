# Streaming example

Streaming is an opt-in advanced path. First run the fixed-RPC example from
[Getting started](../getting-started/README.md); only use this pair when a
payload should not be retained as one fixed request body.

Build and run in two terminals:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DEASY_UDS_BUILD_EXAMPLES=ON
cmake --build build --target \
  easy_uds_streaming_server_example easy_uds_streaming_client_example
./build/easy_uds_streaming_server_example /tmp/easy-uds-stream.sock
./build/easy_uds_streaming_client_example /tmp/easy-uds-stream.sock
```

The server consumes `/discard` through a reusable buffer. The client produces
2 MiB through a `StreamReader`; it does not expose this callback or its
temporary chunk lifetime in the beginner fixed-RPC example. Streams are
half-duplex: the request reader reaches `0` before response chunks begin, and
the dedicated connection does not multiplex fixed requests.

`stream_chunk_size`, `max_stream_size`, `stream_timeout`, and `io_timeout` are
advanced boundaries. See the [option contracts](../api/options.md) before
changing them.
