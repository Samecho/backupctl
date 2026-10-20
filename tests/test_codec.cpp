#include "backup/codec.h"
#include "backup/error.h"

#include <gtest/gtest.h>
#include <random>

using namespace backup;

TEST(Codec, KnownSha256) {
    EXPECT_EQ(sha256("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(sha256(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(checksum_base64(sha256("abc")), "ungWv48Bz+pBQUDeXa4iI7ADYaOWF3qctBD/YfIAFa0=");
}

TEST(Codec, CompressibleAndRandomRoundTrip) {
    Bytes raw(chunk_size, 'x');
    auto encoded = compress_chunk(raw);
    EXPECT_LT(encoded.size(), raw.size());
    EXPECT_EQ(decompress_chunk(encoded, raw.size()), raw);
    std::mt19937 generator(42);
    for (auto& value : raw) {
        value = static_cast<unsigned char>(generator());
    }
    EXPECT_EQ(decompress_chunk(compress_chunk(raw), raw.size()), raw);
}

TEST(Codec, RejectsExpansionCorruptionAndTrailingFrames) {
    Bytes raw(4096, 'x');
    auto encoded = compress_chunk(raw);
    EXPECT_THROW(decompress_chunk(encoded, chunk_size + 1), Error);
    EXPECT_THROW(decompress_chunk(encoded, 10), Error);
    auto trailing = encoded;
    trailing.insert(trailing.end(), encoded.begin(), encoded.end());
    EXPECT_THROW(decompress_chunk(trailing, raw.size()), Error);
    encoded[0] ^= 0xff;
    EXPECT_THROW(decompress_chunk(encoded, raw.size()), Error);
    EXPECT_THROW(compress_chunk({}), Error);
}
