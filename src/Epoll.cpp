#include "chat/Epoll.hpp"

#include <sys/epoll.h>
#include <unistd.h>

#include <cerrno>
#include <stdexcept>

Epoll::Epoll() {
   
    fd_ = epoll_create1(EPOLL_CLOEXEC);
    if(fd_ == -1){
        throw std::runtime_error("epoll_create1 failed inside ");
    }
}   

Epoll::~Epoll() {
  
    if(fd_ != -1){
        close(fd_);
    }
}

bool Epoll::add(int fd, std::uint32_t events) {
  
    struct epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;

    return !epoll_ctl(fd_, EPOLL_CTL_ADD, fd, &ev);
}

bool Epoll::modify(int fd, std::uint32_t events) {
    
    struct epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;

    return !epoll_ctl(fd_, EPOLL_CTL_MOD, fd, &ev);
}

bool Epoll::remove(int fd) {
   
    return !epoll_ctl(fd_, EPOLL_CTL_DEL, fd, nullptr);
}

int Epoll::wait(std::vector<epoll_event>& out, int timeoutMs) {
    int available_fd = epoll_wait(fd_, out.data(), out.size(), timeoutMs) ;
    if(available_fd == -1 && errno == EINTR){
        available_fd = 0;
    }

    return available_fd;
}
