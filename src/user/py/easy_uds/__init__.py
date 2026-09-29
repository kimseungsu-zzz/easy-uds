"""Optional ctypes binding for the easy-uds C ABI.

Set EASY_UDS_LIBRARY when the native library cannot be found by the platform
loader. This module depends only on Python's standard library.
"""

from __future__ import annotations

import ctypes
import ctypes.util
import os
import threading
from dataclasses import dataclass, field
from typing import Callable


class EasyUDSError(RuntimeError):
    def __init__(self, message: str, code: int = -1):
        super().__init__(message)
        self.code = code


@dataclass(frozen=True)
class RequestContext:
    request_id: int
    peer_pid: int | None
    peer_uid: int | None
    peer_gid: int | None
    peer_sid: str | None
    received_handle: int | None
    _native_stop_query: Callable[[], bool] | None = field(
        default=None, repr=False, compare=False)

    @property
    def stop_requested(self) -> bool:
        """Return the live cooperative stop state for this callback."""
        return self._native_stop_query() if self._native_stop_query else False


class _Bytes(ctypes.Structure):
    _fields_ = [("data", ctypes.c_void_p), ("size", ctypes.c_size_t)]


class _ClientOptions(ctypes.Structure):
    _fields_ = [("struct_size", ctypes.c_uint32),
                ("protocol_version", ctypes.c_uint32)]


class _NativeRequestContext(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("request_id", ctypes.c_uint32),
        ("peer_pid", ctypes.c_int64),
        ("peer_uid", ctypes.c_uint64),
        ("peer_gid", ctypes.c_uint64),
        ("peer_sid", ctypes.c_char_p),
        ("peer_identity_present", ctypes.c_uint32),
        ("received_handle", ctypes.c_void_p),
        ("stop_requested", ctypes.c_uint32),
        ("is_stop_requested", ctypes.c_void_p),
        ("opaque_context", ctypes.c_void_p),
    ]


class _Response(ctypes.Structure):
    pass


_Handler = ctypes.CFUNCTYPE(
    ctypes.c_int,
    ctypes.c_void_p,
    ctypes.c_void_p,
    ctypes.c_size_t,
    ctypes.c_void_p,
    ctypes.c_size_t,
    ctypes.POINTER(_Response),
)
_ContextHandler = ctypes.CFUNCTYPE(
    ctypes.c_int, ctypes.c_void_p, ctypes.POINTER(_NativeRequestContext),
    ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t,
    ctypes.POINTER(_Response),
)
_Release = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p)
_Response._fields_ = [
    ("status", ctypes.c_int32),
    ("body", ctypes.c_void_p),
    ("body_size", ctypes.c_size_t),
    ("release_body", _Release),
    ("release_owner", ctypes.c_void_p),
]


def _load_library() -> ctypes.CDLL:
    candidates = []
    configured = os.environ.get("EASY_UDS_LIBRARY")
    if configured:
        candidates.append(configured)
    located = ctypes.util.find_library("easy-uds")
    if located:
        candidates.append(located)
    for candidate in candidates:
        try:
            return ctypes.CDLL(candidate)
        except OSError:
            pass
    raise EasyUDSError(
        "Could not load easy-uds. Set EASY_UDS_LIBRARY to the native library path."
    )


_lib = _load_library()
_lib.easy_uds_c_abi_version.restype = ctypes.c_uint32
if _lib.easy_uds_c_abi_version() != 1:
    raise EasyUDSError("Unsupported easy-uds C ABI version")

_lib.easy_uds_last_error.restype = ctypes.c_char_p
_lib.easy_uds_last_error_code.restype = ctypes.c_int32
_lib.easy_uds_server_create.argtypes = [ctypes.c_char_p]
_lib.easy_uds_server_create.restype = ctypes.c_void_p
_lib.easy_uds_server_on.argtypes = [
    ctypes.c_void_p,
    ctypes.c_void_p,
    ctypes.c_size_t,
    _Handler,
    ctypes.c_void_p,
]
_lib.easy_uds_server_on_context.argtypes = [
    ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, _ContextHandler,
    ctypes.c_void_p,
]
_lib.easy_uds_server_run.argtypes = [ctypes.c_void_p]
_lib.easy_uds_server_stop.argtypes = [ctypes.c_void_p]
_lib.easy_uds_server_destroy.argtypes = [ctypes.c_void_p]
_lib.easy_uds_client_create.argtypes = [ctypes.c_char_p]
_lib.easy_uds_client_create.restype = ctypes.c_void_p
_lib.easy_uds_client_create_with_options.argtypes = [
    ctypes.c_char_p, ctypes.POINTER(_ClientOptions),
]
_lib.easy_uds_client_create_with_options.restype = ctypes.c_void_p
_lib.easy_uds_client_request.argtypes = [
    ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p,
    ctypes.c_size_t, ctypes.POINTER(_Bytes), ctypes.POINTER(ctypes.c_int32),
]
if os.name == "nt":
    _lib.easy_uds_client_request_handle.argtypes = [
        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p,
        ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(_Bytes),
        ctypes.POINTER(ctypes.c_int32),
    ]
_lib.easy_uds_client_request_idempotent.argtypes = [
    ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p,
    ctypes.c_size_t, ctypes.c_uint32, ctypes.POINTER(_Bytes),
    ctypes.POINTER(ctypes.c_int32),
]
_lib.easy_uds_bytes_free.argtypes = [ctypes.POINTER(_Bytes)]
_lib.easy_uds_client_destroy.argtypes = [ctypes.c_void_p]
_lib.easy_uds_session_create.argtypes = [ctypes.c_void_p]
_lib.easy_uds_session_create.restype = ctypes.c_void_p
_lib.easy_uds_session_request.argtypes = [
    ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p,
    ctypes.c_size_t, ctypes.POINTER(_Bytes), ctypes.POINTER(ctypes.c_int32),
]
_lib.easy_uds_cancellation_create.restype = ctypes.c_void_p
_lib.easy_uds_cancellation_cancel.argtypes = [ctypes.c_void_p]
_lib.easy_uds_session_request_cancellable.argtypes = [
    ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
    ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(_Bytes),
    ctypes.POINTER(ctypes.c_int32),
]
_lib.easy_uds_cancellation_destroy.argtypes = [ctypes.c_void_p]
_lib.easy_uds_session_destroy.argtypes = [ctypes.c_void_p]


def _check(result: int) -> None:
    if result != 0:
        message = _lib.easy_uds_last_error()
        if isinstance(message, bytes):
            message = message.decode("utf-8", errors="replace")
        raise EasyUDSError(message or "easy-uds operation failed",
                           _lib.easy_uds_last_error_code())


def _bytes_argument(value: bytes):
    if not value:
        return None, None
    owner = ctypes.create_string_buffer(value)
    return owner, ctypes.cast(owner, ctypes.c_void_p)


class Client:
    """Thread-safe one-shot fixed-request client."""

    def __init__(self, socket_path: str | os.PathLike[str], protocol_version: int = 2):
        options = _ClientOptions(ctypes.sizeof(_ClientOptions), protocol_version)
        self._handle = _lib.easy_uds_client_create_with_options(
            os.fsencode(socket_path), ctypes.byref(options))
        if not self._handle:
            _check(-1)

    def request(self, route: bytes | str, body: bytes = b"") -> tuple[int, bytes]:
        route = route.encode() if isinstance(route, str) else bytes(route)
        body = bytes(body)
        route_owner, route_ptr = _bytes_argument(route)
        body_owner, body_ptr = _bytes_argument(body)
        response = _Bytes()
        status = ctypes.c_int32()
        _check(_lib.easy_uds_client_request(
            self._handle, route_ptr, len(route), body_ptr, len(body),
            ctypes.byref(response), ctypes.byref(status)))
        try:
            payload = ctypes.string_at(response.data, response.size)
            return status.value, payload
        finally:
            _lib.easy_uds_bytes_free(ctypes.byref(response))

    def request_handle(
        self, route: bytes | str, handle: int, body: bytes = b""
    ) -> tuple[int, bytes]:
        if os.name != "nt":
            raise NotImplementedError("HANDLE passing is only available on Windows")
        route = route.encode() if isinstance(route, str) else bytes(route)
        body = bytes(body)
        route_owner, route_ptr = _bytes_argument(route)
        body_owner, body_ptr = _bytes_argument(body)
        response = _Bytes()
        status = ctypes.c_int32()
        _check(_lib.easy_uds_client_request_handle(
            self._require_handle(), route_ptr, len(route), ctypes.c_void_p(handle),
            body_ptr, len(body), ctypes.byref(response), ctypes.byref(status)))
        try:
            return status.value, ctypes.string_at(response.data, response.size)
        finally:
            _lib.easy_uds_bytes_free(ctypes.byref(response))

    def request_idempotent(
        self, route: bytes | str, body: bytes = b"", max_attempts: int = 3
    ) -> tuple[int, bytes]:
        route = route.encode() if isinstance(route, str) else bytes(route)
        body = bytes(body)
        route_owner, route_ptr = _bytes_argument(route)
        body_owner, body_ptr = _bytes_argument(body)
        response = _Bytes()
        status = ctypes.c_int32()
        _check(_lib.easy_uds_client_request_idempotent(
            self._handle, route_ptr, len(route), body_ptr, len(body),
            max_attempts, ctypes.byref(response), ctypes.byref(status)))
        try:
            return status.value, ctypes.string_at(response.data, response.size)
        finally:
            _lib.easy_uds_bytes_free(ctypes.byref(response))

    def session(self) -> "Session":
        return Session(_lib.easy_uds_session_create(self._require_handle()))

    def _require_handle(self):
        if not self._handle:
            raise EasyUDSError("client is closed")
        return self._handle

    def close(self) -> None:
        if self._handle:
            _lib.easy_uds_client_destroy(self._handle)
            self._handle = None

    def __enter__(self) -> "Client":
        return self

    def __exit__(self, *_exc) -> None:
        self.close()


class CancellationSource:
    """Thread-safe cancellation signal for a protocol v3 Session request."""

    def __init__(self):
        self._handle = _lib.easy_uds_cancellation_create()
        if not self._handle:
            _check(-1)

    def cancel(self) -> None:
        if self._handle:
            _lib.easy_uds_cancellation_cancel(self._handle)

    def close(self) -> None:
        if self._handle:
            _lib.easy_uds_cancellation_destroy(self._handle)
            self._handle = None

    def __enter__(self) -> "CancellationSource":
        return self

    def __exit__(self, *_exc) -> None:
        self.close()


class Session:
    """Persistent multiplexed fixed-request session."""

    def __init__(self, handle):
        self._handle = handle
        if not handle:
            _check(-1)

    def request(
        self, route: bytes | str, body: bytes = b"",
        cancellation: CancellationSource | None = None,
    ) -> tuple[int, bytes]:
        route = route.encode() if isinstance(route, str) else bytes(route)
        body = bytes(body)
        route_owner, route_ptr = _bytes_argument(route)
        body_owner, body_ptr = _bytes_argument(body)
        response = _Bytes()
        status = ctypes.c_int32()
        if cancellation is None:
            result = _lib.easy_uds_session_request(
                self._handle, route_ptr, len(route), body_ptr, len(body),
                ctypes.byref(response), ctypes.byref(status))
        else:
            if not cancellation._handle:
                raise EasyUDSError("cancellation source is closed")
            result = _lib.easy_uds_session_request_cancellable(
                self._handle, cancellation._handle, route_ptr, len(route),
                body_ptr, len(body), ctypes.byref(response), ctypes.byref(status))
        _check(result)
        try:
            return status.value, ctypes.string_at(response.data, response.size)
        finally:
            _lib.easy_uds_bytes_free(ctypes.byref(response))

    def close(self) -> None:
        if self._handle:
            _lib.easy_uds_session_destroy(self._handle)
            self._handle = None

    def __enter__(self) -> "Session":
        return self

    def __exit__(self, *_exc) -> None:
        self.close()


class Server:
    """Fixed-request server; callbacks receive and return binary ``bytes``."""

    def __init__(self, socket_path: str | os.PathLike[str]):
        self._handle = _lib.easy_uds_server_create(os.fsencode(socket_path))
        if not self._handle:
            _check(-1)
        self._callbacks: list[object] = []
        self._response_bodies: dict[int, ctypes.Array] = {}
        self._next_owner = 1
        self._lock = threading.Lock()
        self._release_callback = _Release(self._release_body)
        self._runner: threading.Thread | None = None

    def _release_body(self, owner, _body) -> None:
        with self._lock:
            self._response_bodies.pop(int(owner or 0), None)

    def on(self, route: bytes | str, handler: Callable[[bytes, bytes], bytes | tuple[int, bytes]]) -> None:
        route = route.encode() if isinstance(route, str) else bytes(route)
        route_owner, route_ptr = _bytes_argument(route)

        def invoke(_user, c_route, route_size, c_body, body_size, output):
            response = output.contents
            try:
                request_route = ctypes.string_at(c_route, route_size)
                request_body = ctypes.string_at(c_body, body_size)
                result = handler(request_route, request_body)
                status, body = result if isinstance(result, tuple) else (200, result)
                body = bytes(body)
                response.status = int(status)
                response.body_size = len(body)
                response.release_body = self._release_callback
                if body:
                    owner = ctypes.create_string_buffer(body)
                    with self._lock:
                        owner_id = self._next_owner
                        self._next_owner += 1
                        self._response_bodies[owner_id] = owner
                    response.body = ctypes.cast(owner, ctypes.c_void_p)
                    response.release_owner = owner_id
                else:
                    response.body = None
                    response.release_owner = None
                return 0
            except BaseException as error:
                body = str(error).encode("utf-8", errors="replace")
                owner = ctypes.create_string_buffer(body)
                with self._lock:
                    owner_id = self._next_owner
                    self._next_owner += 1
                    self._response_bodies[owner_id] = owner
                response.status = 500
                response.body = ctypes.cast(owner, ctypes.c_void_p)
                response.body_size = len(body)
                response.release_body = self._release_callback
                response.release_owner = owner_id
                return 0

        callback = _Handler(invoke)
        _check(_lib.easy_uds_server_on(self._handle, route_ptr, len(route),
                                       callback, None))
        self._callbacks.append(callback)

    def on_context(
        self, route: bytes | str,
        handler: Callable[[RequestContext, bytes, bytes], bytes | tuple[int, bytes]],
    ) -> None:
        """Register a handler that can inspect peer identity and stop state."""
        route = route.encode() if isinstance(route, str) else bytes(route)
        route_owner, route_ptr = _bytes_argument(route)

        def invoke(_user, native_context, c_route, route_size, c_body,
                   body_size, output):
            response = output.contents
            try:
                native = native_context.contents
                context = RequestContext(
                    request_id=native.request_id,
                    peer_pid=native.peer_pid if native.peer_pid >= 0 else None,
                    peer_uid=(native.peer_uid
                              if native.peer_uid != (1 << 64) - 1 else None),
                    peer_gid=(native.peer_gid
                              if native.peer_gid != (1 << 64) - 1 else None),
                    peer_sid=(native.peer_sid.decode("utf-8", errors="replace")
                              if native.peer_sid else None),
                    received_handle=(int(native.received_handle)
                                     if native.received_handle else None),
                    _native_stop_query=(
                        lambda: bool(ctypes.CFUNCTYPE(
                            ctypes.c_int, ctypes.c_void_p)(
                                native.is_stop_requested)(native.opaque_context))
                    ),
                )
                result = handler(context, ctypes.string_at(c_route, route_size),
                                 ctypes.string_at(c_body, body_size))
                status, body = result if isinstance(result, tuple) else (200, result)
                body = bytes(body)
                response.status = int(status)
                response.body_size = len(body)
                response.release_body = self._release_callback
                if body:
                    owner = ctypes.create_string_buffer(body)
                    with self._lock:
                        owner_id = self._next_owner
                        self._next_owner += 1
                        self._response_bodies[owner_id] = owner
                    response.body = ctypes.cast(owner, ctypes.c_void_p)
                    response.release_owner = owner_id
                else:
                    response.body = None
                    response.release_owner = None
                return 0
            except BaseException as error:
                body = str(error).encode("utf-8", errors="replace")
                owner = ctypes.create_string_buffer(body)
                with self._lock:
                    owner_id = self._next_owner
                    self._next_owner += 1
                    self._response_bodies[owner_id] = owner
                response.status = 500
                response.body = ctypes.cast(owner, ctypes.c_void_p)
                response.body_size = len(body)
                response.release_body = self._release_callback
                response.release_owner = owner_id
                return 0

        callback = _ContextHandler(invoke)
        _check(_lib.easy_uds_server_on_context(
            self._handle, route_ptr, len(route), callback, None))
        self._callbacks.append(callback)

    def run(self) -> None:
        _check(_lib.easy_uds_server_run(self._handle))

    def run_background(self) -> threading.Thread:
        if self._runner is not None and self._runner.is_alive():
            raise RuntimeError("server is already running")
        self._runner = threading.Thread(target=self.run, daemon=True)
        self._runner.start()
        return self._runner

    def stop(self) -> None:
        if self._handle:
            _check(_lib.easy_uds_server_stop(self._handle))

    def close(self) -> None:
        if self._handle:
            self.stop()
            if self._runner is not None:
                self._runner.join()
            _lib.easy_uds_server_destroy(self._handle)
            self._handle = None

    def __enter__(self) -> "Server":
        return self

    def __exit__(self, *_exc) -> None:
        self.close()


__all__ = ["CancellationSource", "Client", "EasyUDSError", "RequestContext",
           "Server", "Session"]
