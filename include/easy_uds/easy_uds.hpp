#pragma once

#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <functional>
#include <system_error>
#include <cstring>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
    #pragma comment(lib, "ws2_32.lib")
    using socket_t = SOCKET;
    #define INVALID_SOCKET_VAL INVALID_SOCKET
    #define SOCKET_ERROR_VAL SOCKET_ERROR
    #define close_socket(s) closesocket(s)
    #define unlink_file(p) _unlink(p)
#else
    #include <sys/socket.h>
    #include <sys/un.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>
    using socket_t = int;
    #define INVALID_SOCKET_VAL (-1)
    #define SOCKET_ERROR_VAL (-1)
    #define close_socket(s) ::close(s)
    #define unlink_file(p) ::unlink(p)
#endif

#include <string>
#include <chrono>
#include <algorithm>

namespace easy_uds {

// ---- 예외 정의 ----
class UDSException : public std::runtime_error {
public:
    explicit UDSException(const std::string& msg) : std::runtime_error(msg) {}
};

// ---- 유틸리티: 논블로킹 설정 (선택) ----
static bool set_nonblocking(socket_t fd) {
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(fd, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
#endif
}

// ---- 안전한 send/recv 래퍼 (부분 I/O + EINTR 처리) ----
static ssize_t safe_send(socket_t fd, const void* buf, size_t len, int flags = 0) {
    const char* data = static_cast<const char*>(buf);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, data + sent, len - sent, flags);
        if (n < 0) {
#ifdef _WIN32
            int err = WSAGetLastError();
            if (err == WSAEINTR) continue;
            throw UDSException("send failed: " + std::to_string(err));
#else
            if (errno == EINTR) continue;
            throw UDSException("send failed: " + std::string(strerror(errno)));
#endif
        }
        if (n == 0) break;  // 상대방 종료
        sent += n;
    }
    return sent;
}

static ssize_t safe_recv(socket_t fd, void* buf, size_t len, int flags = 0) {
    char* data = static_cast<char*>(buf);
    size_t received = 0;
    while (received < len) {
        ssize_t n = ::recv(fd, data + received, len - received, flags);
        if (n < 0) {
#ifdef _WIN32
            int err = WSAGetLastError();
            if (err == WSAEINTR) continue;
            throw UDSException("recv failed: " + std::to_string(err));
#else
            if (errno == EINTR) continue;
            throw UDSException("recv failed: " + std::string(strerror(errno)));
#endif
        }
        if (n == 0) break;  // 클라이언트 정상 종료
        received += n;
    }
    return received;
}

// ---- Server 클래스 ----
class Server {
public:
    using Handler = std::function<std::string(const std::string& request)>;

    Server(const std::string& socket_path, Handler handler)
        : socket_path_(socket_path), handler_(handler), running_(false) {
        // 기존 파일 제거 (bind 실패 방지)
        unlink_file(socket_path_.c_str());
    }

    ~Server() {
        stop();
        if (server_fd_ != INVALID_SOCKET_VAL) {
            close_socket(server_fd_);
            server_fd_ = INVALID_SOCKET_VAL;
        }
        unlink_file(socket_path_.c_str());
    }

    void run() {
        if (running_) return;
        server_fd_ = create_server_socket();
        if (server_fd_ == INVALID_SOCKET_VAL) {
            throw UDSException("failed to create server socket");
        }

        running_ = true;
        std::cout << "[Server] Listening on " << socket_path_ << std::endl;

        while (running_) {
            socket_t client_fd = accept_client(server_fd_);
            if (!running_) break;
            if (client_fd == INVALID_SOCKET_VAL) continue;

            // 각 클라이언트를 별도 스레드에서 처리
            std::thread(&Server::handle_client, this, client_fd).detach();
        }
    }

    void stop() {
        running_ = false;
        // accept 블로킹을 깨우기 위해 dummy 연결 시도 (선택)
        // 간단히 서버 소켓을 닫아도 되지만, 여기서는 플래그만 사용
    }

private:
    socket_t create_server_socket() {
#ifdef _WIN32
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2,2), &wsaData);
#endif
        socket_t fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd == INVALID_SOCKET_VAL) return INVALID_SOCKET_VAL;

        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

        if (::bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
            close_socket(fd);
            return INVALID_SOCKET_VAL;
        }

        if (::listen(fd, SOMAXCONN) != 0) {
            close_socket(fd);
            return INVALID_SOCKET_VAL;
        }
        return fd;
    }

    socket_t accept_client(socket_t server_fd) {
        struct sockaddr_un client_addr;
        socklen_t addr_len = sizeof(client_addr);
        socket_t client_fd = ::accept(server_fd, (struct sockaddr*)&client_addr, &addr_len);
        if (client_fd == INVALID_SOCKET_VAL) {
#ifdef _WIN32
            int err = WSAGetLastError();
            if (err != WSAEWOULDBLOCK && err != WSAEINTR)
#else
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
#endif
                std::cerr << "[Server] accept error" << std::endl;
        }
        return client_fd;
    }

    void handle_client(socket_t client_fd) {
        try {
            // 1. 요청 길이 수신 (4바이트 big-endian)
            uint32_t req_len = 0;
            ssize_t n = safe_recv(client_fd, &req_len, sizeof(req_len), 0);
            if (n != sizeof(req_len)) {
                close_socket(client_fd);
                return;
            }
            req_len = ntohl(req_len);
            if (req_len == 0 || req_len > 1024*1024) { // 1MB 제한
                close_socket(client_fd);
                return;
            }

            // 2. 요청 본문 수신
            std::vector<char> req_buf(req_len);
            n = safe_recv(client_fd, req_buf.data(), req_len, 0);
            if (n != req_len) {
                close_socket(client_fd);
                return;
            }
            std::string request(req_buf.data(), req_len);

            // 3. 핸들러 호출 (사용자 로직)
            std::string response = handler_(request);

            // 4. 응답 길이 전송
            uint32_t res_len = htonl(static_cast<uint32_t>(response.size()));
            safe_send(client_fd, &res_len, sizeof(res_len), 0);
            // 5. 응답 본문 전송
            safe_send(client_fd, response.data(), response.size(), 0);

        } catch (const std::exception& e) {
            std::cerr << "[Server] client handler error: " << e.what() << std::endl;
        }
        close_socket(client_fd);
    }

private:
    std::string socket_path_;
    Handler handler_;
    std::atomic<bool> running_;
    socket_t server_fd_ = INVALID_SOCKET_VAL;
};

// ---- Client 클래스 ----
class Client {
public:
    Client(const std::string& socket_path) : socket_path_(socket_path) {}

    std::string request(const std::string& message, std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
        socket_t fd = connect_server();
        if (fd == INVALID_SOCKET_VAL) {
            throw UDSException("connection failed");
        }

        try {
            // 1. 요청 길이 전송
            uint32_t req_len = htonl(static_cast<uint32_t>(message.size()));
            safe_send(fd, &req_len, sizeof(req_len), 0);
            safe_send(fd, message.data(), message.size(), 0);

            // 2. 응답 길이 수신 (타임아웃 적용 안 함, 여기선 단순)
            uint32_t res_len = 0;
            ssize_t n = safe_recv(fd, &res_len, sizeof(res_len), 0);
            if (n != sizeof(res_len)) {
                close_socket(fd);
                throw UDSException("failed to read response length");
            }
            res_len = ntohl(res_len);
            if (res_len == 0 || res_len > 1024*1024) {
                close_socket(fd);
                throw UDSException("invalid response length");
            }

            std::vector<char> res_buf(res_len);
            n = safe_recv(fd, res_buf.data(), res_len, 0);
            if (n != res_len) {
                close_socket(fd);
                throw UDSException("incomplete response");
            }
            close_socket(fd);
            return std::string(res_buf.data(), res_len);

        } catch (...) {
            close_socket(fd);
            throw;
        }
    }

private:
    socket_t connect_server() {
#ifdef _WIN32
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2,2), &wsaData);
#endif
        socket_t fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd == INVALID_SOCKET_VAL) return INVALID_SOCKET_VAL;

        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

        if (::connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
            close_socket(fd);
            return INVALID_SOCKET_VAL;
        }
        return fd;
    }

private:
    std::string socket_path_;
};

} // namespace easy_uds