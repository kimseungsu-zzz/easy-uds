# Python binding

The `easy_uds` package is a standard-library-only `ctypes` wrapper over the
installed C ABI. It provides fixed-request `Client` and `Server` classes,
multiplexed Sessions, cancellable protocol v3 requests, and preserves request
and response bodies as `bytes`. Register `Server.on_context()` to inspect the
request id, live cooperative stop signal, available POSIX or Windows peer identity,
and (on Windows) a callback-scoped received HANDLE. `Client.request_handle()`
passes a caller-owned Win32 HANDLE.

Install the native easy-uds 1.1.0 shared library first; this Python package
contains the binding, not the native library. Then install the binding from
PyPI:

```console
python -m pip install easy-uds
```

Set `EASY_UDS_LIBRARY` to the shared library path when the platform loader
cannot locate it. Construct `Client` with `protocol_version=3` to use
`Session.request(..., cancellation=source)`; cancel the source from another
thread. Idempotent retry remains an explicit one-shot method. Streaming remains
C++-only. See the [native library installation and platform support guide](https://github.com/kimseungsu-zzz/easy-uds/blob/v1.1.0/docs/platform-support.md).
