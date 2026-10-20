#pragma once

#include "backup/store.h"

namespace backup {

class S3Store : public Store {
public:
    explicit S3Store(const Json& config);
    ~S3Store() override;
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
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace backup
