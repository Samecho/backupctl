#include "backup/error.h"

#include <nlohmann/json.hpp>

#include <filesystem>

namespace backup {

Error describe_error(std::exception_ptr failure) {
    try {
        std::rethrow_exception(failure);
    } catch (const Error& error) {
        return error;
    } catch (const std::filesystem::filesystem_error& error) {
        bool denied = error.code() == std::errc::permission_denied ||
                      error.code() == std::errc::operation_not_permitted;
        return Error(denied ? "permission" : "io", error.what());
    } catch (const nlohmann::json::exception& error) {
        return Error("integrity", error.what());
    } catch (const std::exception& error) {
        return Error("io", error.what());
    } catch (...) {
        return Error("io", "unknown exception");
    }
}

} // namespace backup
