#pragma once
#include <memory>
#include <mutex>
#include <optional>
#include <string>

class ClientSession {
    public:
        explicit ClientSession(int fd);
        ~ClientSession();
        ClientSession(const ClientSession&) = delete;
        ClientSession& operator=(const ClientSession&) = delete;

        // Sends the whole message. Several threads may send to the same client
        // (e.g. two people messaging the same user), so writes are serialised.
        bool send(const std::string& message);

        // Blocks for the next chunk from the client. nullopt = disconnected/error.
        std::optional<std::string> receive();

        // Set once, by the owning handler thread, before the session is registered.
        void setUsername(std::string name);
        const std::string& username() const;

    private:
        int fd_;
        std::string username_;
        std::mutex sendMutex_;
};

using SessionPtr = std::shared_ptr<ClientSession>;
