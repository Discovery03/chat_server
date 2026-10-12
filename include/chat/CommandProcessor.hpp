#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include "ClientRegistry.hpp"
#include "ClientSession.hpp"
#include "GroupRegistry.hpp"
#include "MessageRouter.hpp"

// ---------------------------------------------------------------------------
// CommandProcessor: turns "/command args..." into a handler call.
// A table maps each command to its handler and error text. Adding a command
// is one table row plus one method.
//
// It only ever touches the SENDER's session directly. Messages for other users
// go through the MessageRouter, because their sessions may live on other threads.
// ---------------------------------------------------------------------------
class CommandProcessor {
public:
  CommandProcessor(MessageRouter &router, GroupRegistry &groups,
                   const ClientRegistry &clients);

  // Returns false when the client asked to disconnect.
  bool process(ClientSession &session, const std::string &line);

private:
  using Args = std::vector<std::string>;
  using Handler = bool (CommandProcessor::*)(ClientSession &, const Args &);
  struct Command {
    Handler handler;
    const char *errorMessage;
  };

  // Handlers return true on success, false to have the error text sent back.

  // /msg <user> <text...>
  bool privateMessage(ClientSession &sender, const Args &args);

  // /broadcast <text...>
  bool broadcast(ClientSession &sender, const Args &args);

  // /create_group <name>
  bool createGroup(ClientSession &sender, const Args &args);

  // /join_group <name>
  bool joinGroup(ClientSession &sender, const Args &args);

  // /leave_group <name>
  bool leaveGroup(ClientSession &sender, const Args &args);

  // /group_msg <name> <text...>
  bool groupMessage(ClientSession &sender, const Args &args);

  // /online — who is online (first kOnlineListLimit names, sorted)
  bool online(ClientSession &sender, const Args &args);

  MessageRouter &router_;
  GroupRegistry &groups_;
  const ClientRegistry &clients_;  // read-only here: presence queries for /online
  std::unordered_map<std::string, Command> commands_;
};
