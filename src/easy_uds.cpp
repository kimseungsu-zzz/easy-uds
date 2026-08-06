#include "easy_uds_fixed.hpp"
#include <iostream>

int main() {
    using namespace easy_uds;

    // 서버 시작 (별도 스레드에서 실행)
    Server server("/tmp/my_socket.sock", [](const std::string& req) {
        std::cout << "[Handler] Received: " << req << std::endl;
        return "Echo: " + req;
    });

    std::thread server_thread([&]() {
        try {
            server.run();
        } catch (const std::exception& e) {
            std::cerr << "Server error: " << e.what() << std::endl;
        }
    });

    // 클라이언트 테스트
    Client client("/tmp/my_socket.sock");
    for (int i = 0; i < 5; ++i) {
        try {
            std::string reply = client.request("Hello " + std::to_string(i));
            std::cout << "[Client] Reply: " << reply << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "[Client] Error: " << e.what() << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    server.stop();
    server_thread.join();
    return 0;
}