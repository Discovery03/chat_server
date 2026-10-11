#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace util {

    // Splits on a single delimiter character, skipping empty tokens.
    std::vector<std::string> split(const std::string& s, char delim);

    // Joins parts[from..] with single spaces (no trailing space).
    std::string join(const std::vector<std::string>& parts, std::size_t from);

    std::string stripLineEnding(std::string s);

    std::optional<int> parsePort(const std::string& s);

    // Thread-safe line output to stdout.
    void log(const std::string& line);

}  // namespace util
