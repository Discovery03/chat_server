// ---------------------------------------------------------------------------
// ClientSession: one connected client on a NON-BLOCKING socket.
//
// With non-blocking I/O nothing waits:
//   - recv() may hand us half a line, or three lines at once. So bytes go into
//     inBuf_ and complete lines are cut out of it ("framing": one message =
//     one line ending in '\n').
//   - send() may accept only part of a message. The rest waits in outBuf_ and
//     is written when epoll says the socket is writable again (EPOLLOUT).
//
// Everything runs on the single event-loop thread, so no locks are needed.
// ---------------------------------------------------------------------------

#pragma once
#include <cstddef>
#include <memory>
#include <optional>
#include <string>

class Epoll;

class ClientSession {
    public:
        // Login is a small state machine instead of a blocking function:
        // each line the client sends moves it one step forward.
        enum class State { AwaitUsername, AwaitPassword, Chatting };

        // Lines longer than this (with no '\n') get the client disconnected,
        // so nobody can make us buffer unlimited memory.
        static constexpr std::size_t kMaxLineLength = 4096;

        ClientSession(int fd, Epoll& epoll);
        ~ClientSession();
        ClientSession(const ClientSession&) = delete;
        ClientSession& operator=(const ClientSession&) = delete;

        int fd() const;

        // Called when epoll reports EPOLLIN. Reads everything currently
        // available into inBuf_. Returns false if the client disconnected,
        // an error occurred, or the line limit was exceeded.
        bool readAvailable();

        // Cuts the next complete line out of inBuf_ (without "\n" / "\r\n").
        // nullopt = no complete line buffered yet.
        std::optional<std::string> nextLine();

        // Queues the message and tries to write it immediately. Never blocks.
        // Returns false if the connection is broken.
        bool send(const std::string& message);

        // Writes as much of outBuf_ as the socket accepts right now. Also
        // called when epoll reports EPOLLOUT. Returns false on a broken connection.
        bool flush();

        // A closed session ignores further sends; the server removes it.
        void markClosed();
        bool closed() const;

        State state() const;
        void setState(State s);

        // Holds the username between the username and password steps.
        void setPendingUsername(std::string name);
        const std::string& pendingUsername() const;

        // Set once login succeeds.
        void setUsername(std::string name);
        const std::string& username() const;

    private:
        int fd_;
        Epoll& epoll_;
        std::string inBuf_;
        std::string outBuf_;
        bool wantWrite_ = false;   // are we currently subscribed to EPOLLOUT?
        bool closed_ = false;
        State state_ = State::AwaitUsername;
        std::string pendingUsername_;
        std::string username_;
};

using SessionPtr = std::shared_ptr<ClientSession>;
