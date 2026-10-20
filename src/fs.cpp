#include "backup/fs.h"
#include "backup/codec.h"
#include "backup/error.h"

#include <nlohmann/json.hpp>
#include <openssl/rand.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <memory>
#include <sys/file.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace backup {
namespace fs = std::filesystem;

Fd::Fd(int fd) : fd_(fd) {
    if (fd < 0) {
        io_error("open file");
    }
}

Fd::~Fd() {
    if (fd_ >= 0) {
        close(fd_);
    }
}

Fd::Fd(Fd&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }

Fd& Fd::operator=(Fd&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) {
            close(fd_);
        }
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

void io_error(const std::string& operation) {
    int code = errno;
    throw Error(code == EACCES || code == EPERM ? "permission" : "io",
                operation + ": " + std::strerror(code));
}

void validate_path(const std::string& path, bool allow_root) {
    if (allow_root && path == ".") {
        return;
    }
    require(!path.empty() && path.size() <= 4095 && path.front() != '/' &&
                path.find('\0') == std::string::npos && path.back() != '/',
            "integrity", "invalid relative path");
    size_t start = 0;
    while (start < path.size()) {
        size_t end = path.find('/', start);
        auto part = path.substr(start, end == std::string::npos ? end : end - start);
        require(!part.empty() && part != "." && part != ".." && part.size() <= 255,
                "integrity", "invalid path component");
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    try {
        (void)nlohmann::json(path).dump();
    } catch (const nlohmann::json::exception&) {
        throw Error("unsupported", "non-UTF-8 path");
    }
}

bool is_within(const fs::path& child, const fs::path& parent) {
    auto normalized_child = fs::weakly_canonical(child);
    auto normalized_parent = fs::weakly_canonical(parent);
    auto a = normalized_child.begin();
    for (auto b = normalized_parent.begin(); b != normalized_parent.end(); ++b, ++a) {
        if (a == normalized_child.end() || *a != *b) {
            return false;
        }
    }
    return true;
}

FileInfo file_info(const std::string& path, const struct stat& value) {
    require(S_ISREG(value.st_mode) || S_ISDIR(value.st_mode),
            "unsupported", "unsupported file type: " + path);
    auto nanoseconds = [](const timespec& time) {
        require(time.tv_nsec >= 0 && time.tv_nsec < 1000000000 &&
                    time.tv_sec >= INT64_MIN / 1000000000 &&
                    time.tv_sec <= (INT64_MAX - time.tv_nsec) / 1000000000,
                "unsupported", "source timestamp exceeds the nanosecond format range");
        return time.tv_sec * 1000000000LL + time.tv_nsec;
    };
    require(value.st_size >= 0, "unsupported", "negative source file size");
    return {path, S_ISDIR(value.st_mode), static_cast<int64_t>(value.st_dev),
            static_cast<int64_t>(value.st_ino), S_ISDIR(value.st_mode) ? 0 : value.st_size,
            value.st_mode & 0777, nanoseconds(value.st_mtim), nanoseconds(value.st_ctim)};
}

bool FileInfo::same_identity(const FileInfo& other) const {
    return path == other.path && directory == other.directory && device == other.device &&
           inode == other.inode && size == other.size && mode == other.mode &&
           mtime == other.mtime && ctime == other.ctime;
}

namespace {
void scan_directory(int fd, const std::string& relative, size_t depth,
                    const std::function<void(const FileInfo&)>& visit) {
    require(depth <= 256, "unsupported", "directory nesting exceeds 256 levels");
    struct stat before{};
    if (fstat(fd, &before) < 0) {
        io_error("stat source directory");
    }
    auto info = file_info(relative, before);
    visit(info);
    DIR* raw = fdopendir(dup(fd));
    if (!raw) {
        io_error("read source directory");
    }
    auto close_directory = [](DIR* value) { closedir(value); };
    std::unique_ptr<DIR, decltype(close_directory)> directory(raw, close_directory);
    while (true) {
        errno = 0;
        auto* entry = readdir(directory.get());
        if (!entry) {
            if (errno) {
                io_error("read directory entry");
            }
            break;
        }
        std::string name(entry->d_name);
        if (name == "." || name == "..") {
            continue;
        }
        std::string path = relative == "." ? name : relative + "/" + name;
        validate_path(path);
        struct stat value{};
        if (fstatat(fd, name.c_str(), &value, AT_SYMLINK_NOFOLLOW) < 0) {
            io_error("stat source entry");
        }
        auto child = file_info(path, value);
        if (child.directory) {
            Fd next(openat(fd, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            struct stat opened{};
            if (fstat(next.get(), &opened) < 0) {
                io_error("stat opened directory");
            }
            require(child.same_identity(file_info(path, opened)), "source_changed",
                    "directory changed during scan");
            scan_directory(next.get(), path, depth + 1, visit);
        } else {
            visit(child);
        }
    }
    struct stat after{};
    if (fstat(fd, &after) < 0) {
        io_error("stat source directory");
    }
    require(info.same_identity(file_info(relative, after)), "source_changed",
            "directory changed during scan");
}
} // namespace

void scan_tree(const fs::path& source, const std::function<void(const FileInfo&)>& visit) {
    Fd root(open(source.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    scan_directory(root.get(), ".", 0, visit);
}

Fd open_source(const fs::path& source, const std::string& relative) {
    validate_path(relative);
    Fd current(open(source.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    fs::path path(relative);
    for (auto part = path.begin(); part != path.end(); ++part) {
        auto next = part;
        ++next;
        int flags = O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK;
        if (next != path.end()) {
            flags |= O_DIRECTORY;
        }
        current = Fd(openat(current.get(), part->c_str(), flags));
    }
    return current;
}

void write_all(int fd, const void* data, size_t size) {
#ifdef BACKUPCTL_TESTING
    if (std::getenv("BACKUPCTL_TEST_DISK_FULL")) {
        std::array<char, 4096> path{};
        auto link = "/proc/self/fd/" + std::to_string(fd);
        ssize_t length = readlink(link.c_str(), path.data(), path.size() - 1);
        if (length > 0) {
            std::string name(path.data(), static_cast<size_t>(length));
            if (name.find("/.backupctl-") != std::string::npos && name.find("/data/") != std::string::npos) {
                errno = ENOSPC;
                io_error("write restored file");
            }
        }
    }
#endif
    const auto* bytes = static_cast<const unsigned char*>(data);
    while (size > 0) {
        ssize_t written = write(fd, bytes, size);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            io_error("write file");
        }
        bytes += written;
        size -= static_cast<size_t>(written);
    }
}

void sync_fd(int fd) {
    if (fsync(fd) < 0) {
        io_error("sync file");
    }
}

void sync_directory(const fs::path& path) {
    Fd fd(open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    sync_fd(fd.get());
}

void atomic_write(const fs::path& path, const std::string& data) {
    fs::path temporary = path.string() + ".tmp-" + random_id();
    try {
        Fd fd(open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
        write_all(fd.get(), data.data(), data.size());
        sync_fd(fd.get());
        fs::rename(temporary, path);
        sync_directory(path.parent_path());
    } catch (...) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        throw;
    }
}

std::string read_small(const fs::path& path, size_t limit) {
    Fd fd(open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    struct stat info{};
    if (fstat(fd.get(), &info) < 0) {
        io_error("stat bounded input file");
    }
    require(S_ISREG(info.st_mode), "integrity", "bounded input must be a regular file");
    std::string value;
    std::array<char, 8192> buffer{};
    while (true) {
        ssize_t size = read(fd.get(), buffer.data(), buffer.size());
        if (size < 0 && errno == EINTR) {
            continue;
        }
        if (size < 0) {
            io_error("read file");
        }
        if (size == 0) {
            break;
        }
        require(value.size() + static_cast<size_t>(size) <= limit,
                "integrity", "object exceeds size limit");
        value.append(buffer.data(), static_cast<size_t>(size));
    }
    return value;
}

void publish_directory(const fs::path& temporary, const fs::path& target) {
    if (syscall(SYS_renameat2, AT_FDCWD, temporary.c_str(), AT_FDCWD, target.c_str(),
                RENAME_NOREPLACE) < 0) {
        io_error("publish directory without replacement");
    }
    sync_directory(target.parent_path());
}

std::string random_id() {
    std::array<unsigned char, 16> bytes{};
    require(RAND_bytes(bytes.data(), bytes.size()) == 1, "io", "cannot generate random ID");
    return sha256(bytes).substr(0, 32);
}

int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

StateLock::StateLock(const fs::path& state)
    : fd_(open((state / "writer.lock").c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600)) {
    if (flock(fd_.get(), LOCK_EX | LOCK_NB) < 0) {
        throw Error("busy", "another writer owns this state directory");
    }
}

} // namespace backup
