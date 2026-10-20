#pragma once

#include "backup/codec.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace backup {

using Json = nlohmann::json;
using Metadata = std::map<std::string, std::string>;

struct ObjectInfo {
    uint64_t size = 0;
    Metadata metadata;
};

struct StoreMetrics {
    std::atomic<uint64_t> head{0}, get{0}, put{0}, list{0};
    std::atomic<uint64_t> uploaded{0}, downloaded{0}, put_created{0};
    Json report() const;
};

class Store {
public:
    virtual ~Store() = default;
    virtual std::optional<ObjectInfo> head(const std::string& key) = 0;
    virtual bool put(const std::string& key, std::span<const unsigned char> data,
                     const Metadata& metadata, const std::string& encoded_hash) = 0;
    virtual bool put_file(const std::string& key, const std::filesystem::path& source,
                          const Metadata& metadata, const std::string& encoded_hash) = 0;
    virtual Bytes get(const std::string& key, size_t limit) = 0;
    virtual void get_file(const std::string& key, const std::filesystem::path& destination,
                          uint64_t limit) = 0;
    virtual void list(const std::string& prefix,
                       const std::function<void(const std::string&)>& visit) = 0;
    StoreMetrics metrics;
};

class LocalStore : public Store {
public:
    explicit LocalStore(std::filesystem::path root);
    std::optional<ObjectInfo> head(const std::string& key) override;
    bool put(const std::string& key, std::span<const unsigned char> data,
             const Metadata& metadata, const std::string& encoded_hash) override;
    bool put_file(const std::string& key, const std::filesystem::path& source,
                  const Metadata& metadata, const std::string& encoded_hash) override;
    Bytes get(const std::string& key, size_t limit) override;
    void get_file(const std::string& key, const std::filesystem::path& destination,
                  uint64_t limit) override;
    void list(const std::string& prefix,
               const std::function<void(const std::string&)>& visit) override;

private:
    std::filesystem::path object_path(const std::string& key) const;
    bool write_object(const std::string& key, const std::function<void(int)>& write,
                      const ObjectInfo& info);
    std::filesystem::path root_;
};

std::unique_ptr<Store> make_store(const Json& config);
void checkpoint(const std::string& name);
void test_store_delay();

} // namespace backup
