#pragma once

#include <openssl/evp.h>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace backup {

using Bytes = std::vector<unsigned char>;
inline constexpr size_t chunk_size = 4 * 1024 * 1024;
inline constexpr size_t max_record_size = 64 * 1024;
inline constexpr std::string_view codec_name = "zstd-v1-level3";

class Sha256 {
public:
    Sha256();
    void update(std::span<const unsigned char> data);
    std::string finish();

private:
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context_;
    bool finished_ = false;
};

std::string sha256(std::span<const unsigned char> data);
std::string sha256(std::string_view data);
std::string hash_file(const std::filesystem::path& path);
std::string checksum_base64(std::string_view hex);
bool valid_hash(std::string_view value);
Bytes compress_chunk(std::span<const unsigned char> raw);
Bytes decompress_chunk(std::span<const unsigned char> encoded, size_t raw_size);
std::string chunk_key(std::string_view hash);

} // namespace backup
