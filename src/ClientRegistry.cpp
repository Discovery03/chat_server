#include "chat/ClientRegistry.hpp"

bool ClientRegistry::add(const std::string& username, EventLoop* owner) {
    std::lock_guard<std::mutex> lock(mutex_);
    return owners_.emplace(username, owner).second;
}

void ClientRegistry::remove(const std::string& username) {
    std::lock_guard<std::mutex> lock(mutex_);
    owners_.erase(username);
}

EventLoop* ClientRegistry::find(const std::string& username) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = owners_.find(username);
    return it == owners_.end() ? nullptr : it->second;
}
