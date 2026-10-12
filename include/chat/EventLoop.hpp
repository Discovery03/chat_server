// ---------------------------------------------------------------------------
// EventLoop: one thread, one epoll, and the clients that thread OWNS.
//
// OWNERSHIP RULE: a ClientSession is only ever touched by the loop that owns
// it. No other thread reads or writes it, so sessions need no locks.
//
// How do other threads get work to this loop, then? They post() a task.
// post() puts the task in this loop's MAILBOX (a mutex-protected queue) and
// rings its DOORBELL (an eventfd registered in this loop's epoll). The loop
// wakes up from epoll_wait, empties the mailbox, and runs the tasks on its
// own thread — where touching its sessions is safe.
//
//   Loop 0 thread                         Loop 1 thread (in epoll_wait)
//   loop1->post(task) ──► mailbox_ ──┐
//                    └──► write(wakeFd_) ──► wakes: drainMailbox() → task()
//
// Thread-safety of the methods:
//   post()               — ANY thread.
//   everything else      — ONLY this loop's own thread (usually from inside a
//                          posted task, e.g. post([loop, fd]{ loop->adoptConnection(fd); })).
// ---------------------------------------------------------------------------

#pragma once
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "ClientRegistry.hpp"
#include "ClientSession.hpp"
#include "CommandProcessor.hpp"
#include "Epoll.hpp"
#include "GroupRegistry.hpp"
#include "MessageRouter.hpp"
#include "UserDirectory.hpp"

class EventLoop {
    public:
        using Task = std::function<void()>;

        EventLoop(int id, const UserDirectory& users, ClientRegistry& clients,
                  GroupRegistry& groups, MessageRouter& router);
        ~EventLoop();
        EventLoop(const EventLoop&) = delete;
        EventLoop& operator=(const EventLoop&) = delete;

        int id() const;

        // The event loop. Never returns. Run it on the loop's own thread.
        void run();

        // ANY thread: queue `task` to run on this loop's thread, and wake it.
        void post(Task task);

        // ---- Loop thread only (call them from inside a posted task) --------

        // A freshly accepted client socket now belongs to this loop.
        void adoptConnection(int fd);

        // Sends `text` to `username` if they are logged in on THIS loop.
        void deliverLocal(const std::string& username, const std::string& text);

        // Sends `text` to every user logged in on THIS loop except `exceptUser`.
        void broadcastLocal(const std::string& text, const std::string& exceptUser);

    private:
        // Runs every task currently in the mailbox.
        void drainMailbox();

        // Same jobs as in the single-reactor ChatServer.
        void onReadable(const SessionPtr& session);
        void handleLine(ClientSession& session, const std::string& line);
        void closeSession(const SessionPtr& session);

        int id_;
        Epoll epoll_;
        int wakeFd_ = -1;  // the eventfd "doorbell"

        std::mutex mailboxMutex_;     // guards mailbox_ — the only shared member
        std::vector<Task> mailbox_;

        // Owned by this loop's thread only, so no locks:
        std::unordered_map<int, SessionPtr> sessions_;          // every connection, by fd
        std::unordered_map<std::string, SessionPtr> byName_;    // logged-in users, by name

        const UserDirectory& users_;  // read-only after startup: safe to share
        ClientRegistry& clients_;     // shared, has its own lock
        GroupRegistry& groups_;       // shared, has its own lock
        MessageRouter& router_;       // reaches users on OTHER loops
        CommandProcessor processor_;  // per loop; declared after router_/groups_ it refers to
};
