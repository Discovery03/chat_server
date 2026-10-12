// ---------------------------------------------------------------------------
// Epoll: thin RAII wrapper around the epoll syscalls.
// Owns the epoll fd and closes it on destruction. It does NOT own the fds it
// watches; whoever adds an fd is responsible for removing and closing it.
// ---------------------------------------------------------------------------

#pragma once
#include <sys/epoll.h>

#include <cstdint>
#include <vector> 

class Epoll {
    public:
        Epoll();   // creates the epoll instance (throws std::runtime_error on failure)
        ~Epoll();
        Epoll(const Epoll&) = delete;
        Epoll& operator=(const Epoll&) = delete;

        // Start / change / stop watching `fd`. `events` is a mask such as
        // EPOLLIN or EPOLLIN | EPOLLOUT. Return false on failure.
        bool add(int fd, std::uint32_t events);
        bool modify(int fd, std::uint32_t events);
        bool remove(int fd);

        // Blocks until at least one watched fd is ready (or timeoutMs passes;
        // -1 = wait forever). Fills `out` with the ready events and returns how
        // many there are, or -1 on error.
        int wait(std::vector<epoll_event>& out, int timeoutMs = -1);

    private:
        int fd_ = -1;
};
