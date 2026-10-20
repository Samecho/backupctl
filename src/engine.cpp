#include "backup/engine.h"
#include "backup/error.h"
#include "backup/fs.h"
#include "backup/thread_pool.h"

#include <zstd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <deque>
#include <fcntl.h>
#include <fstream>
#include <sys/resource.h>
#include <unistd.h>

namespace backup {
namespace fs = std::filesystem;
static_assert(std::atomic<bool>::is_always_lock_free);
std::atomic<bool> cancelled{false};

void check_cancelled() {
    require(!cancelled, "cancelled", "operation cancelled; the same job can be resumed");
}

namespace {
FileInfo indexed_file(Statement& query, int offset = 0) {
    return {query.text(offset), query.integer(offset + 1) != 0, query.integer(offset + 2),
            query.integer(offset + 3), query.integer(offset + 4), query.integer(offset + 5),
            query.integer(offset + 6), query.integer(offset + 7)};
}

void insert_file(Database& database, const std::string& job, const FileInfo& file) {
    auto query = database.prepare("INSERT INTO files(job,path,directory,device,inode,size,mode,mtime,ctime) "
                                  "VALUES(?,?,?,?,?,?,?,?,?)");
    query.bind(1, job).bind(2, file.path).bind(3, file.directory ? 1 : 0).bind(4, file.device)
         .bind(5, file.inode).bind(6, file.size).bind(7, file.mode).bind(8, file.mtime)
         .bind(9, file.ctime).run();
}

void set_phase(Database& database, const std::string& job, const std::string& phase) {
    database.prepare("UPDATE jobs SET phase=?,error_category='',error_message='' WHERE id=?")
        .bind(1, phase).bind(2, job).run();
}

void check_inventory(Database& database, const std::string& job, const fs::path& source) {
    int64_t count = 0;
    scan_tree(source, [&](const FileInfo& file) {
        check_cancelled();
        auto query = database.prepare("SELECT path,directory,device,inode,size,mode,mtime,ctime "
                                      "FROM files WHERE job=? AND path=?");
        query.bind(1, job).bind(2, file.path);
        require(query.row() && indexed_file(query).same_identity(file),
                "source_changed", "source inventory changed: " + file.path);
        ++count;
    });
    auto query = database.prepare("SELECT count(*) FROM files WHERE job=?");
    query.bind(1, job);
    require(query.row() && query.integer(0) == count,
            "source_changed", "source file or directory was removed");
}

struct ChunkResult {
    std::string hash;
    int64_t raw_size = 0;
    int64_t encoded_size = 0;
    std::string encoded_hash;
    bool uploaded = false;
};

struct PipelineTiming {
    std::atomic<int64_t> hash_ns{0}, compression_ns{0}, storage_ns{0};
};

int64_t elapsed_ns(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count();
}

ChunkResult store_chunk(Repository& repository, const Bytes& raw, PipelineTiming& timing,
                         std::array<std::mutex, 64>& locks) {
    check_cancelled();
    auto started = std::chrono::steady_clock::now();
    ChunkResult result{sha256(raw), static_cast<int64_t>(raw.size()), 0, "", false};
    timing.hash_ns += elapsed_ns(started);
    // Fixed lock striping prevents duplicate concurrent PUTs without an unbounded hash set.
    std::lock_guard<std::mutex> chunk_lock(locks[std::hash<std::string>{}(result.hash) % locks.size()]);
    started = std::chrono::steady_clock::now();
    auto key = chunk_key(result.hash);
    Metadata metadata{{"repo-id", repository.id()}, {"raw-sha256", result.hash},
                      {"raw-size", std::to_string(raw.size())}, {"codec", std::string(codec_name)}};
    auto inspect = [&](const ObjectInfo& info) {
        for (const auto& [name, value] : metadata) {
            auto found = info.metadata.find(name);
            require(found != info.metadata.end() && found->second == value,
                    "integrity", "existing chunk metadata conflicts: " + key);
        }
        auto checksum = info.metadata.find("encoded-sha256");
        require(checksum != info.metadata.end() && valid_hash(checksum->second) &&
                    info.size > 0 && info.size <= ZSTD_compressBound(raw.size()),
                "integrity", "invalid encoded chunk metadata");
        result.encoded_size = static_cast<int64_t>(info.size);
        result.encoded_hash = checksum->second;
    };
    auto existing = repository.store().head(key);
    if (existing) {
        inspect(*existing);
        timing.storage_ns += elapsed_ns(started);
        return result;
    }
    timing.storage_ns += elapsed_ns(started);
    started = std::chrono::steady_clock::now();
    auto encoded = compress_chunk(raw);
    result.encoded_hash = sha256(encoded);
    result.encoded_size = static_cast<int64_t>(encoded.size());
    timing.compression_ns += elapsed_ns(started);
    started = std::chrono::steady_clock::now();
    checkpoint("chunk_put_before");
    try {
        result.uploaded = repository.store().put(key, encoded, metadata, result.encoded_hash);
    } catch (const Error& error) {
        if (error.category() != "network_exhausted") {
            throw;
        }
        auto reconciled = repository.store().head(key);
        if (!reconciled) {
            throw;
        }
        inspect(*reconciled);
    }
    checkpoint("chunk_put_after");
    auto confirmed = repository.store().head(key);
    require(confirmed.has_value(), "integrity", "uploaded chunk is missing");
    inspect(*confirmed);
    timing.storage_ns += elapsed_ns(started);
    return result;
}

void record_chunk(Database& database, const std::string& job, const std::string& path,
                   int64_t sequence, const ChunkResult& result) {
    checkpoint("checkpoint_before");
    Transaction transaction(database);
    auto existing = database.prepare("SELECT raw_size,encoded_size,encoded_hash FROM chunks WHERE hash=?");
    existing.bind(1, result.hash);
    if (existing.row()) {
        require(existing.integer(0) == result.raw_size && existing.integer(1) == result.encoded_size &&
                    existing.text(2) == result.encoded_hash,
                "integrity", "local chunk index conflicts with remote metadata");
    } else {
        database.prepare("INSERT INTO chunks VALUES(?,?,?,?)")
            .bind(1, result.hash).bind(2, result.raw_size).bind(3, result.encoded_size)
            .bind(4, result.encoded_hash).run();
    }
    // A reference is durable only after the remote object has been confirmed.
    auto previous = database.prepare("SELECT hash FROM file_chunks WHERE job=? AND path=? AND sequence=?");
    previous.bind(1, job).bind(2, path).bind(3, sequence);
    if (previous.row()) {
        require(previous.text(0) == result.hash, "source_changed", "resumed chunk content changed");
    } else {
        database.prepare("INSERT INTO file_chunks(job,path,sequence,hash) VALUES(?,?,?,?)")
            .bind(1, job).bind(2, path).bind(3, sequence).bind(4, result.hash).run();
        database.prepare("UPDATE checkpoints SET confirmed=confirmed+? WHERE job=?")
            .bind(1, result.raw_size).bind(2, job).run();
    }
    transaction.commit();
    checkpoint("checkpoint_after");
}

Json write_manifest(Database& database, Repository& repository, const std::string& job,
                    const std::string& snapshot, int64_t created, const fs::path& destination) {
    Fd output(open(destination.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600));
    Sha256 hash;
    uint64_t length = 0;
    auto emit = [&](const Json& value) {
        auto line = value.dump() + "\n";
        require(line.size() <= max_record_size, "integrity", "manifest record is too large");
        length += line.size();
        require(length <= max_manifest_size, "configuration", "manifest exceeds 1 GiB format limit");
        hash.update(std::span(reinterpret_cast<const unsigned char*>(line.data()), line.size()));
        write_all(output.get(), line.data(), line.size());
    };
    emit({{"type", "header"}, {"version", 1}, {"repo_id", repository.id()},
          {"snapshot_id", snapshot}, {"chunk_size", chunk_size}, {"codec", codec_name}});
    int64_t directories = 0, files = 0, bytes = 0, chunks = 0;
    auto dirs = database.prepare("SELECT path,mode,mtime FROM files WHERE job=? AND directory=1 "
                                 "ORDER BY length(path),path");
    dirs.bind(1, job);
    while (dirs.row()) {
        emit({{"type", "directory"}, {"path", dirs.text(0)},
              {"mode", dirs.integer(1)}, {"mtime_ns", dirs.integer(2)}});
        ++directories;
    }
    auto input = database.prepare("SELECT path,size,mode,mtime,hash,complete FROM files "
                                  "WHERE job=? AND directory=0 ORDER BY path");
    input.bind(1, job);
    while (input.row()) {
        check_cancelled();
        require(input.integer(5) == 1 && valid_hash(input.text(4)), "integrity", "incomplete file");
        ++files;
        bytes += input.integer(1);
        emit({{"type", "file"}, {"id", files}, {"path", input.text(0)},
              {"size", input.integer(1)}, {"mode", input.integer(2)},
              {"mtime_ns", input.integer(3)}, {"sha256", input.text(4)}});
        auto references = database.prepare("SELECT f.sequence,c.raw_size,c.hash FROM file_chunks f "
                                             "JOIN chunks c ON f.hash=c.hash WHERE job=? AND path=? "
                                             "ORDER BY sequence");
        references.bind(1, job).bind(2, input.text(0));
        while (references.row()) {
            emit({{"type", "chunk"}, {"file_id", files}, {"sequence", references.integer(0)},
                  {"raw_size", references.integer(1)}, {"sha256", references.text(2)},
                  {"codec", codec_name}, {"key", chunk_key(references.text(2))}});
            ++chunks;
        }
    }
    emit({{"type", "end"}, {"files", files}, {"directories", directories},
          {"bytes", bytes}, {"chunks", chunks}});
    sync_fd(output.get());
    return {{"version", 1}, {"repo_id", repository.id()}, {"snapshot_id", snapshot},
            {"manifest_key", "manifests/" + snapshot + ".jsonl"},
            {"manifest_size", length}, {"manifest_sha256", hash.finish()},
            {"files", files}, {"directories", directories}, {"bytes", bytes},
            {"chunks", chunks}, {"created_ns", created}};
}

void confirm_file_object(Store& store, const std::string& key, const fs::path& file,
                         const Metadata& metadata, const std::string& hash) {
    bool upload = !store.head(key).has_value();
    if (upload) {
        try {
            store.put_file(key, file, metadata, hash);
        } catch (const Error& error) {
            if (error.category() != "network_exhausted") {
                throw;
            }
        }
    }
    auto info = store.head(key);
    require(info.has_value() && info->size == fs::file_size(file),
            "integrity", "manifest upload is missing or has the wrong size");
    for (const auto& [name, expected] : metadata) {
        auto found = info->metadata.find(name);
        require(found != info->metadata.end() && found->second == expected,
                "integrity", "manifest metadata conflicts");
    }
    auto download = file.string() + ".confirmed";
    std::error_code ignored;
    fs::remove(download, ignored);
    store.get_file(key, download, max_manifest_size);
    bool matches = hash_file(download) == hash;
    fs::remove(download, ignored);
    require(matches, "integrity", "remote manifest content conflicts");
}
} // namespace

Json Engine::backup(const fs::path& requested_source, const BackupOptions& options) {
    require(options.workers >= 1 && options.workers <= 64 && options.memory_budget >= 32ULL * 1024 * 1024,
            "configuration", "invalid workers or data buffer budget");
    require(!fs::is_symlink(fs::symlink_status(requested_source)), "unsupported", "source is a symlink");
    auto source = fs::canonical(requested_source);
    require(!is_within(repository_.state(), source), "configuration", "state directory is inside source");
    if (repository_.config().at("backend") == "local") {
        auto local = fs::path(string_field(repository_.config(), "local_store"));
        require(!is_within(local, source) && !is_within(source, local),
                "configuration", "source and local store must be separate");
    }
    StateLock lock(repository_.state());
    Database database(repository_.state() / "state.db");
    create_schema(database);
    current_job_ = random_id();
    std::string snapshot = random_id();
    {
        Transaction transaction(database);
        database.prepare("INSERT INTO jobs(id,snapshot,repo,source,phase,created,workers,budget) "
                         "VALUES(?,?,?,?,'scanning',?,?,?)")
            .bind(1, current_job_).bind(2, snapshot).bind(3, repository_.id()).bind(4, source.string())
            .bind(5, now_ns()).bind(6, static_cast<int64_t>(options.workers))
            .bind(7, static_cast<int64_t>(options.memory_budget)).run();
        database.prepare("INSERT INTO checkpoints(job) VALUES(?)").bind(1, current_job_).run();
        transaction.commit();
    }
    return run(database, current_job_);
}

Json Engine::resume(const std::string& job) {
    require(valid_id(job), "configuration", "invalid job ID");
    current_job_ = job;
    StateLock lock(repository_.state());
    Database database(repository_.state() / "state.db");
    return run(database, job);
}

Json Engine::status(const std::string& job) {
    require(valid_id(job), "configuration", "invalid job ID");
    Database database(repository_.state() / "state.db", true);
    auto query = database.prepare("SELECT snapshot,phase,error_category,error_message,confirmed "
                                  "FROM jobs JOIN checkpoints ON jobs.id=checkpoints.job WHERE id=?");
    query.bind(1, job);
    require(query.row(), "configuration", "job does not exist");
    return {{"ok", true}, {"job_id", job}, {"snapshot_id", query.text(0)},
            {"phase", query.text(1)}, {"error_category", query.text(2)},
            {"error_message", query.text(3)}, {"confirmed_raw_bytes", query.integer(4)},
            {"remote_committed", query.text(1) == "committed"}};
}

Json Engine::run(Database& database, const std::string& job) {
    auto started = std::chrono::steady_clock::now();
    auto query = database.prepare("SELECT snapshot,repo,source,phase,created,workers,budget,commit_json "
                                  "FROM jobs WHERE id=?");
    query.bind(1, job);
    require(query.row() && query.text(1) == repository_.id(), "configuration", "job/repository mismatch");
    auto snapshot = query.text(0);
    fs::path source(query.text(2));
    auto phase = query.text(3);
    int64_t created = query.integer(4);
    size_t requested_workers = static_cast<size_t>(query.integer(5));
    uint64_t budget = static_cast<uint64_t>(query.integer(6));
    // Include raw bytes, codec buffers, SDK stream copies, and codec context allowance.
    uint64_t slot_charge = chunk_size + 4 * ZSTD_compressBound(chunk_size) + 2 * 1024 * 1024;
    require(budget >= chunk_size + slot_charge, "configuration", "data budget cannot fit one chunk task");
    size_t limit = static_cast<size_t>(std::min<uint64_t>((budget - chunk_size) / slot_charge,
                                                         2 * requested_workers));
    size_t workers = std::min(requested_workers, limit);
    size_t peak_inflight = 0;
    size_t peak_queue = 0;
    PipelineTiming timing;
    std::array<std::mutex, 64> chunk_locks;
    std::unique_ptr<ThreadPool> pool;
    if (workers > 1) {
        pool = std::make_unique<ThreadPool>(workers, limit);
    }
    auto saved_commit = query.text(7);
    auto commit_key = "snapshots/" + snapshot + ".json";
    uint64_t uploaded_chunks = 0, uploaded_raw = 0, uploaded_payload = 0, reused = 0;
    try {
        if (!saved_commit.empty() && repository_.store().head(commit_key)) {
            require(repository_.commit(snapshot).dump() == saved_commit,
                    "integrity", "snapshot commit conflicts with the pending job");
            set_phase(database, job, "committed");
            return {{"ok", true}, {"job_id", job}, {"snapshot_id", snapshot},
                    {"phase", "committed"}, {"reconciled", true},
                    {"chunk_upload_bytes", 0}, {"store", repository_.store().metrics.report()}};
        }
        require(phase != "committed", "integrity", "previously committed snapshot is missing");
        if (phase == "scanning") {
            Transaction transaction(database);
            database.prepare("DELETE FROM files WHERE job=?").bind(1, job).run();
            scan_tree(source, [&](const FileInfo& file) {
                check_cancelled();
                insert_file(database, job, file);
            });
            set_phase(database, job, "uploading");
            transaction.commit();
            checkpoint("scan_after");
        }
        check_inventory(database, job, source);
        set_phase(database, job, "uploading");
        auto files = database.prepare("SELECT path,directory,device,inode,size,mode,mtime,ctime,hash "
                                      "FROM files WHERE job=? AND directory=0 ORDER BY path");
        files.bind(1, job);
        while (files.row()) {
            check_cancelled();
            auto expected = indexed_file(files);
            Fd input = open_source(source, expected.path);
            struct stat before{};
            if (fstat(input.get(), &before) < 0) {
                io_error("stat source file");
            }
            require(expected.same_identity(file_info(expected.path, before)),
                    "source_changed", "source file changed before read");
            Sha256 hash;
            int64_t total = 0, sequence = 0;
            std::deque<std::pair<int64_t, std::future<ChunkResult>>> pending;
            auto consume = [&](int64_t index, const ChunkResult& result) {
                if (result.uploaded) {
                    ++uploaded_chunks;
                    uploaded_raw += static_cast<uint64_t>(result.raw_size);
                    uploaded_payload += static_cast<uint64_t>(result.encoded_size);
                } else {
                    reused += static_cast<uint64_t>(result.raw_size);
                }
                record_chunk(database, job, expected.path, index, result);
            };
            auto drain = [&] {
                auto& first = pending.front();
                consume(first.first, first.second.get());
                pending.pop_front();
            };
            while (total < expected.size) {
                Bytes raw(static_cast<size_t>(std::min<int64_t>(chunk_size, expected.size - total)));
                size_t offset = 0;
                while (offset < raw.size()) {
                    ssize_t size = read(input.get(), raw.data() + offset, raw.size() - offset);
                    if (size < 0 && errno == EINTR) {
                        continue;
                    }
                    if (size < 0) {
                        io_error("read source file");
                    }
                    require(size != 0, "source_changed", "source file became shorter");
                    offset += static_cast<size_t>(size);
                }
                hash.update(raw);
                total += static_cast<int64_t>(raw.size());
                if (pool) {
                    pending.emplace_back(sequence++, pool->enqueue([&, raw = std::move(raw)] {
                        return store_chunk(repository_, raw, timing, chunk_locks);
                    }));
                    peak_inflight = std::max(peak_inflight, pending.size());
                    if (pending.size() == limit) {
                        drain();
                    }
                } else {
                    consume(sequence++, store_chunk(repository_, raw, timing, chunk_locks));
                    peak_inflight = 1;
                }
            }
            while (!pending.empty()) {
                drain();
            }
            struct stat after{};
            if (fstat(input.get(), &after) < 0) {
                io_error("stat source file after read");
            }
            require(expected.same_identity(file_info(expected.path, after)),
                    "source_changed", "source file changed while reading");
            auto digest = hash.finish();
            require(files.null(8) || files.text(8) == digest,
                    "source_changed", "resumed file content changed");
            database.prepare("UPDATE files SET hash=?,complete=1 WHERE job=? AND path=?")
                .bind(1, digest).bind(2, job).bind(3, expected.path).run();
        }
        if (pool) {
            peak_queue = pool->peak_queue();
            pool->shutdown();
        }
        check_inventory(database, job, source);
        checkpoint("source_checked");
        set_phase(database, job, "manifest");
        fs::path manifest = repository_.state() / ("manifest-" + job + ".jsonl");
        auto commit = write_manifest(database, repository_, job, snapshot, created, manifest);
        confirm_file_object(repository_.store(), string_field(commit, "manifest_key"), manifest,
                            {{"repo-id", repository_.id()}, {"kind", "manifest"}},
                            string_field(commit, "manifest_sha256"));
        checkpoint("manifest_after");
        // Source checks are repeated after the potentially slow manifest upload.
        check_inventory(database, job, source);
        auto text = commit.dump();
        require(saved_commit.empty() || saved_commit == text, "integrity", "pending commit changed");
        database.prepare("UPDATE jobs SET phase='committing',commit_json=? WHERE id=?")
            .bind(1, text).bind(2, job).run();
        checkpoint("commit_before");
        bool uncertain = false;
        try {
            Bytes bytes(text.begin(), text.end());
            repository_.store().put(commit_key, bytes,
                                    {{"repo-id", repository_.id()}, {"kind", "commit"}}, sha256(bytes));
        } catch (const Error& error) {
            if (error.category() != "network_exhausted") {
                throw;
            }
            uncertain = true;
        }
        checkpoint("commit_after");
        try {
            require(repository_.commit(snapshot).dump() == text,
                    "integrity", "published snapshot conflicts with this job");
        } catch (const Error& error) {
            if (!uncertain && error.category() == "integrity") {
                throw;
            }
            set_phase(database, job, "commit_unknown");
            throw Error("commit_unknown", "remote commit outcome requires reconciliation with resume");
        }
        set_phase(database, job, "committed");
        checkpoint("committed_after");
        std::error_code ignored;
        fs::remove(manifest, ignored);
        double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        struct rusage usage{};
        getrusage(RUSAGE_SELF, &usage);
        return {{"ok", true}, {"job_id", job}, {"snapshot_id", snapshot}, {"phase", "committed"},
                {"logical_bytes", commit.at("bytes")}, {"files", commit.at("files")},
                {"new_chunks", uploaded_chunks}, {"new_raw_bytes", uploaded_raw},
                {"reused_raw_bytes", reused}, {"chunk_upload_bytes", uploaded_payload},
                {"elapsed_seconds", elapsed}, {"peak_rss_kib", usage.ru_maxrss},
                {"requested_workers", requested_workers}, {"active_workers", workers},
                {"inflight_limit", limit}, {"peak_inflight_chunks", peak_inflight},
                {"peak_queued_chunks", peak_queue}, {"data_budget_bytes", budget},
                {"data_buffer_bound_bytes", chunk_size + limit * slot_charge},
                {"hash_worker_seconds", timing.hash_ns.load() / 1e9},
                {"compression_worker_seconds", timing.compression_ns.load() / 1e9},
                {"storage_worker_seconds", timing.storage_ns.load() / 1e9},
                {"store", repository_.store().metrics.report()}};
    } catch (const Error& error) {
        database.prepare("UPDATE jobs SET error_category=?,error_message=? WHERE id=?")
            .bind(1, error.category()).bind(2, error.what()).bind(3, job).run();
        throw;
    }
}

} // namespace backup
