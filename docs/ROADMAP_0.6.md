# 0.6.x 기술 실험 로드맵

`0.6`의 목표는 쉬운 API 확장이 아니라 Unix Domain Socket 기반 IPC 서버의
reactor, worker, multiplexing, backpressure, streaming, lifetime을 실제로
검증하는 것이다. `0.6.3`에서 실험을 마무리하고 `0.6.4`에서 결과를 반영한
마지막 안정화 릴리즈를 만든다.

## 릴리즈 경계

### 0.6.1 — Reactor Hardening (완료)

- per-connection input/output backpressure
- idle session 수명 수정
- stalled peer 격리
- session wakeup/in-flight/reactor state hot-path 최적화
- reactor와 unit test 모듈 분리

### 0.6.3 — Experimental Closure (진행 목표)

#### P0: 반드시 완료

- [x] 서버 전체 메모리 예산
  - [x] `max_total_inflight_bytes` 내부 accounting
  - [x] `max_total_output_bytes` 내부 accounting
  - [x] connection별 intake 정지/재개 정책
  - [x] budget validation과 shutdown 회귀 테스트
- [ ] 성능 계측 기준선
  - [x] one-shot/session/stream benchmark 고정 workload
  - [x] p50/p95/p99, throughput, CPU, context switch 기록 (`getrusage`)
  - [x] syscall/request, allocation/request 기록
  - [x] spin 0/10/25/50/100µs 비교 (WSL 기준선, 기본값 100µs 유지)
- [ ] ARM64 smoke/stress 검증
  - [ ] 1KiB/64KiB/1MiB RPC
  - [ ] stream과 session concurrency
  - [ ] connection 1/8/32/64
  - [ ] 장시간 shutdown/timeout stress

#### P1: 별도 실험으로 검증

- [x] `io_uring_setup` capability probe — backend는 epoll 유지
- [x] `SCM_RIGHTS`/`memfd_create` FD passing standalone prototype
- [x] file-backed `sendfile` zero-copy standalone 비교 (효과 미미, public API 미도입)
- [x] false sharing 및 allocator 측정 (`alignas`, queue/map allocation probe)

P1 실험은 성능·복잡도·이식성 자료가 없으면 public API 또는 기본 backend에
포함하지 않는다. 결과가 부정적이어도 실험 보고서에 기록하면 완료로 본다.

### 0.6.4 — Final Stabilization (마지막 0.6 릴리즈)

- [ ] 0.6.3 실험 결과 중 효과가 입증된 것만 기본 경로에 반영
- [ ] 효과 없는 prototype 제거 또는 experimental namespace 격리
- [ ] public API/ABI/protocol 동결
- [ ] Linux x86_64 + ARM64 장시간 stress
- [ ] full sanitizer/fuzz/package CI green
- [ ] 성능표와 migration/release 문서 최종 동기화
- [ ] `0.6.x`에서 더 이상 기능 확장하지 않고 0.7 사용성 작업으로 이동

## 명시적으로 0.6에서 하지 않는 것

- handler를 강제로 중단하는 preemptive cancellation
- io_uring, FD passing, zero-copy를 근거 없이 public API에 추가
- 측정 없이 custom allocator/object pool 도입
- `ClientOptions`에 spin duration을 먼저 노출하는 API 확장
- protocol v3 또는 wire-breaking 변경

## 완료 판정

각 최적화는 “코드가 존재함”이 아니라 동일 workload에서 기준선 대비
latency/throughput/CPU/memory 중 하나 이상의 개선 또는 명확한 안정성 이득이
측정되어야 한다. 악화된 실험은 문서에 결과를 남기고 기본 경로에서 제거한다.
