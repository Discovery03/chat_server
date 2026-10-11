
// ---------------------------------------------------------------------------
// ChatServer: owns the listening socket and the per-client lifecycle.
// ---------------------------------------------------------------------------

#pragma once
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include "util.hpp"
#include "ClientSession.hpp"
#include "UserDirectory.hpp"
#include "ClientRegistry.hpp"
#include "GroupRegistry.hpp"
#include "CommandProcessor.hpp"


using SessionPtr = std::shared_ptr<ClientSession>; 
class ChatServer {
    public:
        ChatServer(int port, UserDirectory users)
            : port_(port), users_(std::move(users)), processor_(clients_, groups_) {}
    
        ~ChatServer() {
            if (listenFd_ >= 0) ::close(listenFd_);
        }

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
    