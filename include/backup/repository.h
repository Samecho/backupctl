#pragma once

#include "backup/database.h"
#include "backup/store.h"

namespace backup {

inline constexpr uint64_t max_manifest_size = 1024ULL * 1024 * 1024;

bool valid_id(const std::string& value);
Json parse_json(const std::string& value);
int64_t integer(const Json& value, const std::string& field, int64_t minimum = 0,
                int64_t maximum = INT64_MAX);
std::string string_field(const Json& value, const std::string& field);
Json initialize(const std::filesystem::path& state, Json config);

class Repository {
public:
    explicit Repository(std::filesystem::path state);
    Store& store() { return *store_; }
    const std::filesystem::path& state() const { return state_; }
    const Json& config() const { return config_; }
    const std::string& id() const { return id_; }
    Json commit(const std::string& snapshot);
    void validate_commit(const Json& value, const std::string& snapshot) const;

private:
    std::filesystem::path state_;
    Json config_;
    std::string id_;
    std::unique_ptr<Store> store_;
};

} // namespace backup
