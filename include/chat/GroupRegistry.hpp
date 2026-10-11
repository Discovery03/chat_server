// ---------------------------------------------------------------------------
// GroupRegistry: group name -> member usernames.
// Members are stored by username, not by fd. Usernames stay meaningful if
// sessions are later spread across processes.
// ---------------------------------------------------------------------------

#pragma once
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

class GroupRegistry {
    public:
        bool create(const std::string& group, const std::string& creator);

        // False if the group doesn't exist or the user is already a member.
        bool join(const std::string& group, const std::string& user);

        bool leave(const std::string& group, const std::string& user);

        void removeUser(const std::string& user);

        // Members of `group`, but only if `requester` belongs to it.
        std::optional<std::vector<std::string>> membersIfMember(const std::string& group,
                                                                const std::string& requester) const;

    private:
        mutable std::mutex mutex_;
        std::unordered_map<std::string, std::set<std::string>> groups_;
};
