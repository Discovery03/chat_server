#include "chat/ClientRegistry.hpp"

#include <utility>

bool ClientRegistry::add(const std::string& username, SessionPtr session) {
    std::lock_guard<std::mutex> lock(mutex_);
    return sessions_.emplace(username, std::move(session)).second;
}

void ClientRegistry::remove(const std::string& username) {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.erase(username);
}

SessionPtr ClientRegistry::find(const std::string& username) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(username);
    return it == sessions_.end() ? nullptr : it->second;
}

void ClientRegistry::broadcast(const std::string& message, const std::string& exceptUser) const {
    for (const SessionPtr& session : snapshotExcept(exceptUser)) session->send(message);
}

std::vector<SessionPtr> ClientRegistry::snapshotExcept(const std::string& exceptUser) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SessionPtr> out;
    out.reserve(sessions_.size());
    for (const auto& [name, session] : sessions_) {
        if (name != exceptUser) out.push_back(session);
    }
    return out;
}
