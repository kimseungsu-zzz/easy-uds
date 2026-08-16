# Troubleshooting

## The client reports `unavailable`

Check that the server is running, that both processes use the same pathname,
and that the containing directory permits access. A refused pathname is only
removed when easy-uds can prove that it is an owned stale socket; a regular
file or another user's socket is not removed.

## A `Session` is broken

`Session::status()` is an observation, not a heartbeat. An I/O error, timeout,
protocol error, or peer close permanently moves that connection to
`SessionStatus::broken`. Create a fresh session explicitly after deciding that
the operation is safe to retry; easy-uds does not replay requests.

## A Simple request throws `ResponseError`

The transport succeeded but the server returned a non-200 application status.
Read `status()` and `body()` from the exception. Transport, protocol, timeout,
and closed-connection failures remain `easy_uds::Error` instead.

## A handler does not run before its deadline

Queue time counts toward the configured request deadline. `LatestWins` may
replace an older pending request and `RejectIfBusy` may answer immediately;
neither interrupts a handler that has already started. Use `RequestContext` for
cooperative stop observation.

## POSIX capabilities are unavailable on Windows

Peer credentials and `SCM_RIGHTS` descriptor passing are deliberately POSIX-only
in 1.0. Include `<easy_uds/posix.hpp>` only on a POSIX build and use
`BorrowedFd::duplicate()` when a received descriptor must outlive its handler.

For package or build failures, verify the installed `easy_uds::easy_uds` target
and run the matching compiler's public-header consumer before changing runtime
options.
