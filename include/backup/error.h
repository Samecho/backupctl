#pragma once

#include <stdexcept>
#include <string>

namespace backup {

class Error : public std::runtime_error {
public:
    Error(std::string category, std::string message)
        : std::runtime_error(std::move(message)), category_(std::move(category)) {}

    const std::string& category() const { return category_; }

private:
    std::string category_;
};

inline void require(bool condition, const std::string& category, const std::string& message) {
    if (!condition) {
        throw Error(category, message);
    }
}

} // namespace backup
