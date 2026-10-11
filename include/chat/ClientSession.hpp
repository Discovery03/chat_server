
#pragma once
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include "util.hpp"

constexpr std::size_t kBufferSize = 1024;

class ClientSession {
    public:
        explicit ClientSession(int fd) : fd_(fd) {}
        ~ClientSession() {
            if (fd_ >= 0) ::close(fd_);
        }
        ClientSession(const ClientSession&) = delete;
        ClientSession& operator=(const ClientSession&) = delete;
    
        // Sends the whole message. Several threads may send to the same client
        // (e.g. two people messaging the same user), so writes are serialised.
        // MSG_NOSIGNAL turns "peer closed" into an error instead of a SIGPIPE
        // that would kill the whole server.
        bool send(const std::string& message) {
            std::lock_guard<std::mutex> lock(sendMutex_);
            std::size_t sent = 0;
            while (sent < message.size()) {
                ssize_t n = ::send(fd_, message.data() + sent, message.size() - sent, MSG_NOSIGNAL);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) return false;
                sent += static_cast<std::size_t>(n);
            }
            return true;
        }
    
        // Blocks for the next chunk from the client. nullopt = disconnected/error.
        std::optional<std::string> receive() {
            char buffer[kBufferSize];
            ssize_t n;
            do {
                n = ::recv(fd_, buffer, sizeof(buffer), 0);
            } while (n < 0 && errno == EINTR);
            if (n <= 0) return std::nullopt;
            return util::stripLineEnding(std::string(buffer, static_cast<std::size_t>(n)));
        }
    
        // Set once, by the owning handler thread, before the session is registered.
        void setUsername(std::string name) { username_ = std::move(name); }
        const std::string& username() const { return username_; }
    
    private:
        int fd_;
        std::string username_;
        std::mutex sendMutex_;
};
    