# Internals and history

The current development contract is documented in the
[API reference](../api/README.md), [compatibility and platform contract](../api/compatibility.md),
and [platform support matrix](../platform-support.md). These pages describe
the current implementation; the documents under History and Release records
preserve earlier decisions and validation results.

## Current implementation notes

- [Windows backend](windows-backend.md): Winsock AF_UNIX, peer identity,
  HANDLE transfer, and current validation boundary.
- [Source layout](../SOURCE_LAYOUT.md): current code organization and
  platform source selection.
- [User/system dependency audit](user-system-dependencies.md): historical
  dependency inventory with notes about the later C/Python binding layers.
- [Linux dependency audit](linux-dependency-audit.md): Linux syscall
  ownership and original backend extraction record.
- [Error and I/O inventory](error-and-io-inventory.md): concrete platform
  operation ownership and error mapping.

## Historical design and validation

- [0.8 blocker journal](blocker-journal-0.8.md)
- [0.8 Request capabilities](../PERF_0.8_REQUEST_CAPABILITIES.md)
- [0.8 RC performance](../PERF_0.8_RC.md)
- [0.6 experiments](../history/experiments/0.6.md)
- [History index](../history/README.md)
- [0.7.0 release record](../RELEASE_0.7.md)
- [0.7.1 architecture release](../releases/v0.7.1.md)
- [0.8.0 release](../releases/v0.8.0.md)
- [0.8.0-rc.1 candidate](../releases/v0.8.0-rc.1.md)
- [0.9.0 stabilization release](../releases/v0.9.0.md)
- [1.0.0 stable release](../releases/v1.0.0.md)
- [Public API freeze audit](../api/public-api-audit.md)
- [Simple API design audit](../design/simple-api.md)

Historical records describe the scope and evidence available at the time. Use
the current API and platform pages above for present-day support and behavior.
