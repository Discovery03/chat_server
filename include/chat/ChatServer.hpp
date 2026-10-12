// ---------------------------------------------------------------------------
// ChatServer: multi-reactor ("one loop per thread").
//
//   main thread:  accept() ──round-robin──► loop 0 │ loop 1 │ ... │ loop N-1
//                                             each: own thread, own epoll,
//                                             owns the clients it was given
//
// It also implements MessageRouter: to reach a user, look up which loop owns
// them in ClientRegistry, and post() the work to that loop.
// ---------------------------------------------------------------------------

#pragma once
#include <memory>
#include <string>
#include <vector>
#include "ClientRegistry.hpp"
#include "EventLoop.hpp"
#include "GroupRegistry.hpp"
#include "MessageRouter.hpp"
#include "UserDirectory.hpp"

class ChatServer : public MessageRouter {
    public:
        ChatServer(int port, UserDirectory users, int numLoops);
        ~ChatServer() override;

        ChatServer(const ChatServer&) = delete;
        ChatServer& operator=(const ChatServer&) = delete;

        // Opens the listening socket and starts the loop threads.
        bool start();
        // Accept loop on the calling thread. Never returns.
        void run();

        // MessageRouter (called from any loop thread):
        bool deliver(const std::string& username, std::string text) override;
        void broadcast(std::string text, const std::string& exceptUser) override;

    private:
        int port_;
        int numLoops_;
        int listenFd_ = -1;
        UserDirectory users_;
        ClientRegistry clients_;
        GroupRegistry groups_;
        std::vector<std::unique_ptr<EventLoop>> loops_;  // unique_ptr: loops never move in memory
        std::size_t nextLoop_ = 0;  // round-robin cursor (only the acceptor thread uses it)
};
