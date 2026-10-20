#include "backup/database.h"
#include "backup/error.h"
#include "backup/fs.h"
#include "backup/store.h"

#include <gtest/gtest.h>
#include <unistd.h>

using namespace backup;
namespace fs = std::filesystem;

class StorageTest : public testing::Test {
protected:
    fs::path directory = fs::temp_directory_path() / ("backupctl-test-" + random_id());
    void SetUp() override { fs::create_directories(directory); }
    void TearDown() override { fs::remove_all(directory); }
};

TEST_F(StorageTest, ConditionalObjectsAndChecksum) {
    LocalStore store(directory / "remote");
    Bytes bytes{'a', 'b', 'c'};
    EXPECT_TRUE(store.put("chunks/object", bytes, {{"codec", "test"}}, sha256(bytes)));
    Bytes changed{'x'};
    EXPECT_FALSE(store.put("chunks/object", changed, {}, sha256(changed)));
    EXPECT_EQ(store.get("chunks/object", 10), bytes);
    EXPECT_THROW(store.get("chunks/object", 2), Error);
    atomic_write(directory / "remote/objects/chunks/object/data", "bad");
    EXPECT_THROW(store.get("chunks/object", 10), Error);
}

TEST_F(StorageTest, TransactionsRollbackAndLockExclusion) {
    Database database(directory / "state.db");
    create_schema(database);
    {
        Transaction transaction(database);
        database.exec("INSERT INTO chunks VALUES('a', 1, 1, 'b')");
    }
    auto count = database.prepare("SELECT count(*) FROM chunks");
    ASSERT_TRUE(count.row());
    EXPECT_EQ(count.integer(0), 0);
    StateLock first(directory);
    EXPECT_THROW(StateLock second(directory), Error);
}

TEST_F(StorageTest, RejectsUnsafePathsAndSourceSymlinks) {
    for (const auto& path : {"", "/tmp/x", "../x", "a/../x", "a//b", "a/./b", "a/"}) {
        EXPECT_THROW(validate_path(path), Error);
    }
    fs::create_directory(directory / "source");
    fs::create_symlink("/etc/passwd", directory / "source/link");
    EXPECT_THROW(scan_tree(directory / "source", [](const FileInfo&) {}), Error);
    EXPECT_THROW(open_source(directory / "source", "link"), Error);
}

TEST_F(StorageTest, PublishNeverReplacesExistingDirectory) {
    fs::create_directory(directory / "temporary");
    fs::create_directory(directory / "target");
    EXPECT_THROW(publish_directory(directory / "temporary", directory / "target"), Error);
    fs::remove(directory / "target");
    publish_directory(directory / "temporary", directory / "target");
    EXPECT_TRUE(fs::is_directory(directory / "target"));
}
