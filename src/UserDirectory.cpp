#include "chat/UserDirectory.hpp"
#include <fstream>
#include "chat/util.hpp"

bool UserDirectory::load(const std::string &path) {
    std::ifstream file(path);
    if (!file.is_open()) return false;

    std::string line;
    while (std::getline(file, line)) {
        line = util::stripLineEnding(line);
        std::size_t colon = line.find(':');
        if (colon == std::string::npos || colon == 0) continue;  // skip malformed lines
        users_[line.substr(0, colon)] = line.substr(colon + 1);
    }
    return true;
}

bool UserDirectory::verify(const std::string &username,
                           const std::string &password) const {
    auto it = users_.find(username);
    return it != users_.end() && it->second == password;
}