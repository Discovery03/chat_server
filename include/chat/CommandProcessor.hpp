#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include "ClientRegistry.hpp"
#include "ClientSession.hpp"
#include "GroupRegistry.hpp"

// ---------------------------------------------------------------------------
// CommandProcessor: turns "/command args..." into a handler call.
// A table maps each command to its handler and error text. Adding a command
// is one table row plus one method.
// ---------------------------------------------------------------------------
class CommandProcessor {
public:
  CommandProcessor(ClientRegistry &clients, GroupRegistry &groups);

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

  ClientRegistry &clients_;
  GroupRegistry &groups_;
  std::unordered_map<std::string, Command> commands_;
};
