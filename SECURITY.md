# Security Policy

## Supported versions

Security fixes are applied to the current minor release line.

| Version | Supported |
| --- | --- |
| 1.1.x | Yes |
| 1.0.x | No |

## Reporting a vulnerability

Prefer GitHub private vulnerability reporting for the repository when it is available. If it is not enabled, open a minimal issue asking for a private contact channel and do not include exploit details in the public issue.

Please include the affected version, platform, impact, reproduction conditions, and any suggested mitigation.

## Scope

`easy-uds` is a local IPC transport. It does not provide encryption,
sandboxing, or an application-defined identity policy. C++ servers can install
`ServerOptions::authorize_request` to enforce application authorization using
the request and available peer identity; the application owns that policy and
must fail closed when required identity data is unavailable. C and Python
handlers do not configure this server-level authorization callback. Applications
must choose socket paths, directory permissions, and socket permissions
appropriate to their trust boundary.

The default socket mode is `0600`. The server also uses a same-directory advisory lock file to coordinate easy-uds processes and verifies ownership/inode identity before stale-path cleanup. Unrelated software that ignores the advisory lock is outside that coordination mechanism.
