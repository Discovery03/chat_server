// ---------------------------------------------------------------------------
// ChatServer: owns the listening socket and runs a single-threaded epoll
// event loop ("reactor"). One thread serves every client: it sleeps in
// epoll_wait until some socket is ready, handles exactly that, and goes back.
// ---------------------------------------------------------------------------

#pragma once
#include <string>
#include <unordered_map>
#include "ClientRegistry.hpp"
#include "ClientSession.hpp"
#include "CommandProcessor.hpp"
#include "Epoll.hpp"
#include "GroupRegistry.hpp"
#include "UserDirectory.hpp"

class ChatServer {
    public:
        ChatServer(int port, UserDirectory users);
        ~ChatServer();

        ChatServer(const ChatServer&) = delete;
        ChatServer& operator=(const ChatServer&) = delete;

        bool start();
        // Event loop. Never returns.
        void run();

    private:
        // Accepts every pending connection on the (non-blocking) listen socket.
        void acceptClients();
        // EPOLLIN on a client: read, then handle each complete line.
        void onReadable(const SessionPtr& session);
        // One complete line from a client, routed by its login state.
        void handleLine(ClientSession& session, const std::string& line);
        // Unregisters the session everywhere. Its fd closes when the last
        // SessionPtr goes away.
        void closeSession(const SessionPtr& session);

        int port_;
        int listenFd_ = -1;
        Epoll epoll_;
        UserDirectory users_;
        ClientRegistry clients_;   // logged-in users, by name
        GroupRegistry groups_;
        CommandProcessor processor_;  // declared after clients_ / groups_: it holds references to them
        std::unordered_map<int, SessionPtr> sessions_;  // every connection (logged in or not), by fd
};
