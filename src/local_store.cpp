#include "backup/store.h"
#include "backup/error.h"
#include "backup/fs.h"

#include <array>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>

namespace backup {
namespace fs = std::filesystem;

Json StoreMetrics::report() const {
    return {{"head_requests", head.load()}, {"get_requests", get.load()},
            {"put_requests", put.load()}, {"list_requests", list.load()},
            {"successful_upload_bytes", uploaded.load()}, {"download_bytes", downloaded.load()},
            {"objects_created", put_created.load()},
            {"head_attempts", std::max(head.load(), head_attempts.load())},
            {"get_attempts", std::max(get.load(), get_attempts.load())},
            {"put_attempts", std::max(put.load(), put_attempts.load())},
            {"list_attempts", std::max(list.load(), list_attempts.load())}};
}

void checkpoint(const std::string& name) {
#ifdef BACKUPCTL_TESTING
    static std::atomic<uint64_t> occurrence{0};
    const char* point = std::getenv("BACKUPCTL_FAILPOINT");
    if (point && name == point) {
        const char* configured = std::getenv("BACKUPCTL_FAIL_AFTER");
        uint64_t after = configured ? std::stoull(configured) : 1;
        if (++occurrence == after) {
            _exit(86);
        }
    }
#else
    (void)name;
#endif
}

void test_store_delay() {
#ifdef BACKUPCTL_TESTING
    const char* value = std::getenv("BACKUPCTL_TEST_DELAY_MS");
    if (value) {
        std::this_thread::sleep_for(std::chrono::milliseconds(std::stoul(value)));
    }
#endif
}

LocalStore::LocalStore(fs::path root) : root_(std::move(root)) {
    require(!fs::is_symlink(fs::symlink_status(root_)), "configuration", "store root is a symlink");
    fs::create_directories(root_ / "objects");
    fs::create_directories(root_ / ".pending");
}

fs::path LocalStore::object_path(const std::string& key) const {
    validate_path(key);
    return root_ / "objects" / key;
}

std::optional<ObjectInfo> LocalStore::head(const std::string& key) {
    ++metrics.head;
    auto path = object_path(key);
    if (!fs::exists(path)) {
        return std::nullopt;
    }
#ifdef BACKUPCTL_TESTING
    if (std::getenv("BACKUPCTL_TEST_COMMIT_READ_FAILURE") && key.starts_with("snapshots/")) {
        throw Error("network_exhausted", "injected commit read timeout");
    }
#endif
    auto value = Json::parse(read_small(path / "meta.json", max_record_size));
    ObjectInfo info{value.at("size").get<uint64_t>(), value.at("metadata").get<Metadata>()};
    std::error_code error;
    auto size = fs::file_size(path / "data", error);
    require(!error && size == info.size, "integrity", "object is missing or has the wrong length");
    return info;
}

bool LocalStore::write_object(const std::string& key, const std::function<void(int)>& writer,
                              const ObjectInfo& info) {
    ++metrics.put;
    test_store_delay();
#ifdef BACKUPCTL_TESTING
    if (std::getenv("BACKUPCTL_TEST_PUT_PERMISSION")) {
        throw Error("permission", "injected write denial");
    }
#endif
    auto target = object_path(key);
    auto temporary = root_ / ".pending" / random_id();
    fs::create_directory(temporary);
    try {
        Fd output(open((temporary / "data").c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
        writer(output.get());
        sync_fd(output.get());
        require(fs::file_size(temporary / "data") == info.size,
                "io", "object size changed during upload");
        require(hash_file(temporary / "data") == info.metadata.at("encoded-sha256"),
                "integrity", "encoded upload checksum mismatch");
        atomic_write(temporary / "meta.json", Json{{"size", info.size},
                     {"metadata", info.metadata}}.dump());
        sync_directory(temporary);
        fs::create_directories(target.parent_path());
        // Directory rename makes payload and metadata visible together in the test store.
        if (syscall(SYS_renameat2, AT_FDCWD, temporary.c_str(), AT_FDCWD,
                    target.c_str(), RENAME_NOREPLACE) < 0) {
            if (errno == EEXIST || errno == ENOTEMPTY) {
                fs::remove_all(temporary);
                return false;
            }
            io_error("create immutable object");
        }
        sync_directory(target.parent_path());
        metrics.uploaded += info.size;
        ++metrics.put_created;
#ifdef BACKUPCTL_TESTING
        const char* lost = std::getenv("BACKUPCTL_TEST_LOST_RESPONSE");
        if (lost && key.starts_with(std::string(lost) + "/")) {
            throw Error("network_exhausted", "injected lost PUT response");
        }
#endif
        return true;
    } catch (...) {
        std::error_code ignored;
        fs::remove_all(temporary, ignored);
        throw;
    }
}

bool LocalStore::put(const std::string& key, std::span<const unsigned char> data,
                     const Metadata& metadata, const std::string& encoded_hash) {
    auto values = metadata;
    values["encoded-sha256"] = encoded_hash;
    return write_object(key, [&](int fd) { write_all(fd, data.data(), data.size()); },
                         {data.size(), values});
}

bool LocalStore::put_file(const std::string& key, const fs::path& source,
                          const Metadata& metadata, const std::string& encoded_hash) {
    auto values = metadata;
    values["encoded-sha256"] = encoded_hash;
    return write_object(key, [&](int fd) {
        Fd input(open(source.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
        std::array<char, 65536> buffer{};
        while (true) {
            ssize_t size = read(input.get(), buffer.data(), buffer.size());
            if (size < 0 && errno == EINTR) {
                continue;
            }
            if (size < 0) {
                io_error("read upload file");
            }
            if (!size) {
                break;
            }
            write_all(fd, buffer.data(), static_cast<size_t>(size));
        }
    }, {fs::file_size(source), values});
}

Bytes LocalStore::get(const std::string& key, size_t limit) {
    ++metrics.get;
    test_store_delay();
    auto info = head(key);
    require(info.has_value(), "integrity", "missing object: " + key);
    require(info->size <= limit, "integrity", "object exceeds size limit");
    auto value = read_small(object_path(key) / "data", limit);
    require(sha256(value) == info->metadata.at("encoded-sha256"),
            "integrity", "encoded download checksum mismatch");
    metrics.downloaded += value.size();
    return Bytes(value.begin(), value.end());
}

void LocalStore::get_file(const std::string& key, const fs::path& destination, uint64_t limit) {
    ++metrics.get;
    auto info = head(key);
    require(info.has_value() && info->size <= limit, "integrity", "missing or oversized object");
    Fd input(open((object_path(key) / "data").c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    Fd output(open(destination.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    std::array<unsigned char, 65536> buffer{};
    uint64_t total = 0;
    Sha256 hash;
    while (true) {
        ssize_t size = read(input.get(), buffer.data(), buffer.size());
        if (size < 0 && errno == EINTR) {
            continue;
        }
        if (size < 0) {
            io_error("read object");
        }
        if (!size) {
            break;
        }
        total += static_cast<uint64_t>(size);
        require(total <= limit, "integrity", "download exceeded length limit");
        hash.update(std::span(buffer.data(), static_cast<size_t>(size)));
        write_all(output.get(), buffer.data(), static_cast<size_t>(size));
    }
    require(total == info->size && hash.finish() == info->metadata.at("encoded-sha256"),
            "integrity", "download checksum mismatch");
    sync_fd(output.get());
    metrics.downloaded += total;
}

void LocalStore::list(const std::string& prefix,
                       const std::function<void(const std::string&)>& visit) {
    ++metrics.list;
    validate_path(prefix);
    auto directory = object_path(prefix);
    if (!fs::exists(directory)) {
        return;
    }
    for (const auto& entry : fs::recursive_directory_iterator(directory)) {
        require(!entry.is_symlink(), "integrity", "symlink in local store");
        if (entry.path().filename() == "meta.json" && entry.is_regular_file()) {
            visit(entry.path().parent_path().lexically_relative(root_ / "objects").string());
        }
    }
}

} // namespace backup
