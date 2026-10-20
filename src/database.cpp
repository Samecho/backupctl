#include "backup/database.h"
#include "backup/error.h"

namespace backup {
namespace {
void check(int code, sqlite3* database) {
    if (code != SQLITE_OK) {
        throw Error(code == SQLITE_PERM || code == SQLITE_READONLY || code == SQLITE_AUTH ?
                        "permission" : "io", "SQLite: " + std::string(sqlite3_errmsg(database)));
    }
}
} // namespace

Statement::Statement(sqlite3* database, const std::string& sql) {
    check(sqlite3_prepare_v2(database, sql.c_str(), -1, &statement_, nullptr), database);
}

Statement::~Statement() { sqlite3_finalize(statement_); }

Statement& Statement::bind(int column, std::string_view value) {
    check(sqlite3_bind_text64(statement_, column, value.data(), value.size(), SQLITE_TRANSIENT,
                              SQLITE_UTF8), sqlite3_db_handle(statement_));
    return *this;
}

Statement& Statement::bind(int column, int64_t value) {
    check(sqlite3_bind_int64(statement_, column, value), sqlite3_db_handle(statement_));
    return *this;
}

bool Statement::row() {
    int code = sqlite3_step(statement_);
    if (code == SQLITE_ROW) {
        return true;
    }
    if (code == SQLITE_DONE) {
        return false;
    }
    check(code, sqlite3_db_handle(statement_));
    return false;
}

void Statement::run() { require(!row(), "io", "unexpected SQLite result"); }

std::string Statement::text(int column) const {
    const auto* value = sqlite3_column_text(statement_, column);
    return value ? std::string(reinterpret_cast<const char*>(value),
                               static_cast<size_t>(sqlite3_column_bytes(statement_, column))) : "";
}

int64_t Statement::integer(int column) const { return sqlite3_column_int64(statement_, column); }
bool Statement::null(int column) const { return sqlite3_column_type(statement_, column) == SQLITE_NULL; }

Database::Database(const std::filesystem::path& path, bool readonly) {
    int flags = readonly ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
    int code = sqlite3_open_v2(path.c_str(), &database_, flags | SQLITE_OPEN_NOMUTEX, nullptr);
    if (code != SQLITE_OK) {
        std::string message = database_ ? sqlite3_errmsg(database_) : "cannot open database";
        sqlite3_close(database_);
        database_ = nullptr;
        throw Error("io", "SQLite: " + message);
    }
    try {
        sqlite3_busy_timeout(database_, 5000);
        exec("PRAGMA foreign_keys=ON; PRAGMA temp_store=FILE; PRAGMA cache_size=-4096;");
        if (!readonly) {
            exec("PRAGMA journal_mode=DELETE; PRAGMA synchronous=EXTRA;");
        }
    } catch (...) {
        sqlite3_close(database_);
        database_ = nullptr;
        throw;
    }
}

Database::~Database() { sqlite3_close(database_); }

void Database::exec(const std::string& sql) {
    check(sqlite3_exec(database_, sql.c_str(), nullptr, nullptr, nullptr), database_);
}

int64_t Database::changes() const { return sqlite3_changes64(database_); }

Transaction::Transaction(Database& database) : database_(database) {
    database_.exec("BEGIN IMMEDIATE");
}

Transaction::~Transaction() {
    if (!committed_) {
        try {
            database_.exec("ROLLBACK");
        } catch (...) {
        }
    }
}

void Transaction::commit() {
    database_.exec("COMMIT");
    committed_ = true;
}

void create_schema(Database& database) {
    auto version = database.prepare("PRAGMA user_version");
    require(version.row() && version.integer(0) <= 1,
            "configuration", "unsupported local database version");
    database.exec(R"sql(
        CREATE TABLE IF NOT EXISTS jobs (
            id TEXT PRIMARY KEY, snapshot TEXT NOT NULL, repo TEXT NOT NULL,
            source TEXT NOT NULL, phase TEXT NOT NULL, created INTEGER NOT NULL,
            workers INTEGER NOT NULL, budget INTEGER NOT NULL,
            error_category TEXT NOT NULL DEFAULT '', error_message TEXT NOT NULL DEFAULT '',
            commit_json TEXT NOT NULL DEFAULT ''
        );
        CREATE TABLE IF NOT EXISTS files (
            job TEXT NOT NULL REFERENCES jobs(id), path TEXT NOT NULL,
            directory INTEGER NOT NULL, device INTEGER NOT NULL, inode INTEGER NOT NULL,
            size INTEGER NOT NULL, mode INTEGER NOT NULL, mtime INTEGER NOT NULL, ctime INTEGER NOT NULL,
            hash TEXT, complete INTEGER NOT NULL DEFAULT 0, PRIMARY KEY(job,path)
        );
        CREATE TABLE IF NOT EXISTS chunks (
            hash TEXT PRIMARY KEY, raw_size INTEGER NOT NULL,
            encoded_size INTEGER NOT NULL, encoded_hash TEXT NOT NULL
        );
        CREATE TABLE IF NOT EXISTS file_chunks (
            job TEXT NOT NULL, path TEXT NOT NULL, sequence INTEGER NOT NULL,
            hash TEXT NOT NULL REFERENCES chunks(hash), PRIMARY KEY(job,path,sequence),
            FOREIGN KEY(job,path) REFERENCES files(job,path)
        );
        CREATE TABLE IF NOT EXISTS checkpoints (
            job TEXT PRIMARY KEY REFERENCES jobs(id), confirmed INTEGER NOT NULL DEFAULT 0
        );
        PRAGMA user_version=1;
    )sql");
}

} // namespace backup
