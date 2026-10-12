#include "chat/ChatServer.hpp"

#include <cassert>
#include <cstddef>
#include <netinet/in.h>
#include <optional>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <utility>
#include <vector>

#include "chat/ClientSession.hpp"
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

    // TODO(you) EXERCISE 5a: make listenFd_ non-blocking and add it to epoll_
    //           with EPOLLIN ("readable" on a listen socket = "a connection is
    //           waiting to be accepted"). Return false (with a perror) if either fails.

    if(!util::setNonBlocking(listenFd_)){
        perror("Not able to make listen_fd nonblocking");
        return false;
    }

    epoll_.add(listenFd_, EPOLLIN);

    util::log("Server is listening on port " + std::to_string(port_) + "...");
    return true;
}

// ===========================================================================
// EXERCISE 5 — the event loop
// ===========================================================================

void ChatServer::run() {
    std::vector<epoll_event> events(64);
    while (true) {
        // TODO(you) 5b: n = epoll_.wait(events). On -1, perror and continue.
        //   Then for each of the n events:
        //     int fd = events[i].data.fd;   std::uint32_t ev = events[i].events;
        //
        //     fd == listenFd_       -> acceptClients()
        //     otherwise             -> look the fd up in sessions_ (skip if missing)
        //                              and COPY the pointer: SessionPtr s = it->second;
        //                              (the copy keeps it alive if closeSession erases it)
        //       ev has EPOLLERR or EPOLLHUP -> closeSession, next event
        //       ev has EPOLLIN              -> onReadable(session)
        //       ev has EPOLLOUT             -> if !session->flush(): closeSession
        //
        //   Careful: onReadable may already have closed the session, so check
        //   whether the fd is still in sessions_ before handling EPOLLOUT.
        //
        // About EPOLLOUT (re: the question in ClientSession::flush): epoll here is
        // level-triggered, and a socket is "writable" whenever its send buffer has
        // room — i.e. almost always. Subscribing to EPOLLOUT permanently would make
        // epoll_wait return immediately forever -> 100% CPU doing nothing. So we
        // only ask for it while we actually have unsent bytes.

        int numEvents = epoll_.wait(events);
        if(numEvents < 0){
            perror("epoll_wait returned negative value");
        }

        for(int i = 0 ; i < numEvents ; i++){

            int fd = events[i].data.fd;
            auto ev = events[i].events;

            if(fd == listenFd_){
                assert(ev & EPOLLIN);
                acceptClients();
            }
            else{

                auto it = sessions_.find(fd);
                if(it == sessions_.end()) continue;
                auto s = it->second;

                if( (ev & EPOLLERR) or (ev & EPOLLHUP)) {
                    closeSession(s);
                    continue;
                }

                if (ev & EPOLLIN){
                    onReadable(s);
                }

                // onReadable may have closed and erased the session already.
                if((ev & EPOLLOUT) && sessions_.count(fd)){
                    if (!s->flush()) closeSession(s);
                }
                 
            }
        }
        
    }
}

void ChatServer::acceptClients() {
    // TODO(you) 5c: loop on ::accept(listenFd_, nullptr, nullptr):
    //   fd < 0 and errno EAGAIN/EWOULDBLOCK -> no more pending connections: return
    //   fd < 0 and errno EINTR              -> retry
    //   fd < 0 otherwise                    -> perror and return
    //   fd >= 0:
    //     1. util::setNonBlocking(fd)   (if it fails: ::close(fd), continue)
    //     2. epoll_.add(fd, EPOLLIN)    (same)
    //     3. make the session: std::make_shared<ClientSession>(fd, epoll_)
    //        and store it in sessions_[fd]
    //     4. send it the first prompt: "Enter username: "
    //
    // Why a loop and not one accept()? Several clients may connect between two
    // epoll_wait calls, and we'd only be told once.

    while(true){
        int fd = ::accept(listenFd_, nullptr, nullptr);
        if(fd < 0){

            if(errno == EAGAIN || errno == EWOULDBLOCK){
                return;
            }
            if(errno != EINTR){
                perror("error in acceptClients");
                return;
            }
            continue;
        }

        if(!util::setNonBlocking(fd)){
            perror("setNonBlocking failed in accept client");
            close(fd);
            continue;
        }

        if(!epoll_.add(fd, EPOLLIN)){
            perror("epoll add failed in AcceptClient");
            close(fd);
            continue;
        }

        sessions_.emplace(fd, std::make_shared<ClientSession>(fd,epoll_));
        sessions_[fd]->send("Enter username: ");
    }
}

void ChatServer::onReadable(const SessionPtr& session) {
    // TODO(you) 5d:
    //   1. if !session->readAvailable() -> closeSession(session), return
    //   2. while (auto line = session->nextLine()) handleLine(*session, *line);
    //      (but stop early if the session got marked closed)
    //   3. if the session is closed now (e.g. /exit or failed login),
    //      try one last flush() so it sees the final message, then closeSession.

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

void ChatServer::handleLine(ClientSession& session, const std::string& line) {
    // TODO(you) 5e: this replaces the old blocking authenticate()+handleClient().
    // Switch on session.state():
    //
    //   AwaitUsername:
    //     take the first space-separated token of line (util::split), store it with
    //     setPendingUsername, send "Enter password: ", move to AwaitPassword.
    //     (Empty line? Just ask "Enter username: " again.)
    //
    //   AwaitPassword:
    //     first token of line is the password. If users_.verify fails:
    //       send "Authentication failed.\n", markClosed(), return.
    //     setUsername(pendingUsername). clients_.add(name, sessions_.at(session.fd()))
    //       — the registry needs the shared_ptr, which is why we look it up.
    //     If add fails: send "Authentication failed. User already logged in.\n",
    //       markClosed(), return.
    //     Otherwise: setState(Chatting), log "<name> connected", send the welcome,
    //       broadcast "<name> has joined the chat.\n" (the old handleClient has the
    //       exact strings).
    //
    //   Chatting:
    //     if !processor_.process(session, line) -> markClosed()   (that's /exit)

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
            if(!clients_.add(session.username(), sessions_.at(session.fd()))){
                session.send("Authentication failed. User already logged in.\n");
                session.markClosed();
                break;
            }

            session.setState(ClientSession::State::Chatting);

            const auto &username = session.username();
            util::log(username + " connected");
            session.send("Welcome to the chat server !\n");
            clients_.broadcast(username + " has joined the chat.\n", username);
           
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

void ChatServer::closeSession(const SessionPtr& session) {
    // TODO(you) 5f: undo everything acceptClients + login did:
    //   - epoll_.remove(fd)  — do this while the fd is still open
    //   - if the user had logged in (state == Chatting): clients_.remove,
    //     groups_.removeUser, log "<name> disconnected".
    //     (Why check the state? Think of the "already logged in" case: you must
    //      not remove the OTHER, real session that owns that name.)
    //   - session->markClosed()
    //   - sessions_.erase(fd)
    // The fd itself gets closed by ~ClientSession when the last SessionPtr dies.
    // Note `session` is a reference, possibly to the very shared_ptr inside
    // sessions_ — copy the fd/name you need into locals BEFORE erasing.
    auto fd = session->fd();
    epoll_.remove(fd);

    if(session->state() == ClientSession::State::Chatting){
        clients_.remove(session->username());
        groups_.removeUser(session->username());
        util::log(session->username() + " got disconnected.");
    }

    session->markClosed();
    sessions_.erase(fd);

}
