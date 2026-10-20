#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <sys/stat.h>

namespace backup {

class Fd {
public:
    explicit Fd(int fd);
    ~Fd();
    Fd(Fd&& other) noexcept;
    Fd& operator=(Fd&& other) noexcept;
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const { return fd_; }

private:
    int fd_;
};

struct FileInfo {
    std::string path;
    bool directory = false;
    int64_t device = 0;
    int64_t inode = 0;
    int64_t size = 0;
    int64_t mode = 0;
    int64_t mtime = 0;
    int64_t ctime = 0;
    bool same_identity(const FileInfo& other) const;
};

void io_error(const std::string& operation);
void validate_path(const std::string& path, bool allow_root = false);
bool is_within(const std::filesystem::path& child, const std::filesystem::path& parent);
FileInfo file_info(const std::string& path, const struct stat& value);
void scan_tree(const std::filesystem::path& source, const std::function<void(const FileInfo&)>& visit);
Fd open_source(const std::filesystem::path& source, const std::string& relative);
void write_all(int fd, const void* data, size_t size);
void sync_fd(int fd);
void sync_directory(const std::filesystem::path& path);
void atomic_write(const std::filesystem::path& path, const std::string& data);
std::string read_small(const std::filesystem::path& path, size_t limit);
void publish_directory(const std::filesystem::path& temporary, const std::filesystem::path& target);
std::string random_id();
int64_t now_ns();

class StateLock {
public:
    explicit StateLock(const std::filesystem::path& state);
private:
    Fd fd_;
};

} // namespace backup
