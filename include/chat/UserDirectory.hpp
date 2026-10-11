#pragma once
#include <string>
#include <unordered_map>

class UserDirectory {
    public:
      bool load(const std::string &path);

      bool verify(const std::string &username,
                  const std::string &password) const;
                  
    
    private:
        std::unordered_map<std::string, std::string> users_;
};
    