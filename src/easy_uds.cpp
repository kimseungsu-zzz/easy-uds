#include "easy_uds/easy_uds.hpp"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>
#include <iostream>

namespace easy_uds {

Server::Server(const std::string& path) : socket_path(path), running(false) {
    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) throw std::runtime_error("socket creation failed");
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path)-1);
    unlink(socket_path.c_str());
    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(server_fd);
        throw std::runtime_error("bind failed");
    }
    if (listen(server_fd, 5) < 0) {
        close(server_fd);
        throw std::runtime_error("listen failed");
    }
}

Server::~Server() {
    stop();
    unlink(socket_path.c_str());
}

void Server::on(const std::string& route, std::function<Response(const Request&)> handler) {
    if (handlers.find(route) != handlers.end()) throw std::runtime_error("route already exists");
    handlers[route] = handler;
}

void Server::run() {
    running = true;
    while (running) {
        int client_fd = accept(server_fd, nullptr, nullptr);
        if (client_fd < 0) {
            if (!running) break;
            continue;
        }
        std::thread([this, client_fd]() {
            char len_buf[4];
            if (recv(client_fd, len_buf, 4, MSG_WAITALL) != 4) { close(client_fd); return; }
            uint32_t msg_len = 0;
            memcpy(&msg_len, len_buf, 4);
            if (msg_len > 1024*1024) { close(client_fd); return; }
            std::string data(msg_len, '\0');
            if (recv(client_fd, &data[0], msg_len, MSG_WAITALL) != (ssize_t)msg_len) { close(client_fd); return; }
            size_t sep = data.find('\n');
            if (sep == std::string::npos) { close(client_fd); return; }
            std::string route = data.substr(0, sep);
            std::string body = data.substr(sep+1);
            Request req{route, body};
            Response resp;
            auto it = handlers.find(route);
            if (it != handlers.end()) {
                resp = it->second(req);
            } else {
                resp = {404, "Not Found"};
            }
            std::string resp_data = std::to_string(resp.status_code) + "\n" + resp.body;
            uint32_t resp_len = resp_data.size();
            char resp_len_buf[4];
            memcpy(resp_len_buf, &resp_len, 4);
            send(client_fd, resp_len_buf, 4, 0);
            send(client_fd, resp_data.c_str(), resp_len, 0);
            close(client_fd);
        }).detach();
    }
}

void Server::stop() {
    running = false;
    if (server_fd != -1) {
        close(server_fd);
        server_fd = -1;
    }
}

Client::Client(const std::string& path) : socket_path(path) {}

Client::~Client() {}

int Client::connect_to_server() {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path)-1);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

Response Client::request(const std::string& route, const std::string& body) {
    int fd = connect_to_server();
    if (fd < 0) throw std::runtime_error("connection failed");
    std::string msg = route + "\n" + body;
    uint32_t msg_len = msg.size();
    char len_buf[4];
    memcpy(len_buf, &msg_len, 4);
    if (send(fd, len_buf, 4, 0) != 4) { close(fd); throw std::runtime_error("send failed"); }
    if (send(fd, msg.c_str(), msg_len, 0) != (ssize_t)msg_len) { close(fd); throw std::runtime_error("send failed"); }
    char resp_len_buf[4];
    if (recv(fd, resp_len_buf, 4, MSG_WAITALL) != 4) { close(fd); throw std::runtime_error("recv failed"); }
    uint32_t resp_len = 0;
    memcpy(&resp_len, resp_len_buf, 4);
    if (resp_len > 1024*1024) { close(fd); throw std::runtime_error("response too large"); }
    std::string resp_data(resp_len, '\0');
    if (recv(fd, &resp_data[0], resp_len, MSG_WAITALL) != (ssize_t)resp_len) { close(fd); throw std::runtime_error("recv failed"); }
    close(fd);
    size_t sep = resp_data.find('\n');
    if (sep == std::string::npos) throw std::runtime_error("invalid response");
    int status = std::stoi(resp_data.substr(0, sep));
    std::string body_resp = resp_data.substr(sep+1);
    return {status, body_resp};
}

} // namespace easy_uds