#include "chat/ClientSession.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <utility>

#include "chat/util.hpp"

namespace {
constexpr std::size_t kBufferSize = 1024;
}

ClientSession::ClientSession(int fd) : fd_(fd) {}

ClientSession::~ClientSession() {
    if (fd_ >= 0) ::close(fd_);
}

// MSG_NOSIGNAL turns "peer closed" into an error instead of a SIGPIPE
// that would kill the whole server.
bool ClientSession::send(const std::string& message) {
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

std::optional<std::string> ClientSession::receive() {
    char buffer[kBufferSize];
    ssize_t n;
    do {
        n = ::recv(fd_, buffer, sizeof(buffer), 0);
    } while (n < 0 && errno == EINTR);
    if (n <= 0) return std::nullopt;
    return util::stripLineEnding(std::string(buffer, static_cast<std::size_t>(n)));
}

void ClientSession::setUsername(std::string name) { username_ = std::move(name); }

const std::string& ClientSession::username() const { return username_; }
