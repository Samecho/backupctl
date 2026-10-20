#pragma once

#include <exception>
#include <stdexcept>
#include <string>
#include <utility>

namespace backup {

class Error : public std::runtime_error {
public:
    Error(std::string category, std::string message)
        : std::runtime_error(std::move(message)), category_(std::move(category)) {}

    const std::string& category() const { return category_; }

private:
    std::string category_;
};

Error describe_error(std::exception_ptr failure);

inline void require(bool condition, const std::string& category, const std::string& message) {
    if (!condition) {
        throw Error(category, message);
    }
}

} // namespace backup
