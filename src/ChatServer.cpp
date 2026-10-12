#include "chat/ChatServer.hpp"

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <utility>

#include "chat/util.hpp"

ChatServer::ChatServer(int port, UserDirectory users, int numLoops)
    : port_(port), numLoops_(numLoops), users_(std::move(users)) {}

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
    // The listen socket stays BLOCKING: the acceptor thread does nothing but
    // accept(), so it may as well sleep inside accept() until someone connects.

    for (int i = 0; i < numLoops_; ++i) {
        loops_.push_back(std::make_unique<EventLoop>(i, users_, clients_, groups_, *this));
    }
    // One thread per loop. Detached: the server runs until the process is
    // killed (graceful shutdown is part of Step 3).
    for (auto& loop : loops_) {
        EventLoop* l = loop.get();
        std::thread([l] { l->run(); }).detach();
    }

    util::log("Server is listening on port " + std::to_string(port_) + " with " +
              std::to_string(numLoops_) + " event loops...");
    return true;
}

// ===========================================================================
// Acceptor and router
// ===========================================================================

// Runs on the main thread. Its only job: accept connections and hand them out.
void ChatServer::run() {
    while (true) {
        int fd = ::accept(listenFd_, nullptr, nullptr);  // blocks until someone connects
        if (fd < 0) {
            if (errno == EINTR) continue;  // interrupted by a signal: just retry
            std::perror("accept failed");
            // e.g. EMFILE (out of fds): back off briefly instead of spinning at 100% CPU.
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        // Round-robin: connection 0 -> loop 0, 1 -> loop 1, ..., N -> loop 0 again.
        EventLoop* loop = loops_[nextLoop_].get();
        nextLoop_ = (nextLoop_ + 1) % loops_.size();

        // Hand the fd over by POSTING, not by calling loop->adoptConnection(fd)
        // here. adoptConnection inserts into the loop's sessions_ map, which the
        // loop's own thread may be reading at this very moment -> data race.
        // Posting makes the loop's thread do the insert itself.
        //
        // The lambda captures `loop` and `fd` by value (they're just a pointer
        // and an int), because it runs later, on the other thread.
        loop->post([loop, fd]()->void { loop->adoptConnection(fd); });
    }
}

// Runs on the SENDER's loop thread (called from CommandProcessor).
bool ChatServer::deliver(const std::string& username, std::string text) {
    // Which loop owns the receiver? (one short lock inside ClientRegistry)
    EventLoop* loop = clients_.find(username);
    if (loop == nullptr) return false;  // not online

    // Ask THAT loop to deliver it, on its own thread.
    // Everything is captured by value: by the time the task runs, this function
    // has returned and its parameters are gone. `text = std::move(text)` moves
    // the string into the lambda instead of copying it.
    //
    // Note: even when the receiver is on the SAME loop as the sender, we still
    // post. That costs a lock + an eventfd write, but keeps one simple rule.
    // (A possible optimization: deliver directly when loop == current loop.)
    loop->post([loop, username, text = std::move(text)] {
        loop->deliverLocal(username, text);
    });
    return true;
}

// Runs on the sender's loop thread.
void ChatServer::broadcast(std::string text, const std::string& exceptUser) {
    // One post per LOOP, not per user: each loop fans out to its own users.
    // So 10k users on 4 loops = 4 posts, and the 10k sends happen in parallel.
    for (auto& owned : loops_) {
        EventLoop* loop = owned.get();
        // Each task gets its own copy of text/exceptUser (captured by value),
        // since the loops run them independently and at different times.
        loop->post([loop, text, exceptUser] { loop->broadcastLocal(text, exceptUser); });
    }
}
