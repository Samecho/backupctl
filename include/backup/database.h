#pragma once

#include <sqlite3.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace backup {

class Statement {
public:
    Statement(sqlite3* database, const std::string& sql);
    ~Statement();
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    Statement& bind(int column, std::string_view value);
    Statement& bind(int column, int64_t value);
    bool row();
    void run();
    std::string text(int column) const;
    int64_t integer(int column) const;
    bool null(int column) const;

private:
    sqlite3_stmt* statement_ = nullptr;
};

class Database {
public:
    explicit Database(const std::filesystem::path& path, bool readonly = false);
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    void exec(const std::string& sql);
    Statement prepare(const std::string& sql) { return Statement(database_, sql); }
    int64_t changes() const;

private:
    sqlite3* database_ = nullptr;
};

class Transaction {
public:
    explicit Transaction(Database& database);
    ~Transaction();
    void commit();
private:
    Database& database_;
    bool committed_ = false;
};

void create_schema(Database& database);

} // namespace backup
