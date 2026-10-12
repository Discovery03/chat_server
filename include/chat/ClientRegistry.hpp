// ---------------------------------------------------------------------------
// ClientRegistry: who is online, and WHICH EVENT LOOP owns them.
// Shared by all loops, so every method takes the lock.
//
// It deliberately stores the owning loop, not the session: other threads must
// not touch a session they don't own, so they only need to know where to
// send a message (see MessageRouter / EventLoop::post).
// ---------------------------------------------------------------------------

#pragma once
#include <mutex>
#include <string>
#include <unordered_map>

class EventLoop;

class ClientRegistry {
    public:
        // Atomic "check not taken + insert", so two simultaneous logins with the
        // same username (even on different loops) cannot both succeed.
        bool add(const std::string& username, EventLoop* owner);

        void remove(const std::string& username);

        // The loop that owns `username`, or nullptr if they are not online.
        EventLoop* find(const std::string& username) const;

    private:
        mutable std::mutex mutex_;
        std::unordered_map<std::string, EventLoop*> owners_;
};
