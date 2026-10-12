#include "chat/EventLoop.hpp"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

#include "chat/util.hpp"

// ===========================================================================
// The mailbox and the doorbell
// ===========================================================================

EventLoop::EventLoop(int id, const UserDirectory& users, ClientRegistry& clients,
                     GroupRegistry& groups, MessageRouter& router)
    : id_(id), users_(users), clients_(clients), groups_(groups), router_(router),
      processor_(router, groups, clients) {
    // The doorbell. An eventfd is a file descriptor wrapping a 64-bit counter
    // in the kernel: write() adds to it, read() returns it and resets it to 0,
    // and while it is > 0 the fd counts as "readable".
    //   EFD_NONBLOCK: a read() when nobody rang returns EAGAIN instead of
    //                 blocking our whole loop.
    //   EFD_CLOEXEC:  don't leak the fd into child processes (good hygiene).
    wakeFd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakeFd_ < 0) throw std::runtime_error("eventfd failed");

    // Registering it in OUR epoll is the whole trick: "you've got mail" becomes
    // just another event that wakes epoll_wait, exactly like socket data does.
    // No polling, no timeouts.
    if (!epoll_.add(wakeFd_, EPOLLIN)) {
        ::close(wakeFd_);
        throw std::runtime_error("epoll add (eventfd) failed");
    }
}

EventLoop::~EventLoop() {
    if (wakeFd_ >= 0) ::close(wakeFd_);
}

int EventLoop::id() const { return id_; }

// Called from ANY thread (other loops, the acceptor, or this loop itself).
void EventLoop::post(Task task) {
    // 1. Put the task in the mailbox. The lock is held only for one push_back.
    {
        std::lock_guard<std::mutex> lock(mailboxMutex_);
        mailbox_.push_back(std::move(task));
    }

    // 2. Ring the doorbell. eventfd only accepts exactly 8 bytes (a uint64),
    //    and ADDS the value to its counter.
    //
    //    Why push first, ring second? If we rang first, the loop could wake up,
    //    find the mailbox still empty, and go back to sleep — and then our task
    //    sits there until something else happens to wake the loop. Pushing first
    //    guarantees the task is already there when the loop looks.
    std::uint64_t one = 1;
    ssize_t n = ::write(wakeFd_, &one, sizeof(one));
    (void)n;  // can only fail if the counter would overflow 2^64-2: not a real concern
}

// Runs on THIS loop's thread, when epoll reports wakeFd_ readable.
void EventLoop::drainMailbox() {
    // Reset the doorbell. The value read = how many times it was rung since
    // last time; we don't need it, because we take EVERYTHING in the mailbox.
    // (Several posts can share one wake-up: that batching is a nice side effect.)
    std::uint64_t rings = 0;
    ssize_t n = ::read(wakeFd_, &rings, sizeof(rings));
    (void)n;  // EAGAIN just means it was already reset: harmless

    // Take the whole queue out under the lock, then run the tasks WITHOUT it.
    // Why not run them while holding the lock?
    //  a) Other threads calling post() on us would be blocked for the entire
    //     time our tasks take (sending to hundreds of users...).
    //  b) Deadlock: a task may itself call post() on THIS loop (e.g. a join
    //     broadcast posts to every loop, including the current one). post()
    //     would try to lock mailboxMutex_ again from the same thread, and
    //     std::mutex is not re-entrant -> the thread waits for itself forever.
    std::vector<Task> tasks;
    {
        std::lock_guard<std::mutex> lock(mailboxMutex_);
        std::swap(tasks, mailbox_);  // O(1): just swaps the vectors' internal pointers
    }
    for (Task& task : tasks) task();
}

// ===========================================================================
// The per-loop event loop
// ===========================================================================

void EventLoop::run() {
    std::vector<epoll_event> events(64);
    while (true) {
        int numEvents = epoll_.wait(events);
        if (numEvents < 0) {
            std::perror("epoll_wait");
            continue;
        }

        for (int i = 0; i < numEvents; ++i) {
            const int fd = events[i].data.fd;
            const std::uint32_t ev = events[i].events;

            // Mail from another thread (new connection, message to deliver, ...).
            if (fd == wakeFd_) {
                drainMailbox();
                continue;
            }

            // Otherwise it's one of our clients. Same logic as the single-reactor
            // server. Copy the SessionPtr: closeSession() erases the map entry,
            // and the copy keeps the session alive until we're done with it.
            auto it = sessions_.find(fd);
            if (it == sessions_.end()) continue;  // closed earlier in this batch
            SessionPtr session = it->second;

            if (ev & (EPOLLERR | EPOLLHUP)) {
                closeSession(session);
                continue;
            }
            if (ev & EPOLLIN) {
                onReadable(session);
            }
            // onReadable may have closed (and erased) the session already.
            if ((ev & EPOLLOUT) && sessions_.count(fd)) {
                if (!session->flush()) closeSession(session);
            }
        }
    }
}

// Runs on THIS loop's thread: the acceptor posted it to us.
void EventLoop::adoptConnection(int fd) {
    // The acceptor's listen socket is blocking, but OUR client sockets must not
    // be: one slow client must never stall the whole loop.
    if (!util::setNonBlocking(fd)) {
        std::perror("setNonBlocking");
        ::close(fd);
        return;
    }
    if (!epoll_.add(fd, EPOLLIN)) {
        std::perror("epoll add (client)");
        ::close(fd);
        return;
    }

    // From here on this connection belongs to this loop for its whole life.
    // The session gets OUR epoll, so its flush() toggles EPOLLOUT in the right place.
    auto session = std::make_shared<ClientSession>(fd, epoll_);
    sessions_.emplace(fd, session);
    session->send("Enter username: ");
}

// Runs on THIS loop's thread: posted by ChatServer::deliver() on some loop.
void EventLoop::deliverLocal(const std::string& username, const std::string& text) {
    // Look the user up by NAME, now, at delivery time. Between the post() and
    // now, the user may have disconnected; then they're simply not in byName_
    // and the message is dropped.
    //
    // Why not address messages by fd? If bob disconnects and the kernel reuses
    // his fd number for a new client (say carol), a "deliver to fd 12" message
    // would reach carol. Names don't get "reused" like that.
    auto it = byName_.find(username);
    if (it == byName_.end()) return;
    it->second->send(text);
}

// Runs on THIS loop's thread: posted by ChatServer::broadcast(), once per loop.
void EventLoop::broadcastLocal(const std::string& text, const std::string& exceptUser) {
    // Every loop does this for ITS OWN users, so a broadcast to 10k users costs
    // one post per loop, and the 10k send() calls are spread over all loops.
    for (const auto& [name, session] : byName_) {
        if (name != exceptUser) session->send(text);
    }
}

// ---------------------------------------------------------------------------
// Moved from the single-reactor ChatServer.
// ---------------------------------------------------------------------------

void EventLoop::onReadable(const SessionPtr& session) {
    if(!session->readAvailable()) {
        closeSession(session);
        return;
    }

    while(!session->closed()){
        auto line = session->nextLine();
        if(line == std::nullopt) break;
        handleLine(*session, *line);
    }

    if(session->closed()){
        session->flush();  // send any outgoin messages.
        closeSession(session);
    }
}

void EventLoop::handleLine(ClientSession& session, const std::string& line) {
    switch (session.state()) {

        case ClientSession::State::AwaitUsername : {
            auto vec = util::split(line, ' ');
            if(vec.size() == 0){
                session.send("Enter username: ");
                break;
            }
            session.setPendingUsername(vec[0]);
            session.send("Enter password: ");
            session.setState(ClientSession::State::AwaitPassword);
            break;
        }

        case ClientSession::State::AwaitPassword : {
            auto vec = util::split(line, ' ');
            if(vec.size() == 0){
                session.send("Enter password: ");
                break;
            }

            if(!users_.verify(session.pendingUsername(), vec[0])){
                session.send("Authentication failed.\n");
                session.markClosed();
                break;
            }

            session.setUsername(session.pendingUsername());

            // Register in the GLOBAL registry: "this user lives on this loop".
            // add() is check-and-insert under one lock, so if the same name logs
            // in on two loops at the same moment, exactly one of them wins.
            if(!clients_.add(session.username(), this)){
                session.send("Authentication failed. User already logged in.\n");
                session.markClosed();
                break;
            }
            // ...and in OUR local map, so deliverLocal/broadcastLocal can find
            // them by name. byName_ needs the shared_ptr, which lives in sessions_.
            byName_[session.username()] = sessions_.at(session.fd());

            session.setState(ClientSession::State::Chatting);

            const auto &username = session.username();
            util::log(username + " connected");
            session.send("Welcome to the chat server !\n");

            // No "<name> has joined the chat" broadcast: pushing it to every
            // online user made each login O(N) and N logins O(N^2). Users ask
            // with /online instead (see CommandProcessor::online).
            break;
        }

        case ClientSession::State::Chatting : {
            if(!processor_.process(session, line)){
                session.markClosed();
            }
            break;
        }
    }
}

void EventLoop::closeSession(const SessionPtr& session) {
    auto fd = session->fd();
    epoll_.remove(fd);

    if(session->state() == ClientSession::State::Chatting){
        // Undo both registrations from login. After this, deliver() to this
        // name returns false, and any message already posted for them is
        // dropped by deliverLocal (not in byName_ any more).
        byName_.erase(session->username());
        clients_.remove(session->username());
        groups_.removeUser(session->username());
        util::log(session->username() + " got disconnected.");
    }

    session->markClosed();
    sessions_.erase(fd);
}
