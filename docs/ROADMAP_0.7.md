# 0.7 roadmap — Easy by default, explicit when advanced

0.7의 목표는 0.6.4에서 확정한 성능·안정성 경로를 유지하면서 easy-uds의
사용성과 public API 정체성을 완성하는 것이다.

> 초보자는 `Server → on() → run()`과 `Client → request()`만 알면 되고,
> 고급 사용자는 ownership, deadline, queue, memory, diagnostics를 소스 코드
> 없이 정확히 이해하고 제어할 수 있어야 한다.

새 성능 실험은 0.7 범위가 아니다. 0.6에서 채택·기각한 실험과 benchmark는
보존하며, 0.7의 모든 추상화는 0.6.4 baseline에 대한 regression gate를 통과해야
한다.

## 호환성 원칙

- C++ API 정리만으로 protocol version을 올리지 않는다.
- protocol v2 header와 wire semantics로 자연스럽게 표현할 수 있는 동안 v2를
  유지한다.
- cancellation/deadline/priority metadata, response flags, multiple-FD semantics,
  full-duplex streaming 또는 header/message layout 변경이 필요하면 extension을
  누적하지 않는다. 먼저 `docs/internals/protocol-v3.md`에서 semantics와 migration을
  동결한 뒤 protocol v3를 구현한다.
- 자동 retry는 제공하지 않는다. reconnect는 쉽게 만들되 non-idempotent request의
  재실행 여부는 호출자가 결정한다.
- 0.7에서 source-breaking API 변경은 허용하지만
  `docs/migration/0.6-to-0.7.md`에 old/new/reason을 남긴다.

## 성능·안정성 회귀 게이트

고정 기준선은 `v0.6.4`와 `docs/PERF_0.6.md`이다. 동일 호스트·동일 빌드·동일
workload의 반복 중앙값으로 비교한다.

| 지표 | 기본 허용 범위 | 초과 시 |
|---|---:|---|
| Session/one-shot p50 | +5% 이내 | 설계 재검토 |
| p99 / p99.9 | +10% 이내 | 원인 분리 A/B 후 reject 또는 수정 |
| throughput | -5% 이내 | hot-path abstraction 제거 검토 |
| CPU-s / 1M requests | +5% 이내 | allocation/lock/syscall 계측 |
| warm Session allocation | 0/request 유지 | release 차단 |
| RSS / retained request bytes | 설정된 의미와 일치 | release 차단 |

Hosted runner 절대값은 보장값이 아니다. 같은 runner 안의 A/B와 native x86_64,
ARM64 결과를 함께 판정한다. ASan/UBSan, TSan, protocol/session fuzz, stress,
static/shared package consumer는 모든 phase의 필수 gate다.

## Phase 1 — Cleanup / correctness

- [x] `max_total_inflight_bytes` strict semantics
  - header 검증 직후, parser buffer 할당 전에 선언 route+body 크기 예약
  - partial parsing + queued + executing request가 하나의 logical-byte cap 공유
  - 예산 부족 peer만 `EPOLLIN` pause, partial close/completion 후 global resume
  - strict mode에서 worker continuation이 reactor admission을 우회하지 않음
  - header-only partial frame과 cross-connection resume 회귀 테스트
- [x] FD ownership을 타입으로 표현
  - `BorrowedFd`/`OwnedFd` 또는 동등한 move-only 설계
  - `valid()`, `get()`, `duplicate()`와 handler lifetime 문서화
  - raw descriptor 이중 소유처럼 보이는 수동 `fd = -1` 제거
- [x] error model 설계
  - 작은 `ErrorCode`/category와 구체적인 human-readable message 병행
  - timeout, closed, protocol, busy, too-large, invalid-request 분류
  - OS `errno`와 `std::system_error` 정보 손실 금지
- [x] Session 상태 API
  - lock-free `status()`/`valid()`로 `active`, `broken`, `moved_from` 조회
  - `active`는 liveness probe가 아닌 관찰 시점 snapshot으로 문서화
  - reconnect helper와 implicit retry/replay 없이 새 Session 생성을 명시적 결정으로 유지
- [x] 기능 기준 directory 재배치
  - public umbrella include는 그대로 유지
  - 20줄 파일 남발 없이 client/session, server/lifecycle/routing,
    protocol, detail utility를 책임별로 분리

## Phase 2 — Public API foundation

- [x] 현재 public API header 분할
  - `client.hpp`, `session.hpp`, `server.hpp`, `request.hpp`, `response.hpp`
  - `stream.hpp`, `options.hpp`, `error.hpp`, `fd.hpp`, `version.hpp`
  - `easy_uds.hpp`는 위 header의 umbrella 역할만 수행
- [x] `request_context.hpp` 기능 header 추가
- [x] `stats.hpp` 기능 header 추가
- [x] `RequestContext`
  - request id, peer, arrival time, deadline, connection/cancellation 상태
  - 기존 `Handler(const Request&)` 단순형 유지
  - context overload가 request hot path를 회귀시키지 않도록 A/B
- [x] `ServerStats` / `SessionStats`
  - active connections/streams, requests, timeout/rejection
  - retained input/output bytes, worker/serialized queue depth
  - snapshot 비용과 thread-safety semantics 문서화
- [x] option 구조 정리
  - `Server(path)` / `Client(path)` 기본 진입점 유지
  - advanced option은 명시적이며 숨은 retry나 workload 추측 금지
  - profile은 실측된 값 묶음일 때만 고려
- [x] Response helper와 naming consistency review
  - `Response{status, body}` aggregate와 `status_*` 상수를 유지하고 중복 factory helper는 추가하지 않음
- [x] 0.6 → 0.7 migration guide 시작

## Phase 3 — Advanced usability

- [x] serialized domain
  - 기존 `on_serialized(route, handler)`는 default domain FIFO
  - drivetrain/arm처럼 독립적인 domain은 서로 병렬 진행
  - domain lifecycle, fairness, shutdown semantics 명시
- [x] queue policy
  - `FIFO` 기본
  - `LatestWins`: 실행 전 대기 중인 같은 key의 오래된 command 교체
  - `RejectIfBusy`: 실행/대기 상태를 명확한 error/status로 반환
  - 교체·거부된 request의 response와 stats semantics 회귀 테스트
- [x] advanced route options
  - domain, policy, deadline/cancellation 관찰을 한 options 구조로 확장
  - overload 폭증 방지
  - [x] simple/contextual handler, domain/policy, replacement key와 409 semantics 사전 설계
- [x] production diagnostics (bounded snapshot scope)
  - `Server::stats()`/`Session::stats()`와 opt-in cumulative counters로 운영
    상태를 관찰하고, 기본 비활성 시 request hot path에 counter RMW가 없음
  - exporter는 라이브러리에 강제하지 않고 application-owned health/
    diagnostics RPC 예제로 연결
  - low-level optional tracing은 build-time diagnostic scope로 유지

## Phase 4 — Examples / documentation

- [ ] README를 소개 → 설치 → 5분 quick start → 핵심 기능 → 상세 링크로 축소
- [ ] `docs/getting-started/`: installation, quick-start, concepts
- [ ] `docs/api/`: client/session/server/request-response/stream/options/errors/
      FD/context/stats
- [ ] `docs/guides/`: serialized, robot HAL, timeout, production, security,
      performance tuning
- [ ] `docs/internals/`: architecture, reactor, concurrency, backpressure,
      lifetime, protocol v2
- [ ] 모든 중요 API reference에 Purpose, Parameters, Return, Thread safety,
      Ownership, Lifetime, Timeout, Error, Performance, Example 명시
- [ ] examples 확대
  - hello world, persistent session, concurrent requests, serialized commands
  - streaming, FD passing, peer credentials, stats, production server
  - [x] `drive/velocity`, `arm/position`, `health`, `diagnostics`를 포함한
        컴파일 가능한 robot HAL composition example
- [ ] 0.6 실험은 `experiments/0.6/`와 history 문서로 이동하되 삭제하지 않음

## Phase 5 — Stabilization

- [ ] 0.6.4 대비 x86_64/ARM64 p50/p99/p99.9/throughput/CPU/allocation A/B
- [ ] long-lived Session, timeout, shutdown, stalled peer, strict budget stress
- [ ] ASan/UBSan, TSan, protocol/session fuzz final gate
- [ ] GCC/Clang static/shared와 install-package consumer
- [ ] public API naming/ownership/thread-safety consistency review
- [ ] documentation link/example build check
- [ ] protocol v2 유지 또는 v3 전환 결정과 migration 동결

## 목표 directory 형태

최종 이름보다 “기능으로 찾을 수 있음”이 우선이다.

```text
include/easy_uds/       umbrella + 기능별 public header
src/client/             one-shot client, Session, Session state
src/server/             lifecycle, routing, serialized execution, state
src/reactor/            epoll parsing, dispatch, flow/output, workers, streams
src/protocol/           version-independent codec boundary + protocol v2
src/detail/             fd, deadline, socket, exact I/O utilities
tests/                  unit, integration, stress, fuzz, package
benchmarks/             rpc, Session, streaming
experiments/0.6/        채택되지 않은 실험과 재현 코드 보존
examples/               번호가 있는 학습 순서 + production/robot examples
docs/                   getting-started, api, guides, internals, migration, history
```

## 0.7 완료 조건

- 처음 사용자는 README만 보고 5–10분 안에 기본 RPC를 실행한다.
- 일반 사용자는 30분 안에 Session, timeout, serialized, stream, FD passing을
  소스 복사와 문서만으로 사용한다.
- 고급 사용자는 소스를 읽지 않고 ownership, thread safety, deadline, queue,
  memory, backpressure, credentials, stats, lifecycle을 정확히 이해한다.
- 0.6.4의 hot-path 성능과 안정성은 위 회귀 게이트 안에 남는다.
- 실패한 0.6 실험과 기각 근거는 계속 재현 가능하다.

한 줄 목표:

> **0.6이 실제로 빠른 것만 남겼다면, 0.7은 그 성능을 누구나 쉽게 쓰게 만든다.**
