// ---------------------------------------------------------------------------
// ClientRegistry: who is online. The lock only guards the map. Network sends
// happen after the lock is released, so one slow client cannot stall everyone.
// ---------------------------------------------------------------------------

#pragma once
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "ClientSession.hpp"

class ClientRegistry {
    public:
        // Atomic "check not taken + insert", so two simultaneous logins with the
        // same username cannot both succeed.
        bool add(const std::string& username, SessionPtr session);

        void remove(const std::string& username);

        SessionPtr find(const std::string& username) const;

        void broadcast(const std::string& message, const std::string& exceptUser) const;

    private:
        std::vector<SessionPtr> snapshotExcept(const std::string& exceptUser) const;

        mutable std::mutex mutex_;
        std::unordered_map<std::string, SessionPtr> sessions_;
};
