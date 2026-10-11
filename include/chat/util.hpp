#pragma once


#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <iostream>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace util {

    // Splits on a single delimiter character, skipping empty tokens.
    inline std::vector<std::string> split(const std::string& s, char delim) {
        std::vector<std::string> parts;
        std::string current;
        for (char c : s) {
            if (c == delim) {
                if (!current.empty()) parts.push_back(current);
                current.clear();
            } else {
                current += c;
            }
        }
        if (!current.empty()) parts.push_back(current);
        return parts;
    }
    
    // Joins parts[from..] with single spaces (no trailing space).
    inline std::string join(const std::vector<std::string>& parts, std::size_t from) {
        std::string out;
        for (std::size_t i = from; i < parts.size(); ++i) {
            if (i > from) out += ' ';
            out += parts[i];
        }
        return out;
    }
    
    inline std::string stripLineEnding(std::string s) {
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        return s;
    }
    
    inline std::optional<int> parsePort(const std::string& s) {
        if (s.empty() || s.size() > 5) return std::nullopt;
        if (!std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); }))
            return std::nullopt;
        int port = std::stoi(s);
        if (port < 1 || port > 65535) return std::nullopt;
        return port;
    }
    
    inline void log(const std::string& line) {
        static std::mutex m;
        std::lock_guard<std::mutex> lock(m);
        std::cout << line << std::endl;
    }
    
}  // namespace util
    