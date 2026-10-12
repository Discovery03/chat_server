// ---------------------------------------------------------------------------
// MessageRouter: "get this text to user X" / "get this text to everyone".
//
// Code that handles one client (CommandProcessor, the login logic) never
// touches ANOTHER client's ClientSession directly: that session may be owned
// by a different thread. It asks the router instead, and the router gets the
// text to whichever thread owns the recipient.
//
// Today ChatServer implements this with per-loop mailboxes (one process).
// In Step 2 another implementation can route through Redis (many servers),
// without CommandProcessor changing at all.
// ---------------------------------------------------------------------------

#pragma once
#include <string>

class MessageRouter {
    public:
        virtual ~MessageRouter() = default;

        // Queues `text` for `username`. Returns false if that user is not online.
        // Delivery is asynchronous: true means "on its way", not "received".
        virtual bool deliver(const std::string& username, std::string text) = 0;

        // Queues `text` for every online user except `exceptUser`.
        virtual void broadcast(std::string text, const std::string& exceptUser) = 0;
};
