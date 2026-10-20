#include "backup/repository.h"
#include "backup/error.h"
#include "backup/fs.h"
#ifdef BACKUPCTL_S3
#include "backup/s3_store.h"
#endif

#include <algorithm>
#include <limits>

namespace backup {
namespace fs = std::filesystem;

bool valid_id(const std::string& value) {
    return value.size() == 32 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

Json parse_json(const std::string& value) {
    try {
        return Json::parse(value);
    } catch (const Json::exception&) {
        throw Error("integrity", "invalid JSON object");
    }
}

int64_t integer(const Json& value, const std::string& field, int64_t minimum, int64_t maximum) {
    require(value.is_object() && value.contains(field) && value.at(field).is_number_integer(),
            "integrity", "invalid integer field: " + field);
    const auto& number = value.at(field);
    if (number.is_number_unsigned()) {
        require(number.get<uint64_t>() <= static_cast<uint64_t>(INT64_MAX),
                "integrity", "integer overflow: " + field);
    }
    int64_t result = number.get<int64_t>();
    require(result >= minimum && result <= maximum, "integrity", "integer out of range: " + field);
    return result;
}

std::string string_field(const Json& value, const std::string& field) {
    require(value.is_object() && value.contains(field) && value.at(field).is_string(),
            "integrity", "invalid string field: " + field);
    return value.at(field).get<std::string>();
}

std::unique_ptr<Store> make_store(const Json& config) {
    auto backend = string_field(config, "backend");
    if (backend == "local") {
        return std::make_unique<LocalStore>(string_field(config, "local_store"));
    }
#ifdef BACKUPCTL_S3
    if (backend == "s3") {
        return std::make_unique<S3Store>(config);
    }
#endif
    throw Error("configuration", "storage backend is unavailable in this build");
}

namespace {
void validate_format(const Json& value) {
    require(integer(value, "version") == 1 && valid_id(string_field(value, "repo_id")) &&
                integer(value, "chunk_size") == static_cast<int64_t>(chunk_size) &&
                string_field(value, "codec") == codec_name,
            "configuration", "incompatible repository format");
}

Json remote_format(Store& store) {
    auto bytes = store.get("repo.json", max_record_size);
    Json value = parse_json(std::string(bytes.begin(), bytes.end()));
    validate_format(value);
    return value;
}
} // namespace

Json initialize(const fs::path& requested_state, Json config) {
    auto state = fs::absolute(requested_state).lexically_normal();
    require(state.has_parent_path() && state != state.root_path() && !fs::is_symlink(state),
            "configuration", "invalid state directory");
    fs::create_directories(state);
    fs::permissions(state, fs::perms::owner_all, fs::perm_options::replace);
    StateLock lock(state);
    auto store = make_store(config);
    Json format;
    if (store->head("repo.json")) {
        format = remote_format(*store);
    } else {
        format = {{"version", 1}, {"repo_id", random_id()}, {"chunk_size", chunk_size},
                  {"codec", codec_name}};
        auto text = format.dump();
        Bytes bytes(text.begin(), text.end());
        store->put("repo.json", bytes, {{"kind", "repository"}}, sha256(bytes));
        format = remote_format(*store);
    }
    config["repo_id"] = format.at("repo_id");
    if (fs::exists(state / "config.json")) {
        require(parse_json(read_small(state / "config.json", max_record_size)) == config,
                "configuration", "state directory belongs to another configuration");
    } else {
        atomic_write(state / "config.json", config.dump(2) + "\n");
    }
    Database database(state / "state.db");
    create_schema(database);
    return {{"ok", true}, {"repo_id", format.at("repo_id")}, {"state", state.string()}};
}

Repository::Repository(fs::path state) : state_(fs::absolute(std::move(state)).lexically_normal()) {
    require(!fs::is_symlink(state_), "configuration", "state directory is a symlink");
    config_ = parse_json(read_small(state_ / "config.json", max_record_size));
    id_ = string_field(config_, "repo_id");
    store_ = make_store(config_);
    auto format = remote_format(*store_);
    require(format.at("repo_id") == id_, "configuration", "repository ID does not match local state");
}

void Repository::validate_commit(const Json& value, const std::string& snapshot) const {
    require(valid_id(snapshot) && integer(value, "version") == 1 &&
                string_field(value, "repo_id") == id_ &&
                string_field(value, "snapshot_id") == snapshot &&
                string_field(value, "manifest_key") == "manifests/" + snapshot + ".jsonl" &&
                valid_hash(string_field(value, "manifest_sha256")),
            "integrity", "invalid commit descriptor");
    integer(value, "manifest_size", 1, static_cast<int64_t>(max_manifest_size));
    integer(value, "files");
    integer(value, "directories", 1);
    integer(value, "bytes");
    integer(value, "chunks");
    integer(value, "created_ns", 1);
}

Json Repository::commit(const std::string& snapshot) {
    require(valid_id(snapshot), "configuration", "invalid snapshot ID");
    auto bytes = store_->get("snapshots/" + snapshot + ".json", max_record_size);
    auto value = parse_json(std::string(bytes.begin(), bytes.end()));
    validate_commit(value, snapshot);
    return value;
}

} // namespace backup
