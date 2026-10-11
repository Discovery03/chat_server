// ---------------------------------------------------------------------------
// ClientRegistry: who is online. The lock only guards the map. Network sends
// happen after the lock is released, so one slow client cannot stall everyone.
// ---------------------------------------------------------------------------


#pragma once
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include "util.hpp"
#include "ClientSession.hpp"

using SessionPtr = std::shared_ptr<ClientSession>;

class ClientRegistry {
    public:
        // Atomic "check not taken + insert", so two simultaneous logins with the
        // same username cannot both succeed.
        bool add(const std::string& username, SessionPtr session) {
            std::lock_guard<std::mutex> lock(mutex_);
            return sessions_.emplace(username, std::move(session)).second;
        }
    
        void remove(const std::string& username) {
            std::lock_guard<std::mutex> lock(mutex_);
            sessions_.erase(username);
        }
    
        SessionPtr find(const std::string& username) const {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = sessions_.find(username);
            return it == sessions_.end() ? nullptr : it->second;
        }
    
        void broadcast(const std::string& message, const std::string& exceptUser) const {
            for (const SessionPtr& session : snapshotExcept(exceptUser)) session->send(message);
        }
    
    private:
        std::vector<SessionPtr> snapshotExcept(const std::string& exceptUser) const {
            std::lock_guard<std::mutex> lock(mutex_);
            std::vector<SessionPtr> out;
            out.reserve(sessions_.size());
            for (const auto& [name, session] : sessions_) {
                if (name != exceptUser) out.push_back(session);
            }
            return out;
        }
    
        mutable std::mutex mutex_;
        std::unordered_map<std::string, SessionPtr> sessions_;

};
    