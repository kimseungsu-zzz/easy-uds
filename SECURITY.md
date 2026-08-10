# Security Policy

## Supported versions

Security fixes are applied to the current minor release line.

| Version | Supported |
| --- | --- |
| 0.4.x | Yes |
| 0.3.x | No |
| 0.2.x | No |
| 0.1.x | No |

## Reporting a vulnerability

Prefer GitHub private vulnerability reporting for the repository when it is available. If it is not enabled, open a minimal issue asking for a private contact channel and do not include exploit details in the public issue.

Please include the affected version, platform, impact, reproduction conditions, and any suggested mitigation.

## Scope

`easy-uds` is a local IPC transport. It does not provide application-level authentication, encryption, authorization, sandboxing, or a peer-credential policy. Applications must choose socket paths, directory permissions, and socket permissions appropriate to their trust boundary.

The default socket mode is `0600`. The server also uses a same-directory advisory lock file to coordinate easy-uds processes and verifies ownership/inode identity before stale-path cleanup. Unrelated software that ignores the advisory lock is outside that coordination mechanism.
