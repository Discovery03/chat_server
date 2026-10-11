// ---------------------------------------------------------------------------
// GroupRegistry: group name -> member usernames.
// Members are stored by username, not by fd. Usernames stay meaningful if
// sessions are later spread across processes.
// ---------------------------------------------------------------------------

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
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>


class GroupRegistry {
    public:
        bool create(const std::string& group, const std::string& creator) {
            std::lock_guard<std::mutex> lock(mutex_);
            return groups_.emplace(group, std::set<std::string>{creator}).second;
        }
    
        bool join(const std::string& group, const std::string& user) {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = groups_.find(group);
            if (it == groups_.end()) return false;
            return it->second.insert(user).second;  // false if already a member
        }
    
        bool leave(const std::string& group, const std::string& user) {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = groups_.find(group);
            if (it == groups_.end()) return false;
            return it->second.erase(user) > 0;
        }
    
        void removeUser(const std::string& user) {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& [name, members] : groups_) members.erase(user);
        }
    
        // Members of `group`, but only if `requester` belongs to it.
        std::optional<std::vector<std::string>> membersIfMember(const std::string& group,
                                                                const std::string& requester) const {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = groups_.find(group);
            if (it == groups_.end() || it->second.count(requester) == 0) return std::nullopt;
            return std::vector<std::string>(it->second.begin(), it->second.end());
        }
    
    private:
        mutable std::mutex mutex_;
        std::unordered_map<std::string, std::set<std::string>> groups_;
    };
    