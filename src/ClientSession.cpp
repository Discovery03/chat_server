#include "chat/ClientSession.hpp"

#include <cassert>
#include <optional>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <utility>

#include "chat/Epoll.hpp"
#include "chat/util.hpp"

namespace {
constexpr std::size_t kBufferSize = 4096;
}

ClientSession::ClientSession(int fd, Epoll &epoll) : fd_(fd), epoll_(epoll) {}

ClientSession::~ClientSession() {
  if (fd_ >= 0)
    ::close(fd_);
}

int ClientSession::fd() const { return fd_; }

// ===========================================================================
// EXERCISE 3 — reading and framing
// ===========================================================================

bool ClientSession::readAvailable() {
  // TODO(you): loop calling ::recv(fd_, buffer, sizeof(buffer), 0) and
  //            append what you get to inBuf_.
  // The three outcomes of recv on a non-blocking socket:
  //   n > 0   -> got n bytes, append and keep looping
  //   n == 0  -> peer closed the connection -> return false
  //   n < 0   -> look at errno:
  //                EAGAIN / EWOULDBLOCK -> nothing more to read now: return
  //                true EINTR                -> just retry
  //                anything else ->
  //                real error: return false
  // Also: if inBuf_ grows beyond kMaxLineLength *without containing a '\n'*,
  //       return false (protects the server from a client that never ends a
  //       line).

  while (true) {

    char buff[kBufferSize];
    ssize_t bytes_read = ::recv(fd_, (void *)(buff),
                                kBufferSize, 0);

    if (bytes_read == 0) {
      return false;
    } else if (bytes_read < 0) {

      switch (errno) {
      case EAGAIN:
        return true;
        break;
      case EINTR: {
        break;
      }
      default: {
        return false;
        break;
      }
      }

    } else {

      inBuf_.append(buff, static_cast<size_t>(bytes_read));

      // Only an unfinished line counts against the limit; many short complete
      // lines arriving together are fine.
      if (inBuf_.size() > kMaxLineLength &&
          inBuf_.find('\n') == std::string::npos) {
        return false;
      }
    }
  }

  return false;
}

std::optional<std::string> ClientSession::nextLine() {
  const size_t newline_pos = inBuf_.find('\n');
  if (newline_pos == std::string::npos)
    return std::nullopt;

  std::string ret = inBuf_.substr(0, newline_pos);
  inBuf_.erase(0, newline_pos + 1);
  return util::stripLineEnding(ret);
}

// ===========================================================================
// EXERCISE 4 — writing with backpressure
// ===========================================================================

bool ClientSession::send(const std::string &message) {
  // TODO(you): if closed_, return false. Otherwise append message to outBuf_
  //            and return flush().

  if (closed_)
    return false;

  outBuf_.append(message.begin(), message.end());
  return flush();
}

bool ClientSession::flush() {

  // TODO(you): while outBuf_ is not empty, ::send() from outBuf_.data() with
  //            MSG_NOSIGNAL (keep that flag, it's why a dead client can't kill
  //            the server) and erase the bytes that were written from the
  //            front.
  //   n < 0 with EAGAIN/EWOULDBLOCK -> kernel send buffer is full; stop looping
  //   n < 0 with EINTR              -> retry
  //   other errors                  -> return false
  //
  // After the loop, keep the EPOLLOUT subscription in sync:
  //   - data still left AND !wantWrite_ -> epoll_.modify(fd_, EPOLLIN |
  //   EPOLLOUT),
  //                                        wantWrite_ = true
  //   - buffer empty    AND  wantWrite_ -> epoll_.modify(fd_, EPOLLIN),
  //                                        wantWrite_ = false
  // Think: why not just stay subscribed to EPOLLOUT all the time?
  //        (Answer is in the comment at the bottom of ChatServer.cpp's run()).

  while (!outBuf_.empty()) {
    int bytes_send = ::send(fd_, outBuf_.data(), outBuf_.size(), MSG_NOSIGNAL);
    if (bytes_send < 0) {

      if(errno == EAGAIN){
        break;
      }
      else if(errno == EINTR){
        continue;
      }
      else{
        return false;
      }
      
      
    } else {
      outBuf_.erase(0, bytes_send);
    }
  }


  if(outBuf_.size() && !wantWrite_){
    epoll_.modify(fd_, EPOLLIN | EPOLLOUT);
    wantWrite_ = true;
  }

  else if(outBuf_.empty() && wantWrite_){
    epoll_.modify(fd_, EPOLLIN);
    wantWrite_ = false;
  }

  return true;
}

void ClientSession::markClosed() { closed_ = true; }
bool ClientSession::closed() const { return closed_; }

ClientSession::State ClientSession::state() const { return state_; }
void ClientSession::setState(State s) { state_ = s; }

void ClientSession::setPendingUsername(std::string name) {
  pendingUsername_ = std::move(name);
}
const std::string &ClientSession::pendingUsername() const {
  return pendingUsername_;
}

void ClientSession::setUsername(std::string name) {
  username_ = std::move(name);
}
const std::string &ClientSession::username() const { return username_; }
