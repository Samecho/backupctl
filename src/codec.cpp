#include "backup/codec.h"
#include "backup/error.h"

#include <zstd.h>

#include <algorithm>
#include <array>
#include <fstream>

namespace backup {

Sha256::Sha256() : context_(EVP_MD_CTX_new(), EVP_MD_CTX_free) {
    require(context_ && EVP_DigestInit_ex(context_.get(), EVP_sha256(), nullptr) == 1,
            "io", "cannot initialize SHA-256");
}

void Sha256::update(std::span<const unsigned char> data) {
    require(!finished_ && EVP_DigestUpdate(context_.get(), data.data(), data.size()) == 1,
            "io", "cannot update SHA-256");
}

std::string Sha256::finish() {
    std::array<unsigned char, 32> digest{};
    unsigned int size = 0;
    require(!finished_ && EVP_DigestFinal_ex(context_.get(), digest.data(), &size) == 1 &&
                size == digest.size(), "io", "cannot finish SHA-256");
    finished_ = true;
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (unsigned char value : digest) {
        result += digits[value >> 4];
        result += digits[value & 15];
    }
    return result;
}

std::string sha256(std::span<const unsigned char> data) {
    Sha256 hash;
    hash.update(data);
    return hash.finish();
}

std::string sha256(std::string_view data) {
    return sha256(std::span(reinterpret_cast<const unsigned char*>(data.data()), data.size()));
}

std::string hash_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(input.is_open(), "io", "cannot open file for hashing");
    std::array<unsigned char, 64 * 1024> buffer{};
    Sha256 hash;
    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
        hash.update(std::span(buffer.data(), static_cast<size_t>(input.gcount())));
    }
    require(input.eof() && !input.bad(), "io", "cannot read file for hashing");
    return hash.finish();
}

bool valid_hash(std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

std::string checksum_base64(std::string_view hex) {
    require(valid_hash(hex), "integrity", "invalid SHA-256");
    std::array<unsigned char, 32> raw{};
    auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    for (size_t i = 0; i < raw.size(); ++i) {
        raw[i] = static_cast<unsigned char>(digit(hex[2 * i]) * 16 + digit(hex[2 * i + 1]));
    }
    std::array<unsigned char, 45> result{};
    int size = EVP_EncodeBlock(result.data(), raw.data(), static_cast<int>(raw.size()));
    require(size == 44, "io", "cannot encode checksum");
    return std::string(reinterpret_cast<char*>(result.data()), static_cast<size_t>(size));
}

Bytes compress_chunk(std::span<const unsigned char> raw) {
    require(!raw.empty() && raw.size() <= chunk_size, "integrity", "invalid raw chunk size");
    Bytes encoded(ZSTD_compressBound(raw.size()));
    size_t size = ZSTD_compress(encoded.data(), encoded.size(), raw.data(), raw.size(), 3);
    require(!ZSTD_isError(size), "io", "zstd compression failed");
    encoded.resize(size);
    return encoded;
}

Bytes decompress_chunk(std::span<const unsigned char> encoded, size_t raw_size) {
    require(raw_size > 0 && raw_size <= chunk_size &&
                encoded.size() <= ZSTD_compressBound(chunk_size),
            "integrity", "chunk exceeds decode limits");
    auto size = ZSTD_getFrameContentSize(encoded.data(), encoded.size());
    require(size == raw_size && ZSTD_findFrameCompressedSize(encoded.data(), encoded.size()) ==
                encoded.size(), "integrity", "invalid zstd frame");
    Bytes raw(raw_size);
    size_t decoded = ZSTD_decompress(raw.data(), raw.size(), encoded.data(), encoded.size());
    require(!ZSTD_isError(decoded) && decoded == raw_size,
            "integrity", "zstd decompression failed");
    return raw;
}

std::string chunk_key(std::string_view hash) {
    require(valid_hash(hash), "integrity", "invalid chunk hash");
    return "chunks/v1/zstd/" + std::string(hash.substr(0, 2)) + "/" + std::string(hash);
}

} // namespace backup
