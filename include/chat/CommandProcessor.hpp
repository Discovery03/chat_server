#pragma once
#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdio>
#include <optional>
#include <string>
#include <unordered_map>
#include "ClientRegistry.hpp"
#include "GroupRegistry.hpp"
#include "util.hpp"
#include <vector>

// ---------------------------------------------------------------------------
// CommandProcessor: turns "/command args..." into a handler call.
// A table maps each command to its handler and error text. Adding a command
// is one table row plus one method.
// ---------------------------------------------------------------------------
class CommandProcessor {
public:
  CommandProcessor(ClientRegistry &clients, GroupRegistry &groups)
      : clients_(clients), groups_(groups) {
    commands_ = {
        {"/msg",
         {&CommandProcessor::privateMessage,
          "Error: Check receiver name or message and try again"}},
        {"/broadcast",
         {&CommandProcessor::broadcast, "Error: Check message and try again"}},
        {"/create_group",
         {&CommandProcessor::createGroup,
          "Error: Check if group already exists and try again"}},
        {"/join_group",
         {&CommandProcessor::joinGroup,
          "Error: Check if group exists (and you are not already in it) and "
          "try again"}},
        {"/leave_group",
         {&CommandProcessor::leaveGroup,
          "Error: Check if group exists and you are a member, and try again"}},
        {"/group_msg",
         {&CommandProcessor::groupMessage,
          "Error: Check group name, membership or message and try again"}},
    };
  }

  // Returns false when the client asked to disconnect.
  bool process(ClientSession &session, const std::string &line) {
    const Args args = util::split(line, ' ');
    if (args.empty())
      return true;
    if (args[0] == "/exit")
      return false;

    auto it = commands_.find(args[0]);
    if (it == commands_.end()) {
      session.send("Error: Invalid: Check and type the valid command");
      return true;
    }

    const Command &command = it->second;
    if (!(this->*command.handler)(session, args))
      session.send(command.errorMessage);
    return true;
  }

private:
  using Args = std::vector<std::string>;
  using Handler = bool (CommandProcessor::*)(ClientSession &, const Args &);
  struct Command {
    Handler handler;
    const char *errorMessage;
  };

  // Handlers return true on success, false to have the error text sent back.

  // /msg <user> <text...>
  bool privateMessage(ClientSession &sender, const Args &args) {
    if (args.size() < 3)
      return false;
    SessionPtr target = clients_.find(args[1]);
    if (!target || target.get() == &sender)
      return false;
    target->send("[" + sender.username() + "]: " + util::join(args, 2));
    return true;
  }

  // /broadcast <text...>
  bool broadcast(ClientSession &sender, const Args &args) {
    if (args.size() < 2)
      return false;
    clients_.broadcast("[Broadcast from " + sender.username() +
                           "]: " + util::join(args, 1),
                       sender.username());
    return true;
  }

  // /create_group <name>
  bool createGroup(ClientSession &sender, const Args &args) {
    if (args.size() != 2 || !groups_.create(args[1], sender.username()))
      return false;
    sender.send("Group " + args[1] + " Created.");
    return true;
  }

  // /join_group <name>
  bool joinGroup(ClientSession &sender, const Args &args) {
    if (args.size() != 2 || !groups_.join(args[1], sender.username()))
      return false;
    sender.send("You joined the group " + args[1] + ".");
    return true;
  }

  // /leave_group <name>
  bool leaveGroup(ClientSession &sender, const Args &args) {
    if (args.size() != 2 || !groups_.leave(args[1], sender.username()))
      return false;
    sender.send("You left the group " + args[1] + ".");
    return true;
  }

  // /group_msg <name> <text...>
  bool groupMessage(ClientSession &sender, const Args &args) {
    if (args.size() < 3)
      return false;
    auto members = groups_.membersIfMember(args[1], sender.username());
    if (!members)
      return false;

    const std::string message =
        "[Group " + args[1] + "]: " + util::join(args, 2);
    for (const std::string &name : *members) {
      if (name == sender.username())
        continue;
      if (SessionPtr target = clients_.find(name))
        target->send(message);
    }
    return true;
  }

  ClientRegistry &clients_;
  GroupRegistry &groups_;
  std::unordered_map<std::string, Command> commands_;
};
