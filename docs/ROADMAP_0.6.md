# 0.6.x UDS 한계 돌파 로드맵

`0.6`의 목표는 Unix Domain Socket 기반 IPC 서버의 **한계 돌파**다. reactor,
worker, multiplexing, backpressure, streaming, lifetime을 실제로 검증하는
것을 넘어서, 측정으로 입증된 기법(io_uring 백엔드, `SCM_RIGHTS` FD passing,
zero-copy 파일 스트림, allocator/object pool, 플랫폼별 spin 프로파일 등)을
**0.6 안에서 채택**한다. API/ABI/protocol 동결(0.6.5)은 채택 판정이 모두 끝난
뒤의 마지막 0.6 릴리즈로 **연기**한다.

판정 원칙은 그대로 유지한다: "코드가 존재함"이 아니라 동일 워크로드에서
latency/throughput/CPU/memory 중 하나 이상의 개선이 **측정**되어야 채택한다.
게이트를 통과하지 못한 실험은 문서에 결과를 남기고 default 경로에서 제거한다.

## 한계 축 (KPI)

| 축 | 현재 (WSL2 i7-1260P) | 목표 | 지표 |
|---|---|---:|---|
| 지연 바닥 | session p50 38µs (raw RTT ~34µs) | 오버헤드 여유가 수 µs로 RTT에 수렴 | p50/p99, MAD |
| 처리량 | ~67k req/s @8-way | syscall/req 감축으로 승수 확보 | req/s, syscall/req |
| 자원 효율 | warm session alloc 0/req | stream/serialized 경로도 0에 근접 | alloc/req, ctx-switch/req, CPU |
| 복사 제거 | stream 5.7~9.5 GiB/s | user-space 복사 제거 영역 확대 | copy/bytes, GiB/s |

## 릴리즈 경계

### 0.6.1 — Reactor Hardening (완료)

- per-connection input/output backpressure
- idle session 수명 수정
- stalled peer 격리
- session wakeup/in-flight/reactor state hot-path 최적화
- reactor wakeup pipe를 단일 `eventfd` counter로 교체
- worker wakeup 중복 write를 atomic coalescing
- connection별 epoll interest mask 캐시로 불필요한 MOD syscall 제거
- prefix route registry를 longest-first로 정렬해 dispatch 조기 종료
- reactor bounded read-ahead batch로 대형 frame epoll 왕복 감소
- session spin loop의 중복 clock read 제거
- session spin window에서 architecture CPU hint 사용
- reactor와 unit test 모듈 분리

### 0.6.3 — Experimental Closure + 기준선/경합 판정 (진행 목표)

#### P0: 반드시 완료

- [x] 서버 전체 메모리 예산
  - [x] `max_total_inflight_bytes` 내부 accounting
  - [x] `max_total_output_bytes` 내부 accounting
  - [x] connection별 fixed request 개수/바이트 high-water mark를 옵션화
  - [x] connection별 fixed response queue high-water mark를 옵션화
  - [x] connection별 intake 정지/재개 정책
  - [x] budget validation과 shutdown 회귀 테스트
- [ ] **기준선 재측정** (WSL 수치 폐기)
  - [ ] 네이티브 x86_64 Linux에서 one-shot/session/stream/allocation
        benchmark 재실행
  - [ ] `strace -c` / `perf stat` 가용 호스트에서 syscall/cache/branch miss
        기록 (syscall/req, alloc/req)
  - [ ] 샘플·워밍업·반복 수 고정, 중앙값+분산 규약 문서화
- [ ] **지연 바닥 판정**
  - [x] continuation lease 확장: 단일 follow-up 홉을 넘는 조건 재설계 → 측정
        — **이미 지속 multi-followup 구현됨을 확인** (continuation ON vs OFF
        A/B: futex 2x·지연 2x 절감). 잔여 rearm 비용(epoll_ctl ~2%)은
        burst 병렬성 보호 대비 가치 없어 재설계하지 않음. 남은 지연 여유는
        shared/multiplexed 경로. PERF_0.6.md 기록
  - [ ] spin 기본값 확정: 50µs vs 100µs — ARM64 실측으로 고정 또는
        플랫폼별 프로파일 도입 (`ClientOptions` 노출은 게이트 통과 시에만)
  - [x] condvar fallback 트리거 지점 측정 (spin 창 초과 빈도, p99 관점)
        — `EASY_UDS_TRACE_SPIN_MISS` 진단 카운터 추가. 100µs에서 0.58% fallback:
        PERF_0.6.md 기록
- [ ] **경합/자원 판정**
  - [x] false-sharing 패딩: Connection/ServerState 핫 아토믹이 실제 동시성
        벤치에서 병목인지 측정 → **기각** (shared c16 A/B, 5개 아토믹 alignas
        64: 신호 없음. PERF_0.6.md 기록)
  - [x] allocator/object pool: stream/serialized/prefix 경로 alloc/req 측정
        → **기각** (session 0.0004/req, serialized 2.33/req, stream
        15.76/MiB. 병목 없음. PERF_0.6.md 기록)
  - [x] read-ahead 256KiB 배치 크기 스윕 — 기본 유지 확정 (1MiB 배치는 large-frame
        2x 회귀, 64KiB는 9% 손해): PERF_0.6.md 기록
- [ ] ARM64 게이트 (P0 완료 조건)
  - [ ] 1KiB/64KiB/1MiB RPC 페이로드
  - [ ] stream과 session concurrency
  - [ ] connection 1/8/32/64
  - [ ] 장시간 shutdown/timeout stress
  - [ ] 기준선과 비교표

#### P1: 게이트용 실험 (수행 완료, 채택 판정은 0.6.3/0.6.4에서)

- [x] `io_uring_setup` capability probe — 채택 판정은 0.6.5 옵트인 백엔드
- [x] `SCM_RIGHTS`/`memfd_create` FD passing standalone prototype — 채택 판정은
      0.6.4
- [x] file-backed `sendfile`/`splice` zero-copy standalone 비교 — 채택 판정은
      0.6.4
- [x] false sharing 및 allocator 측정 (`alignas`, queue/map allocation probe)
- [ ] end-to-end framed stream에서 `sendfile`/`splice` 유효성 검증
      (framing + size limit + deadline + fallback을 얹고 재측정)

P1 실험은 측정 자료가 없으면 public API 또는 기본 backend에 포함하지 않는다.
결과가 부정적이면 문서에 기록하고 default 경로에서 제거한다. (기존 방침 유지)

### 0.6.4 — Zero-copy + FD passing 채택

- [x] framed file-stream 측정 게이트 + in-library 검증 → **기각**
      - framed probe 승수(1.7~1.8x)는 `read+write()` 기준선 대비 착시였다.
        라이브러리 gathered `sendmsg` 경로(~9.8 GiB/s)와 비교하면 sendfile
        파일 스트림은 non-blocking에서 1.3–1.6 GiB/s, blocking에서 2.3 GiB/s로
        4~7x 회귀. 파일 소스 public API는 **도입하지 않는다**.
      - zero-copy 목표는 io_uring(0.6.5) `IORING_OP_SENDFILE`로 연기.
      - 같은 검증에서 백투백 스트림 스퓨리어스 거부 레이스 발견·하드닝:
        스트림 슬롯을 응답 쓰기 직후(rearm 전) 해제. 회귀 테스트
        `test_back_to_back_streams` 추가.
- [x] `SCM_RIGHTS` FD passing을 Linux-gated public API로 승격
      - 공개 API: `Client::request_fd()`, `Request::fd` (서버가 핸들러 반환 후
        fd 소유권 취득·close, `dup()`은 핸들러 책임)
      - wire: 헤더 예약 플래그 bit0 `carries_fd_flag` — v2 헤더 레이아웃 유지.
        미지원 플래그 조합은 명시 거부(조용한 fd 유실 없음)
      - 리액터 read-ahead를 recvmsg(제어버퍼)+순서 FIFO로 전환. read-ahead
        배치에서 중간 프레임 fd 생존을 프로브로 검증, plain recv는 조용히
        버리는 것도 확인. 세션 연속 경로는 fd 플래그 거부, 스트림 요청도 거부
      - hot-path 회귀 없음: session A/B 중앙값 43.7k(recvmsg) vs 42.3k(recv)
      - 세 시나리오로 프로브 확장 + 유닛 테스트 `test_fd_passing` (round-trip,
        누수 없음, 404 경로)
- [x] protocol v2 와이어 호환 유지 (SCM_RIGHTS는 라이트 확장·ancillary data)

### 0.6.5 — io_uring 백엔드 + Final Stabilization (마지막 0.6 릴리즈)

- [ ] io_uring 리액터 프로토타입: multishot accept, fixed CQE, 제공자 버퍼
- [ ] epoll 대비 동일 워크로드 벤치 판정 (고동시성, syscall/req, p99, CPU)
- [ ] 채택 시 기본은 epoll 유지하고 옵트인 백엔드로 포함 여부 결정
- [ ] 0.6.3/0.6.4 채택 결과 정리 — 미채택 prototype 제거 또는 실험
      namespace 격리
- [ ] public API/ABI/protocol 동결
- [ ] Linux x86_64 + ARM64 장시간 stress
- [ ] full sanitizer/fuzz/package CI green
- [ ] 성능표와 migration/release 문서 최종 동기화
- [ ] `0.6.x`에서 더 이상 기능 확장하지 않고 0.7 사용성 작업으로 이동

## 채택 게이트

각 후보는 아래 조건을 만족해야 0.6 기본 경로에 포함된다. 게이트 실패는
문서에 보고서를 남기고 제거한다. (동일한 기준으로 채택과 폐기 모두 결판)

- **지연 바닥**: continuation 홉 감소 또는 spin 확정이 동일 워크로드에서
  p50/p99 개선이 측정되어야 함
- **경합/자원**: 패딩·allocator·read-ahead가 동시성 벤치에서 측정된 병목을
  해소해야 함
- **zero-copy**: framed end-to-end 스트림에서 socketpair probe와 동등하거나
  더 큰 승수가 유지되어야 함 (1.26–2.88x는 socketpair 전용, 호스트 의존)
- **FD passing**: 프로세스 간 fd 전달이 API 복잡도 대비 명확한 사용처와
  안전성(ownership, 첨부 프레임 조각 제한)이 검증되어야 함
- **io_uring**: 기본 epoll 대비 syscall/req 또는 p99 개선이 측정되어야 함.

## 명시적으로 0.6에서 하지 않는 것 (남는 것)

- handler를 강제로 중단하는 preemptive cancellation — portable C++ 한계 유지
- protocol v3 또는 wire-breaking 변경 — 0.6.4 확장은 v2 와이어 호환 안에서
- 측정 없이 무조건 채택 — 아래는 전부 게이트 항목이므로 측정 필수:
  io_uring 백엔드, FD passing, zero-copy, allocator/object pool,
  `ClientOptions`의 spin 노출

## 완료 판정

각 최적화는 "코드가 존재함"이 아니라 동일 workload에서 기준선 대비
latency/throughput/CPU/memory 중 하나 이상의 개선 또는 명확한 안정성 이득이
측정되어야 한다. 채택 실패 실험은 문서에 결과를 남기고 기본 경로에서
제거하며, 채택/폐기 결정 자체를 보고서로 남긴다.
