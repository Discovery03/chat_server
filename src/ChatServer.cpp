#include "chat/ChatServer.hpp"

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>
#include <utility>

#include "chat/util.hpp"

ChatServer::ChatServer(int port, UserDirectory users)
    : port_(port), users_(std::move(users)), processor_(clients_, groups_) {}

ChatServer::~ChatServer() {
    if (listenFd_ >= 0) ::close(listenFd_);
}

bool ChatServer::start() {
    listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) {
        std::perror("socket failed");
        return false;
    }

    int yes = 1;  // allow quick restarts without "address already in use"
    ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(static_cast<uint16_t>(port_));

    if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        std::perror("bind failed");
        return false;
    }
    if (::listen(listenFd_, SOMAXCONN) < 0) {
        std::perror("listen failed");
        return false;
    }

    util::log("Server is listening on port " + std::to_string(port_) + "...");
    return true;
}


void ChatServer::run() {
    while (true) {
        int fd = ::accept(listenFd_, nullptr, nullptr);
        if (fd < 0) {
            if (errno == EINTR) continue;
            std::perror("accept failed");
            // Avoid a hot loop on persistent errors such as running out of fds.
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        auto session = std::make_shared<ClientSession>(fd);
        std::thread([this, session] { handleClient(session); }).detach();
    }
}


// Asks for credentials. Returns the username on success.
std::optional<std::string> ChatServer::authenticate(ClientSession& session) {
    if (!session.send("Enter username: ")) return std::nullopt;
    auto rawUser = session.receive();
    if (!rawUser) return std::nullopt;

    if (!session.send("Enter password: ")) return std::nullopt;
    auto rawPass = session.receive();
    if (!rawPass) return std::nullopt;

    // Only the first whitespace-delimited token is used (as in the original).
    const auto user = util::split(*rawUser, ' ');
    const auto pass = util::split(*rawPass, ' ');
    if (user.empty() || pass.empty() || !users_.verify(user[0], pass[0])) {
        session.send("Authentication failed. \n");
        return std::nullopt;
    }
    return user[0];
}

void ChatServer::handleClient(SessionPtr session) {
    auto username = authenticate(*session);
    if (!username) return;  // session closes its fd when the last reference goes away

    session->setUsername(*username);
    if (!clients_.add(*username, session)) {
        session->send("Authentication failed. User already logged in.\n");
        return;
    }

    util::log(*username + " connected");
    session->send("Welcome to the chat server !\n");
    clients_.broadcast(*username + " has joined the chat.\n", *username);

    while (auto line = session->receive()) {
        if (!processor_.process(*session, *line)) break;
    }

    clients_.remove(*username);
    groups_.removeUser(*username);
    util::log(*username + " disconnected");
}


