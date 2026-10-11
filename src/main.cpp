#include <iostream>
#include <string>
#include <utility>

#include "chat/ChatServer.hpp"
#include "chat/UserDirectory.hpp"
#include "chat/util.hpp"

// Usage: chat_server <port> [users_file]
// users_file holds one "username:password" per line (default: users.txt).
int main(int argc, char* argv[]) {
    if (argc < 2 || argc > 3) {
        std::cerr << "Usage: " << argv[0] << " <port> [users_file]\n";
        return 1;
    }

    auto port = util::parsePort(argv[1]);
    if (!port) {
        std::cerr << "Invalid port: " << argv[1] << " (expected 1-65535)\n";
        return 1;
    }

    const std::string usersPath = argc == 3 ? argv[2] : "users.txt";
    UserDirectory users;
    if (!users.load(usersPath)) {
        std::cerr << "Could not open users file: " << usersPath << '\n';
        return 1;
    }

    ChatServer server(*port, std::move(users));
    if (!server.start()) return 1;
    server.run();
}
