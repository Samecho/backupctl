#pragma once

#include "backup/repository.h"

#include <csignal>

namespace backup {

extern std::atomic<bool> cancelled;
void check_cancelled();

struct BackupOptions {
    size_t workers = 4;
    uint64_t memory_budget = 128ULL * 1024 * 1024;
};

class Engine {
public:
    explicit Engine(Repository& repository) : repository_(repository) {}
    Json backup(const std::filesystem::path& source, const BackupOptions& options);
    Json resume(const std::string& job);
    Json status(const std::string& job);
    Json verify(const std::string& snapshot);
    Json restore(const std::string& snapshot, const std::filesystem::path& target);
    const std::string& current_job() const { return current_job_; }

private:
    Json run(Database& database, const std::string& job);
    Repository& repository_;
    std::string current_job_;
};

} // namespace backup
