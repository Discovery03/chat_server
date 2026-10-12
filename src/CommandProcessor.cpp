#include "chat/CommandProcessor.hpp"

#include "chat/util.hpp"

namespace {
// /online lists at most this many names, so the reply stays small even with
// thousands of users online.
constexpr std::size_t kOnlineListLimit = 100;
}

CommandProcessor::CommandProcessor(MessageRouter &router, GroupRegistry &groups,
                                   const ClientRegistry &clients)
    : router_(router), groups_(groups), clients_(clients) {
  commands_ = {
      {"/msg",
       {&CommandProcessor::privateMessage,
        "Error: Check receiver name or message and try again\n"}},
      {"/broadcast",
       {&CommandProcessor::broadcast, "Error: Check message and try again\n"}},
      {"/create_group",
       {&CommandProcessor::createGroup,
        "Error: Check if group already exists and try again\n"}},
      {"/join_group",
       {&CommandProcessor::joinGroup,
        "Error: Check if group exists (and you are not already in it) and "
        "try again\n"}},
      {"/leave_group",
       {&CommandProcessor::leaveGroup,
        "Error: Check if group exists and you are a member, and try again\n"}},
      {"/group_msg",
       {&CommandProcessor::groupMessage,
        "Error: Check group name, membership or message and try again\n"}},
      {"/online",
       {&CommandProcessor::online, "Error: Usage: /online\n"}},
  };
}

bool CommandProcessor::process(ClientSession &session, const std::string &line) {
  const Args args = util::split(line, ' ');
  if (args.empty())
    return true;
  if (args[0] == "/exit")
    return false;

  auto it = commands_.find(args[0]);
  if (it == commands_.end()) {
    session.send("Error: Invalid: Check and type the valid command\n");
    return true;
  }

  const Command &command = it->second;
  if (!(this->*command.handler)(session, args))
    session.send(command.errorMessage);
  return true;
}

bool CommandProcessor::privateMessage(ClientSession &sender, const Args &args) {
  if (args.size() < 3)
    return false;
  if (args[1] == sender.username())
    return false;
  // false if the receiver is not online
  return router_.deliver(args[1],
                         "[" + sender.username() + "]: " + util::join(args, 2) + "\n");
}

bool CommandProcessor::broadcast(ClientSession &sender, const Args &args) {
  if (args.size() < 2)
    return false;
  router_.broadcast("[Broadcast from " + sender.username() +
                         "]: " + util::join(args, 1) + "\n",
                     sender.username());
  return true;
}

bool CommandProcessor::createGroup(ClientSession &sender, const Args &args) {
  if (args.size() != 2 || !groups_.create(args[1], sender.username()))
    return false;
  sender.send("Group " + args[1] + " Created.\n");
  return true;
}

bool CommandProcessor::joinGroup(ClientSession &sender, const Args &args) {
  if (args.size() != 2 || !groups_.join(args[1], sender.username()))
    return false;
  sender.send("You joined the group " + args[1] + ".\n");
  return true;
}

bool CommandProcessor::leaveGroup(ClientSession &sender, const Args &args) {
  if (args.size() != 2 || !groups_.leave(args[1], sender.username()))
    return false;
  sender.send("You left the group " + args[1] + ".\n");
  return true;
}

bool CommandProcessor::groupMessage(ClientSession &sender, const Args &args) {
  if (args.size() < 3)
    return false;
  auto members = groups_.membersIfMember(args[1], sender.username());
  if (!members)
    return false;

  const std::string message =
      "[Group " + args[1] + "]: " + util::join(args, 2) + "\n";
  for (const std::string &name : *members) {
    if (name == sender.username())
      continue;
    router_.deliver(name, message);  // offline members are simply skipped
  }
  return true;
}

// Presence is PULLED with /online instead of PUSHED on every login. Pushing
// "<name> has joined" to every user made each login cost O(users online), so
// N logins cost O(N^2) messages (~50 million for 10k users).
bool CommandProcessor::online(ClientSession &sender, const Args &args) {
  if (args.size() != 1)
    return false;
  const std::size_t total = clients_.onlineCount();
  const std::vector<std::string> names = clients_.onlineUsers(kOnlineListLimit);

  std::string reply = "Online (" + std::to_string(total) + "): " + util::join(names, 0);
  if (total > names.size())
    reply += " ... and " + std::to_string(total - names.size()) + " more";
  sender.send(reply + "\n");
  return true;
}
