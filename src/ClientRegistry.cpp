#include "chat/ClientRegistry.hpp"

#include <algorithm>

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

std::size_t ClientRegistry::onlineCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return owners_.size();
}

std::vector<std::string> ClientRegistry::onlineUsers(std::size_t limit) const {
    std::vector<std::string> names;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        names.reserve(owners_.size());
        for (const auto& [name, owner] : owners_) names.push_back(name);
    }
    // Sort outside the lock: other loops shouldn't wait for our sorting.
    std::sort(names.begin(), names.end());
    if (names.size() > limit) names.resize(limit);
    return names;
}
