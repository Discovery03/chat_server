#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <utility>

#include "chat/ChatServer.hpp"
#include "chat/UserDirectory.hpp"
#include "chat/util.hpp"

// Usage: chat_server <port> [users_file] [event_loops]
// users_file holds one "username:password" per line (default: users.txt).
// event_loops defaults to the number of CPU cores.
int main(int argc, char* argv[]) {
    if (argc < 2 || argc > 4) {
        std::cerr << "Usage: " << argv[0] << " <port> [users_file] [event_loops]\n";
        return 1;
    }

    auto port = util::parsePort(argv[1]);
    if (!port) {
        std::cerr << "Invalid port: " << argv[1] << " (expected 1-65535)\n";
        return 1;
    }

    const std::string usersPath = argc >= 3 ? argv[2] : "users.txt";
    UserDirectory users;
    if (!users.load(usersPath)) {
        std::cerr << "Could not open users file: " << usersPath << '\n';
        return 1;
    }

    int loops = static_cast<int>(std::thread::hardware_concurrency());
    if (argc == 4) loops = std::atoi(argv[3]);
    if (loops < 1) loops = 1;

    ChatServer server(*port, std::move(users), loops);
    if (!server.start()) return 1;
    server.run();
}
