# 0.8 blocker journal

## B-WIN-001 — Windows compiler/runtime gate unavailable locally

- **Found:** Windows backend source-set milestone.
- **Exact issue:** The development environment has no MSVC, clang-cl, or
  Windows runner, so the Winsock AF_UNIX source set cannot be compiled or
  executed locally.
- **Affected work:** Windows library compile, Core Server/Client/Session
  runtime, package consumer, lifecycle and stress validation.
- **Does not block:** public-header cleanup, Linux regression tests, CMake
  source selection, architecture guards, Windows source implementation, CI
  workflow, unsupported-capability documentation, and Linux performance work.
- **Attempted mitigation:** Added a dedicated `windows-core` GitHub Actions
  job, a Windows-only public-header smoke target, explicit Windows source
  lists, and concrete Winsock AF_UNIX capability files.
- **Resolution for this candidate:** The full static/shared Windows Core,
  Session, streaming, Simple, lifecycle, and installed-package matrix passed
  in [Actions run 31916904359](https://github.com/kimseungsu-zzz/easy-uds/actions/runs/31916904359).
  Local MSVC remains unavailable, so every later candidate must repeat the
  hosted gate.

## B-WIN-002 — Windows resource/identity capabilities intentionally excluded

- **Scope:** `SCM_RIGHTS`/HANDLE passing, Linux POSIX peer credentials, and
  SID/token authentication are not part of the initial 0.8 backend.
- **Reason:** These are not semantic equivalents and must not be hidden behind
  a generic handle or fake `PeerCredentials` value.
- **Independent work completed:** Windows fixed RPC, common Session framing,
  Simple API headers, and package assembly can proceed without these
  capabilities.
- **Deferred:** A separate 0.9/1.0 capability design after Windows transport
  and public identity semantics are stable.
