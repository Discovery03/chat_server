// ---------------------------------------------------------------------------
// ChatServer: owns the listening socket and the per-client lifecycle.
// ---------------------------------------------------------------------------

#pragma once
#include <optional>
#include <string>
#include "ClientRegistry.hpp"
#include "ClientSession.hpp"
#include "CommandProcessor.hpp"
#include "GroupRegistry.hpp"
#include "UserDirectory.hpp"

class ChatServer {
    public:
        ChatServer(int port, UserDirectory users);
        ~ChatServer();

        ChatServer(const ChatServer&) = delete;
        ChatServer& operator=(const ChatServer&) = delete;

        bool start();
        // Accept loop: one detached thread per client.
        void run();

    private:
        // Asks for credentials. Returns the username on success.
        std::optional<std::string> authenticate(ClientSession& session);
        void handleClient(SessionPtr session);

        int port_;
        int listenFd_ = -1;
        UserDirectory users_;
        ClientRegistry clients_;
        GroupRegistry groups_;
        CommandProcessor processor_;  // declared last: it holds references to clients_ / groups_
};
