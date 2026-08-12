# easy-uds

[English README](README.md)

`easy-uds`는 Unix Domain Socket(`AF_UNIX`)을 이용해 같은 시스템 안의 프로세스끼리 통신하기 위한 작은 C++17 IPC 라이브러리입니다. 공개 API를 작게 유지하면서도 요청/응답 RPC, 대용량 스트리밍, 제한된 동시성, 타임아웃, 안전한 종료, 바이너리 프레이밍, CMake 패키지 설치를 제공합니다.

특히 한 프로세스에서만 소유할 수 있는 로봇 드라이버나 하드웨어 드라이버를 여러 프로세스가 함께 사용해야 할 때 유용합니다. `on_serialized()`를 사용하면 여러 클라이언트 프로세스가 동시에 명령을 보내더라도 실제 하드웨어 명령은 한 번에 하나씩 FIFO 순서로 실행됩니다.

> **프로토콜 참고:** v0.6.0은 20바이트 헤더와 request-id 멀티플렉싱을 사용하는 protocol version 2입니다. v0.5.x 이하의 protocol v1과 wire 호환되지 않습니다.

## 주요 기능

- C++17, 런타임 서드파티 의존성 없음
- route 기반 request/response RPC
- handler table copy-on-write snapshot(요청 dispatch 시 전역 table lock·`std::function` 복사 없음)
- request/response body에 NUL, 개행 등을 포함한 임의 바이너리 데이터 사용 가능
- `on_serialized()`를 이용한 전역 FIFO 직렬 명령 큐
- 직렬 명령 대기 중에도 일반 `on()` RPC worker는 점유하지 않음
- 큰 데이터를 메모리에 전부 올리지 않는 chunk streaming
- Unix socket backpressure를 통한 자연스러운 흐름 제어
- versioned binary protocol
- connection마다 detached thread를 만들지 않는 고정 worker pool
- 느린 고정 응답의 쓰기가 reactor나 worker pool을 점유하지 않는 `EPOLLOUT` 출력 큐
- 최대 connection 수, I/O inactivity timeout, absolute request deadline, connect timeout 설정
- non-blocking socket + 필요할 때만 `poll()` 사용
- `sendmsg()`를 이용한 header+payload gathered write
- 기본 socket 권한 `0600`
- 동일 socket path의 중복 server 실행과 stale socket 정리를 보호하는 instance lock
- 다른 thread에서 안전하게 호출할 수 있는 `Server::stop()`
- handler 예외를 `500 / Internal Server Error`로 변환
- socket 오류를 `errno`를 보존한 `std::system_error`로 전달
- static/shared library 및 CMake `find_package()` 지원
- unit/stress/ASan/UBSan/TSan/fuzz/package-consumer 테스트 구성

## 플랫폼

0.6 구현은 Linux(`epoll`, `SO_PEERCRED`)를 요구합니다. Windows, macOS, BSD는 현재 지원하지 않습니다. Linux 전용 abstract socket이 아니라 파일시스템 pathname socket을 사용합니다.

## 빠른 시작

### Server

```cpp
#include <easy_uds/easy_uds.hpp>

#include <iostream>

int main() {
    easy_uds::Server server("/tmp/easy-uds.sock");

    server.on("ping", [](const easy_uds::Request&) {
        return easy_uds::Response{200, "pong"};
    });

    server.on("echo", [](const easy_uds::Request& request) {
        return easy_uds::Response{200, request.body};
    });

    std::cout << "Server listening on " << server.socket_path() << '\n';
    server.run();
}
```

### Client

```cpp
#include <easy_uds/easy_uds.hpp>

#include <iostream>

int main() {
    easy_uds::Client client("/tmp/easy-uds.sock");

    const auto response = client.request("echo", "hello");
    std::cout << response.status << ' ' << response.body << '\n';
}
```

`Client`는 각 `request()` 호출마다 독립된 socket을 사용하고 내부 상태를 변경하지 않으므로 하나의 `Client` 객체에서 여러 thread가 동시에 `request()`를 호출할 수 있습니다.

## 로봇/하드웨어 명령을 한 번에 하나씩 실행하기

여러 프로세스가 하나의 로봇 드라이버를 공유할 때 일반 `on()`만 사용하면 여러 worker thread에서 서로 다른 handler가 동시에 실행될 수 있습니다.

```text
Vision Process ─┐
Mission Process ├── Unix Domain Socket ── Robot Driver Server
GUI Process ────┘
```

예를 들어 Mission과 GUI가 거의 동시에 이동 명령을 보내면 드라이버에 명령이 겹칠 수 있습니다. 이런 route는 `on_serialized()`로 등록합니다.

```cpp
#include <easy_uds/easy_uds.hpp>

int main() {
    easy_uds::Server server("/run/robot-driver.sock");

    server.on_serialized("drive", [](const easy_uds::Request& request) {
        // 실제 drive driver 호출
        // 이 handler가 실행되는 동안 다른 on_serialized() handler는 대기합니다.
        execute_drive_command(request.body);
        return easy_uds::Response{200, "ok"};
    });

    server.on_serialized("arm", [](const easy_uds::Request& request) {
        // route가 달라도 drive와 동시에 실행되지 않습니다.
        execute_arm_command(request.body);
        return easy_uds::Response{200, "ok"};
    });

    server.on("status", [](const easy_uds::Request&) {
        // 일반 RPC는 serialized queue 뒤에서 기다리지 않습니다.
        return easy_uds::Response{200, read_robot_status()};
    });

    server.run();
}
```

동작은 다음과 같습니다.

```text
Process A: drive ───────┐
                        ├─> serialized FIFO ─> drive 실행 ─> arm 실행 ─> ...
Process B: arm ─────────┘

Process C: status ─────────> 일반 worker pool에서 별도로 처리
```

모든 `on_serialized()` route는 하나의 FIFO executor를 공유합니다. 따라서 `drive`, `arm`, `motor/set`처럼 route가 서로 달라도 직렬 route끼리는 동시에 실행되지 않습니다.

FIFO 순서는 server가 완전한 request를 serialized executor에 넣은 순서입니다. 연결된 순서 자체가 아니라 **완전히 수신되어 queue에 들어온 순서**라는 점에 주의하십시오.

### 오래 기다린 명령의 안전 처리

serialized queue에서 기다리는 시간도 기존 server `request_timeout`에 포함됩니다. 명령이 실행되기 전에 absolute deadline을 넘으면 handler를 호출하지 않고 `408`로 응답합니다.

이 동작은 오래된 로봇 명령이 뒤늦게 실행되는 상황을 막기 위한 것입니다.

```text
move A 실행 중
    ↓
move B 대기
    ↓ request_timeout 초과
move B 폐기 (handler 실행 안 함)
```

`Server::stop()`을 호출해도 아직 queue에서 기다리는 serialized 명령은 폐기됩니다.

단, 이미 실행을 시작한 C++ handler를 강제로 중단하는 기능은 없습니다. 이는 일반 `on()` handler와 동일합니다. 장시간 실행되는 hardware handler가 필요하다면 애플리케이션 수준의 cancellation/stop flag를 함께 설계하는 것이 좋습니다.

### 어떤 route를 `on_serialized()`로 등록해야 하나?

하나의 exclusive resource를 변경하는 명령은 보통 serialized route가 적합합니다.

```text
권장: on_serialized()
- motor/set
- drive
- arm/move
- servo/write
- gpio/write
- robot/reset
- trajectory/start

보통 on()
- ping
- health
- version
- status
- configuration 조회
- 독립적으로 안전한 sensor 조회
```

다만 sensor read 자체도 사용하는 드라이버가 thread-safe하지 않다면 해당 조회 역시 `on_serialized()`로 묶어야 합니다. 어떤 route가 동시에 실행되어도 안전한지는 `easy-uds`가 아니라 실제 드라이버의 thread-safety 조건에 따라 결정해야 합니다.

## 대용량 또는 연속 데이터 스트리밍

`request_stream()`은 request를 고정 크기 buffer로 조금씩 읽고 response도 chunk 단위로 전달합니다. `StreamReader`가 `0`을 반환하면 해당 stream의 끝입니다.

```cpp
// Server: 전체 파일을 메모리에 저장하지 않고 업로드 처리
server.on_stream("upload", [](const easy_uds::StreamReader& body) {
    std::ofstream output("received.bin", std::ios::binary);
    std::array<char, 64 * 1024> buffer{};
    while (const std::size_t size = body(buffer.data(), buffer.size())) {
        output.write(buffer.data(), static_cast<std::streamsize>(size));
    }
    return easy_uds::StreamResponse{201, {}};
});

// Client: 파일을 chunk 단위로 전송
std::ifstream input("large.bin", std::ios::binary);
easy_uds::StreamReader upload = [&input](char* buffer, std::size_t capacity) {
    input.read(buffer, static_cast<std::streamsize>(capacity));
    return static_cast<std::size_t>(input.gcount());
};

const int status = client.request_stream("upload", upload, [](std::string_view chunk) {
    // chunk view는 callback이 끝나기 전에 소비하거나 복사해야 합니다.
});
```

stream은 half-duplex입니다. request stream이 모두 끝난 다음 response stream이 시작됩니다. `max_stream_size = 0`, `stream_timeout = 0`이면 장시간/무제한 stream을 허용할 수 있으며, `io_timeout`은 데이터 진행이 멈춘 peer를 계속 감지합니다.

## 고주파 폴링을 위한 지속 연결 세션

`Client::request()`는 호출마다 연결을 열고 닫습니다. 고주파 폴링(IMU, encoder, health check)에는 `Client::session()`으로 연 연결을 재사용해 요청당 connect/accept 및 teardown 비용을 없앨 수 있습니다:

```cpp
easy_uds::Client client("/tmp/robot-driver.sock");
easy_uds::Session session = client.session();  // 하나의 연결을 계속 재사용
while (true) {
    const auto response = session.request("imu", "poll");
    // ...처리...
}
```

`Session`은 동시 `request()` 호출을 **멀티플렉싱**합니다(request id로 상관관계를 매기고 응답은 무순서로 도착). 서버는 유휴 grace 동안 연결을 처리한 워커가 직접 이어 읽는 고속 경로를 쓰므로 고주파 폴링의 단일 요청 왕복 지연이 WSL 원시 바닥(~38 µs p50)에 근접합니다. I/O 오류, request timeout 또는 peer close 이후에는 영구적으로 사용할 수 없습니다(재연결하려면 새 세션). serialized route 요청도 응답 후 세션을 유지합니다. `request_stream()`은 독립된 전용 연결을 사용하므로 fixed request나 다른 stream 호출을 막지 않습니다.

`Server::enqueue_maintenance()`는 serialized handler와 같은 FIFO 실행기에서, 그리고 그들과 엄격한 순서로 실행되는 작업을 등록합니다. serialized handler가 접근하는 서버 측 상태(예: 드라이버 인스턴스 맵)를 외부 스레드(스위퍼 등)에서 안전하게 정리할 때 사용합니다:

```cpp
server.enqueue_maintenance([&] { drivers.erase(dead_driver_name); });
```

작업은 정확히 한 번, FIFO 순서로 실행되며, 예외를 던져도 실행기가 계속 동작합니다. server가 실행 중이 아니면 `std::logic_error`를 던집니다.

## Server 설정

```cpp
#include <chrono>

using namespace std::chrono_literals;

easy_uds::ServerOptions options;
options.worker_threads = 4;
options.max_connections = 64;
options.max_message_size = 1024 * 1024;
options.stream_chunk_size = 64 * 1024;
options.max_stream_size = 1024 * 1024 * 1024;
options.io_timeout = 5s;
options.request_timeout = 30s;
options.stream_timeout = 0ms;
options.stale_socket_grace_period = 250ms;
options.listen_backlog = 64;
options.socket_permissions = 0600;

easy_uds::Server server("/tmp/easy-uds.sock", options);

// 선택 사항: 자동 모드는 가능하면 일반 RPC worker 하나를 예약합니다.
options.max_concurrent_streams = 3;
```

| 옵션 | 기본값 | 의미 |
| --- | ---: | --- |
| `worker_threads` | `4` | 일반·stream handler를 실행하는 worker 수(serialized executor는 별도) |
| `max_connections` | `64` | 동시에 accept되어 열려 있는 client connection 최대 수 |
| `max_message_size` | `1 MiB` | request route+body 및 response body 최대 크기 |
| `stream_chunk_size` | `64 KiB` | stream용 재사용 buffer/frame 크기 |
| `max_stream_size` | `1 GiB` | 한 request/response stream의 최대 총 byte 수. `0`은 제한 없음 |
| `max_total_inflight_bytes` | `0` | 전체 connection의 fixed request payload 합산 예산. `0`은 전역 제한 없음 |
| `max_total_output_bytes` | `0` | 전체 connection의 fixed response queue 합산 예산. `0`은 전역 제한 없음 |
| `max_inflight_requests_per_connection` | `64` | connection별 queued/executing fixed request 개수 high-water mark |
| `max_inflight_request_bytes_per_connection` | `4 MiB` | connection별 queued/executing route+body 바이트 high-water mark |
| `max_output_bytes_per_connection` | `4 MiB` | connection별 queued fixed response 바이트 high-water mark |
| `io_timeout` | `5 s` | 성공적인 socket I/O 진행 사이의 최대 idle 시간 |
| `request_timeout` | `30 s` | 일반 RPC의 첫 header byte부터 response 완료까지 absolute deadline. serialized queue 대기도 포함 |
| `stream_timeout` | `0` | stream 전체 absolute deadline. `0`은 비활성 |
| `session_idle_grace` | `1 ms` | 마지막 요청을 마친 워커가 이 시간 동안 후속 요청 하나를 직접 기다려 리액터 디스패치 홉을 줄입니다. 핸들러 실행 전에는 연결을 리액터에 반환해 멀티플렉싱을 유지합니다. `0`은 고속 경로 비활성 |
| `max_concurrent_streams` | `0` (자동) | 동시 stream 수 상한. 자동값은 `worker_threads - 1`이며 worker가 하나뿐이면 `1`. 명시값은 `1`~`worker_threads` |
| `include_handler_error_messages` | `true` | `500` body에 handler 예외 메시지 포함. 내부 정보 노출을 피하려면 `false` |
| `stale_socket_grace_period` | `250 ms` | refused socket을 stale로 판단하기 전 대기 시간 |
| `listen_backlog` | `64` | `listen()` backlog |
| `socket_permissions` | `0600` | Unix socket pathname 권한 |

일반 RPC 입력은 connection마다 `max_inflight_requests_per_connection`과 `max_inflight_request_bytes_per_connection`으로 제한되며, `max_total_inflight_bytes`로 서버 전체 예산도 제한할 수 있습니다. 어느 high-water mark에 도달하면 해당 peer의 `EPOLLIN`만 일시 중지하고 저수위에서 다시 시작합니다. fixed response queue는 `max_output_bytes_per_connection`으로 느린 peer를 격리하고, `max_total_output_bytes`로 전체 예산도 제한할 수 있습니다. 따라서 kernel이 받은 나머지 byte에는 Unix socket backpressure가 걸리며 worker queue가 무제한으로 커지지 않습니다.

고정 응답은 worker가 non-blocking fast path로 한 번 전송한 뒤, 남은 byte만 connection별 `EPOLLOUT` 큐에 넘깁니다. 큐 상한은 4 MiB와 최대 응답 하나의 크기 중 큰 값이며, 이를 넘기는 peer만 닫습니다. 응답을 읽지 않는 client가 일반 worker pool을 점유하지 않습니다. Stream은 기존의 전용 worker lease와 `max_concurrent_streams` 제한을 사용합니다.

`request_timeout`은 worker queue, serialized queue, socket I/O 시간을 모두 포함합니다. serialized 명령이 실행 전에 timeout되면 handler를 실행하지 않고 `408`로 응답합니다.

## Client 설정

```cpp
#include <chrono>

using namespace std::chrono_literals;

easy_uds::ClientOptions options;
options.max_message_size = 1024 * 1024;
options.stream_chunk_size = 64 * 1024;
options.max_stream_size = 1024 * 1024 * 1024;
options.connect_timeout = 2s;
options.io_timeout = 5s;
options.request_timeout = 30s;
options.stream_timeout = 0ms;

easy_uds::Client client("/tmp/easy-uds.sock", options);
```

`connect_timeout`은 연결 수립 시간만 제한합니다. `io_timeout`은 I/O 진행이 없는 시간을 제한합니다. 유휴 `Session` 자체에는 이 시간이 적용되지 않습니다. 첫 response byte는 무기한 기다리되 각 호출자의 `request_timeout`은 계속 적용되며, response frame이 일부 도착한 뒤 멈추면 `io_timeout`이 적용됩니다. `request_timeout`은 일반 request 전체, `stream_timeout`은 streaming transaction 전체를 제한합니다. `0`은 해당 제한을 비활성화합니다.

socket timeout은 `std::system_error`로 전달되며 timeout의 error code는 `ETIMEDOUT`입니다.

## Socket 소유권과 stale socket 정리

각 server socket에는 `<socket_path>.lock` companion lock file을 사용합니다. 파일이 존재하는 것 자체가 소유권을 의미하는 것이 아니라 kernel advisory lock이 실제 ownership을 의미합니다.

기존 pathname이 존재하지만 connection을 거부하는 경우 easy-uds는 다음 조건을 확인한 뒤 stale socket을 제거합니다.

1. Unix socket인지 확인
2. 현재 effective user 소유인지 확인
3. `stale_socket_grace_period`만큼 대기
4. inode identity가 바뀌지 않았는지 다시 확인
5. 동일 socket일 때만 `unlink()`

일반 파일이나 다른 사용자의 socket을 의도적으로 삭제하지 않습니다.

가능하면 `/tmp`의 공개 위치보다는 애플리케이션 trust boundary에 맞는 private runtime directory에 socket을 두는 것이 좋습니다.

## 동시성과 종료

`Server::run()`은 epoll reactor와 고정 worker pool을 시작한 뒤 종료될 때까지 block됩니다. serialized executor thread는 처음 필요할 때 지연 시작됩니다. 하나의 `Server` 객체에서 `run()`은 한 번만 호출할 수 있습니다.

일반 `on()` handler는 여러 worker thread에서 동시에 실행될 수 있습니다. 동일하게 등록된 함수 객체가 여러 worker에서 동시에 호출될 수 있으므로 mutable capture와 공유 state는 애플리케이션이 직접 동기화해야 합니다. Handler table 갱신은 copy-on-write이며 진행 중인 요청을 무효화하지 않고 원자적으로 공개됩니다.

`on_serialized()` handler는 모든 serialized route가 하나의 executor를 공유하므로 한 번에 정확히 하나만 실행됩니다. reactor가 serialized request의 header/body를 읽어 전용 queue로 넘기므로 일반 worker pool을 점유하지 않습니다.

종료 시에는 다음 순서로 정리됩니다.

1. `running` 해제 및 wakeup pipe로 `epoll_wait()` 중단
2. 소유 중인 socket pathname을 inode 확인 후 제거
3. accept된 모든 client socket을 `shutdown()`하여 blocked I/O 중단
4. 일반·serialized executor에 종료를 알리고 아직 실행되지 않은 작업 폐기
5. reactor, worker pool, serialized executor 종료 및 join
6. connection/listener/wakeup/epoll descriptor와 instance lock 해제

reactor가 listener를 poll하는 동안 `stop()` thread가 listener FD를 직접 close하지 않으므로 descriptor-number reuse race를 피합니다.

## Wire protocol

모든 message는 20-byte binary header로 시작합니다. header에는 `EUDS` magic, protocol version, message type, request id와 두 argument로 구성된 세 개의 32-bit network-byte-order field가 들어갑니다. route와 body는 길이 기반이므로 NUL과 newline을 그대로 보존합니다.

정확한 형식은 [`docs/PROTOCOL.md`](docs/PROTOCOL.md)를 참고하십시오.

지속 연결에서는 fixed request를 파이프라이닝하고 request id로 응답을 구분할 수 있습니다. stream은 해당 wire connection에서 half-duplex로 독점 실행되며, 응답이 끝난 뒤 같은 connection에 다음 request를 보낼 수 있습니다.

## 오류 동작

- 존재하지 않는 route: `404 / Not Found`
- handler에서 예외 발생: `500`, response body에 예외의 `what()` 메시지 포함(`max_message_size`로 제한, `std::exception`이 아닌 throw는 고정 `Internal Server Error`)
- handler가 음수 status 또는 너무 큰 body 반환: `500`, response body에 거부 사유 포함
- malformed/timed-out/disconnected peer: 해당 connection만 종료, server는 계속 실행
- serialized queue에서 server `request_timeout` 초과: handler를 실행하지 않고 `408` 응답
- connection/request deadline 초과: 관찰하는 쪽에서 `ETIMEDOUT`을 가진 `std::system_error`
- 잘못된 로컬 argument/configuration: `std::invalid_argument` 또는 `std::length_error`
- socket/OS 오류: `std::system_error`
- 두 번째 `run()` 등의 잘못된 lifecycle 동작: `std::logic_error`
- 이미 다른 easy-uds Server가 같은 path를 소유: `EADDRINUSE`를 가진 `std::system_error`

## 빌드 및 테스트

필요 사항:

- CMake 3.20+
- C++17 compiler
- POSIX threads

Developer preset 사용:

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

직접 구성:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

주요 CMake 옵션:

- `EASY_UDS_BUILD_EXAMPLES=ON|OFF`
- `EASY_UDS_BUILD_TESTS=ON|OFF`
- `EASY_UDS_BUILD_FUZZERS=ON|OFF`
- `EASY_UDS_BUILD_BENCHMARKS=ON|OFF`
- `EASY_UDS_WARNINGS_AS_ERRORS=ON|OFF`
- `BUILD_SHARED_LIBS=ON|OFF`

`add_subdirectory()`로 다른 프로젝트에 포함하면 examples/tests는 기본적으로 꺼집니다.

### Fuzz target

upstream Clang 사용:

```bash
CC=clang CXX=clang++ cmake -S . -B build-fuzz \
  -DEASY_UDS_BUILD_TESTS=OFF \
  -DEASY_UDS_BUILD_EXAMPLES=OFF \
  -DEASY_UDS_BUILD_FUZZERS=ON
cmake --build build-fuzz --target easy_uds_protocol_fuzz
mkdir -p fuzz-corpus
./build-fuzz/easy_uds_protocol_fuzz fuzz-corpus -runs=20000 -max_len=64
```

### Benchmark

```bash
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release \
  -DEASY_UDS_BUILD_TESTS=OFF \
  -DEASY_UDS_BUILD_EXAMPLES=OFF \
  -DEASY_UDS_BUILD_BENCHMARKS=ON
cmake --build build-bench --parallel

./build-bench/easy_uds_stream_benchmark 1024 65536
./build-bench/easy_uds_rpc_benchmark 100000 8
# 호출자마다 독립 세션
./build-bench/easy_uds_session_benchmark 200000 8
# 하나의 세션을 여러 호출자가 공유
./build-bench/easy_uds_session_benchmark 200000 8 shared
# 워밍업 이후 tiny session RPC의 일반 heap 할당 수
./build-bench/easy_uds_allocation_benchmark 20000
```

세션 벤치마크는 기본적으로 호출자마다 독립 세션을 사용합니다. 마지막 인수로 `shared`를 주면 하나의 세션에서 request-id 멀티플렉싱과 클라이언트 내부 경합을 측정합니다.

session spin window는 latency 실험을 위한 build-time 조정값이며 기본값은 `100` microseconds입니다. `-DEASY_UDS_SESSION_SPIN_US=0`, `10`, `25`, `50`, `100`으로 benchmark 변형을 빌드해 p50/p95/p99, throughput, CPU, context switch를 비교합니다. session benchmark는 `getrusage()`로 user/system CPU 시간과 voluntary/involuntary context switch도 출력합니다. 측정으로 안정적인 정책이 확인되기 전에는 public runtime 옵션으로 노출하지 않습니다. `perf` 또는 `strace`가 있는 환경에서는 같은 benchmark를 감싸 syscall/request, cache miss, branch miss를 추가 측정할 수 있습니다.

## 예제 실행

Server:

```bash
./build/easy_uds_server_example
```

다른 terminal에서 Client:

```bash
./build/easy_uds_client_example
```

## 설치 및 CMake 프로젝트에서 사용

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DEASY_UDS_BUILD_TESTS=OFF \
  -DEASY_UDS_BUILD_EXAMPLES=OFF
cmake --build build --parallel
cmake --install build --prefix /path/to/prefix
```

사용하는 프로젝트:

```cmake
find_package(easy_uds CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE easy_uds::easy_uds)
```

필요하다면 install prefix를 지정합니다.

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/prefix
```

pre-1.0 shared build에서는 minor release 사이 ABI 변경 가능성이 있으므로 ELF `SOVERSION`이 major.minor를 사용합니다. 예: `0.4`.

## Public API

### `easy_uds::Server`

- `Server(std::string socket_path, ServerOptions options = {})`
- `on(std::string route, Handler handler)`
- `on_prefix(std::string prefix, Handler handler)`
- `on_serialized(std::string route, Handler handler)`
- `enqueue_maintenance(std::function<void()> task)`
- `on_stream(std::string route, StreamHandler handler)`
- `on_stream_prefix(std::string prefix, StreamHandler handler)`
- `run()`
- `stop()`
- `is_running()`
- `socket_path()`

### `easy_uds::Client`

- `Client(std::string socket_path, ClientOptions options = {})`
- `request(std::string_view route, std::string_view body = {})`
- `request_stream(std::string_view route, const StreamReader&, response_chunk)`
- `session()`
- `socket_path()`

### `easy_uds::Session`

- `request(std::string_view route, std::string_view body = {})` — 멀티플렉싱, 동시 호출 가능
- `request_stream(std::string_view route, const StreamReader&, response_chunk)` — 독립된 전용 연결

### 데이터 타입

```cpp
using Status = std::int32_t;  // status_ok=200, status_request_timeout=408, status_not_found=404, ...

struct PeerCredentials {
    pid_t pid; uid_t uid; gid_t gid;
    bool present;  // 플랫폼이 자격 증명을 제공하지 못하면 false
};

struct Request {
    std::string route;
    std::string body;
    PeerCredentials peer;
    std::uint32_t request_id;
};

struct Response {
    Status status = 200;
    std::string body;
};

using StreamReader = std::function<std::size_t(char*, std::size_t)>;

struct StreamResponse {
    Status status = 200;
    StreamReader body;
};
```

## 보안 범위

`easy-uds`는 local IPC 라이브러리이며 network security protocol이 아닙니다. 애플리케이션 수준 authentication, authorization, encryption, sandboxing을 제공하지 않습니다.

기본 socket mode는 `0600`입니다. 여러 사용자 또는 권한 경계를 넘는 daemon에서 사용할 경우 socket directory와 권한 정책을 애플리케이션의 trust boundary에 맞게 설계해야 합니다.

instance lock은 같은 pathname을 사용하는 easy-uds server끼리 startup/stale cleanup을 조정하기 위한 것입니다. 이 lock protocol을 무시하는 다른 프로그램까지 강제로 제어하지는 못합니다.

## 저장소 구조

```text
include/easy_uds/       공개 header
src/                    client/server facade와 내부 protocol codec
src/reactor/            reactor core, parser, dispatch, flow control, output,
                        streaming, worker executor
examples/               최소 server/client 예제
tests/easy_uds_test/     기능별로 나눈 unit 테스트
tests/                  stress, fuzz, benchmark, package-consumer 테스트
cmake/                  설치용 CMake config
docs/                   protocol 문서
docs/ROADMAP_0.6.md     0.6.x 기술 실험 및 릴리즈 경계
docs/EXPERIMENTS_0.6.md  독립 UDS 기술 capability probe
docs/PERF_0.6.md         0.6 benchmark 측정 결과와 해석
.github/workflows/      GitHub Actions CI
```

## 라이선스

MIT License. [`LICENSE`](LICENSE)를 참고하십시오.
