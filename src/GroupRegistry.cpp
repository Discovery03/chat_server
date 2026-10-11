#include "chat/GroupRegistry.hpp"

bool GroupRegistry::create(const std::string& group, const std::string& creator) {
    std::lock_guard<std::mutex> lock(mutex_);
    return groups_.emplace(group, std::set<std::string>{creator}).second;
}

bool GroupRegistry::join(const std::string& group, const std::string& user) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = groups_.find(group);
    if (it == groups_.end()) return false;
    return it->second.insert(user).second;  // false if already a member
}

bool GroupRegistry::leave(const std::string& group, const std::string& user) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = groups_.find(group);
    if (it == groups_.end()) return false;
    return it->second.erase(user) > 0;
}

void GroupRegistry::removeUser(const std::string& user) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [name, members] : groups_) members.erase(user);
}

std::optional<std::vector<std::string>> GroupRegistry::membersIfMember(
        const std::string& group, const std::string& requester) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = groups_.find(group);
    if (it == groups_.end() || it->second.count(requester) == 0) return std::nullopt;
    return std::vector<std::string>(it->second.begin(), it->second.end());
}
