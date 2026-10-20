#include "backup/engine.h"
#include "backup/error.h"
#include "backup/fs.h"

#include <zstd.h>

#include <array>
#include <algorithm>
#include <chrono>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <unistd.h>

namespace backup {
namespace fs = std::filesystem;
namespace {

class Workspace {
public:
    explicit Workspace(const fs::path& parent) {
        path_ = parent / (".backupctl-" + random_id());
        require(fs::create_directory(path_), "io", "cannot create temporary workspace");
        fs::permissions(path_, fs::perms::owner_all, fs::perm_options::replace);
    }
    ~Workspace() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    const fs::path& path() const { return path_; }
private:
    fs::path path_;
};

void parent_exists(Database& index, const std::string& path) {
    if (path == ".") {
        return;
    }
    auto parent = fs::path(path).parent_path().string();
    auto query = index.prepare("SELECT kind FROM entries WHERE path=?");
    query.bind(1, parent.empty() ? "." : parent);
    require(query.row() && query.text(0) == "directory",
            "integrity", "missing parent directory or conflicting path");
}

void parse_manifest(Database& index, const fs::path& manifest, const Json& commit,
                     const std::string& repo) {
    index.exec(R"sql(
        CREATE TABLE entries (
            path TEXT PRIMARY KEY, kind TEXT NOT NULL, id INTEGER UNIQUE,
            size INTEGER NOT NULL, mode INTEGER NOT NULL, mtime INTEGER NOT NULL, hash TEXT NOT NULL
        );
        CREATE TABLE refs (
            file_id INTEGER NOT NULL, sequence INTEGER NOT NULL, size INTEGER NOT NULL,
            hash TEXT NOT NULL, key TEXT NOT NULL, PRIMARY KEY(file_id,sequence)
        );
    )sql");
    std::ifstream input(manifest, std::ios::binary);
    require(input.is_open(), "io", "cannot read downloaded manifest");
    std::array<char, max_record_size + 1> buffer{};
    bool header = false, ended = false, root = false;
    int64_t files = 0, directories = 0, bytes = 0, chunks = 0;
    int64_t active_file = 0, active_size = 0, active_bytes = 0, sequence = 0;
    auto finish_file = [&] {
        require(active_bytes == active_size, "integrity", "file references have the wrong total size");
    };
    // A disk index keeps path conflict checks independent of total manifest size.
    size_t batch = 0;
    std::unique_ptr<Transaction> transaction = std::make_unique<Transaction>(index);
    while (input.peek() != std::char_traits<char>::eof()) {
        check_cancelled();
        input.getline(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        require(!input.fail() && !input.eof(), "integrity", "oversized or unterminated manifest record");
        auto length = static_cast<size_t>(input.gcount());
        require(length > 1 && length <= max_record_size, "integrity", "invalid manifest record length");
        auto record = parse_json(std::string(buffer.data(), length - 1));
        auto type = string_field(record, "type");
        require(!ended, "integrity", "records after manifest end");
        if (!header) {
            require(type == "header" && integer(record, "version") == 1 &&
                        string_field(record, "repo_id") == repo &&
                        string_field(record, "snapshot_id") == commit.at("snapshot_id") &&
                        integer(record, "chunk_size") == static_cast<int64_t>(chunk_size) &&
                        string_field(record, "codec") == codec_name,
                    "integrity", "manifest header conflicts with commit");
            header = true;
            continue;
        }
        if (type == "directory" || type == "file") {
            finish_file();
            active_file = 0;
            active_size = 0;
            active_bytes = 0;
            sequence = 0;
            auto path = string_field(record, "path");
            validate_path(path, type == "directory");
            require(std::count(path.begin(), path.end(), '/') <= 256,
                    "integrity", "directory nesting exceeds limit");
            if (!root) {
                require(type == "directory" && path == ".", "integrity", "missing root directory");
                root = true;
            }
            parent_exists(index, path);
            int64_t size = 0;
            std::string hash;
            if (type == "file") {
                active_file = integer(record, "id", 1);
                require(active_file == files + 1, "integrity", "invalid file ID order");
                size = integer(record, "size");
                active_size = size;
                hash = string_field(record, "sha256");
                require(valid_hash(hash) && bytes <= INT64_MAX - size, "integrity", "invalid file size/hash");
                bytes += size;
                ++files;
            } else {
                require(files == 0, "integrity", "directory appears after file records");
                ++directories;
            }
            auto duplicate = index.prepare("SELECT 1 FROM entries WHERE path=?");
            duplicate.bind(1, path);
            require(!duplicate.row(), "integrity", "duplicate or conflicting manifest path");
            auto insert = index.prepare("INSERT INTO entries(path,kind,id,size,mode,mtime,hash) "
                                         "VALUES(?,?,NULLIF(?,0),?,?,?,?)");
            insert.bind(1, path).bind(2, type).bind(3, active_file).bind(4, size)
                  .bind(5, integer(record, "mode", 0, 0777))
                  .bind(6, integer(record, "mtime_ns", INT64_MIN))
                  .bind(7, hash).run();
        } else if (type == "chunk") {
            require(active_file > 0 && integer(record, "file_id", 1) == active_file &&
                        integer(record, "sequence") == sequence &&
                        string_field(record, "codec") == codec_name,
                    "integrity", "invalid chunk order or encoding");
            int64_t size = integer(record, "raw_size", 1, chunk_size);
            require(size == std::min<int64_t>(chunk_size, active_size - active_bytes),
                    "integrity", "invalid fixed-size chunk boundary");
            auto hash = string_field(record, "sha256");
            auto key = string_field(record, "key");
            require(valid_hash(hash) && key == chunk_key(hash), "integrity", "invalid chunk reference");
            index.prepare("INSERT INTO refs VALUES(?,?,?,?,?)").bind(1, active_file)
                .bind(2, sequence++).bind(3, size).bind(4, hash).bind(5, key).run();
            active_bytes += size;
            ++chunks;
        } else if (type == "end") {
            finish_file();
            for (const auto& [field, actual] : std::array<std::pair<std::string, int64_t>, 4>{
                     {{"files", files}, {"directories", directories}, {"bytes", bytes}, {"chunks", chunks}}}) {
                require(integer(record, field) == actual && integer(commit, field) == actual,
                        "integrity", "manifest totals conflict with commit");
            }
            ended = true;
        } else {
            throw Error("integrity", "unknown manifest record type");
        }
        if (++batch == 256) {
            transaction->commit();
            transaction = std::make_unique<Transaction>(index);
            batch = 0;
        }
    }
    require(input.eof() && header && root && ended, "integrity", "incomplete manifest");
    transaction->commit();
}

void load_manifest(Repository& repository, const Json& commit, const fs::path& workspace,
                    Database& index) {
    auto manifest = workspace / "manifest.jsonl";
    repository.store().get_file(string_field(commit, "manifest_key"), manifest,
                                static_cast<uint64_t>(integer(commit, "manifest_size")));
    require(fs::file_size(manifest) == static_cast<uint64_t>(integer(commit, "manifest_size")) &&
                hash_file(manifest) == string_field(commit, "manifest_sha256"),
            "integrity", "manifest hash or length mismatch");
    parse_manifest(index, manifest, commit, repository.id());
}

timespec timestamp(int64_t nanoseconds) {
    timespec value{};
    value.tv_sec = nanoseconds / 1000000000;
    value.tv_nsec = nanoseconds % 1000000000;
    if (value.tv_nsec < 0) {
        --value.tv_sec;
        value.tv_nsec += 1000000000;
    }
    return value;
}

void apply_metadata(int fd, int64_t mode, int64_t mtime) {
    timespec values[2]{{0, UTIME_OMIT}, timestamp(mtime)};
    if (fchmod(fd, static_cast<mode_t>(mode & 0777)) < 0 || futimens(fd, values) < 0) {
        io_error("restore file metadata");
    }
    sync_fd(fd);
}

Json read_files(Repository& repository, Database& index, const Json& commit,
                const std::optional<fs::path>& destination) {
    auto started = std::chrono::steady_clock::now();
    if (destination) {
        fs::create_directory(*destination);
        fs::permissions(*destination, fs::perms::owner_all, fs::perm_options::replace);
        auto directories = index.prepare("SELECT path FROM entries WHERE kind='directory' "
                                          "AND path!='.' ORDER BY length(path),path");
        while (directories.row()) {
            auto path = *destination / directories.text(0);
            require(fs::create_directory(path), "integrity", "restore directory conflict");
            fs::permissions(path, fs::perms::owner_all, fs::perm_options::replace);
        }
    }
    auto files = index.prepare("SELECT path,id,size,mode,mtime,hash FROM entries WHERE kind='file' ORDER BY id");
    uint64_t verified_chunks = 0;
    while (files.row()) {
        check_cancelled();
        std::optional<Fd> output;
        if (destination) {
            auto path = *destination / files.text(0);
            output.emplace(open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
        }
        auto references = index.prepare("SELECT size,hash,key FROM refs WHERE file_id=? ORDER BY sequence");
        references.bind(1, files.integer(1));
        Sha256 file_hash;
        int64_t total = 0;
        while (references.row()) {
            check_cancelled();
            auto encoded = repository.store().get(references.text(2), ZSTD_compressBound(chunk_size));
            auto raw = decompress_chunk(encoded, static_cast<size_t>(references.integer(0)));
            require(sha256(raw) == references.text(1), "integrity", "raw chunk hash mismatch");
            file_hash.update(raw);
            total += static_cast<int64_t>(raw.size());
            if (output) {
                write_all(output->get(), raw.data(), raw.size());
            }
            ++verified_chunks;
        }
        require(total == files.integer(2) && file_hash.finish() == files.text(5),
                "integrity", "restored file hash or length mismatch");
        if (output) {
            apply_metadata(output->get(), files.integer(3), files.integer(4));
        }
    }
    if (destination) {
        auto directories = index.prepare("SELECT path,mode,mtime FROM entries WHERE kind='directory' "
                                          "ORDER BY length(path) DESC,path DESC");
        while (directories.row()) {
            auto path = directories.text(0) == "." ? *destination : *destination / directories.text(0);
            Fd directory(open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            apply_metadata(directory.get(), directories.integer(1), directories.integer(2));
        }
    }
    return {{"ok", true}, {"snapshot_id", commit.at("snapshot_id")},
            {"verified_files", commit.at("files")}, {"verified_bytes", commit.at("bytes")},
            {"verified_chunks", verified_chunks},
            {"elapsed_seconds", std::chrono::duration<double>(
                std::chrono::steady_clock::now() - started).count()},
            {"store", repository.store().metrics.report()}};
}
} // namespace

Json Engine::verify(const std::string& snapshot) {
    auto commit = repository_.commit(snapshot);
    Workspace workspace(repository_.state());
    Database index(workspace.path() / "index.db");
    load_manifest(repository_, commit, workspace.path(), index);
    return read_files(repository_, index, commit, std::nullopt);
}

Json Engine::restore(const std::string& snapshot, const fs::path& requested_target) {
    auto target = fs::absolute(requested_target).lexically_normal();
    require(target != target.root_path() && !fs::exists(fs::symlink_status(target)),
            "configuration", "restore target must not exist");
    require(fs::is_directory(target.parent_path()), "configuration", "target parent directory is missing");
    auto commit = repository_.commit(snapshot);
    Workspace workspace(target.parent_path());
    Json result;
    {
        Database index(workspace.path() / "index.db");
        load_manifest(repository_, commit, workspace.path(), index);
        result = read_files(repository_, index, commit, workspace.path() / "data");
    }
    check_cancelled();
    checkpoint("restore_publish_before");
    publish_directory(workspace.path() / "data", target);
    checkpoint("restore_publish_after");
    result["target"] = target.string();
    return result;
}

} // namespace backup
