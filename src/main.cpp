#include "backup/engine.h"
#include "backup/error.h"
#include "backup/fs.h"

#include <charconv>
#include <chrono>
#include <csignal>
#include <iostream>
#include <map>
#include <sys/stat.h>

namespace fs = std::filesystem;
using namespace backup;

namespace {
const char* help = R"help(backupctl: incremental backups for static Linux directories

  init    --state DIR (--local-store DIR | --bucket NAME --prefix PATH --region REGION)
  backup  --state DIR --source DIR [--workers N] [--memory-mib N]
  resume  --state DIR --job ID
  status  --state DIR --job ID
  list    --state DIR
  verify  --state DIR --snapshot ID
  restore --state DIR --snapshot ID --target DIR

Results are JSON. list emits one JSON object per committed snapshot.
The target must not exist. State and storage must be outside the static source.
Credentials use the AWS SDK default chain, including AWS_PROFILE. Never pass keys.
)help";

class Arguments {
public:
    Arguments(int argc, char** argv) {
        require(argc >= 2, "configuration", "a command is required; see --help");
        command = argv[1];
        for (int i = 2; i < argc; i += 2) {
            std::string name = argv[i];
            require(name.starts_with("--") && i + 1 < argc && !values_.contains(name),
                    "configuration", "invalid or repeated option: " + name);
            values_.emplace(name, argv[i + 1]);
        }
    }
    std::string take(const std::string& key, const std::string& fallback = "") {
        auto found = values_.find(key);
        if (found == values_.end()) {
            return fallback;
        }
        auto result = found->second;
        values_.erase(found);
        return result;
    }
    std::string required(const std::string& key) {
        auto value = take(key);
        require(!value.empty(), "configuration", "missing option: " + key);
        return value;
    }
    void finish() const {
        require(values_.empty(), "configuration", "unknown option: " +
                    (values_.empty() ? "" : values_.begin()->first));
    }
    std::string command;
private:
    std::map<std::string, std::string> values_;
};

uint64_t number(const std::string& text, uint64_t minimum, uint64_t maximum) {
    uint64_t value = 0;
    auto [end, code] = std::from_chars(text.data(), text.data() + text.size(), value);
    require(code == std::errc{} && end == text.data() + text.size() &&
                value >= minimum && value <= maximum, "configuration", "invalid numeric option");
    return value;
}

void on_signal(int) { cancelled = 1; }
} // namespace

int main(int argc, char** argv) {
    auto command_started = std::chrono::steady_clock::now();
    umask(0077);
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::string job;
    std::unique_ptr<Engine> engine;
    try {
        if (argc == 2 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "help")) {
            std::cout << help;
            return 0;
        }
        if (argc == 2 && std::string(argv[1]) == "--version") {
            std::cout << "backupctl 0.1.0\n";
            return 0;
        }
        Arguments args(argc, argv);
        fs::path state(args.required("--state"));
        if (args.command == "init") {
            auto local = args.take("--local-store");
            Json config;
            if (!local.empty()) {
                config = {{"backend", "local"},
                          {"local_store", fs::weakly_canonical(fs::absolute(local)).string()}};
            } else {
                auto prefix = args.required("--prefix");
                validate_path(prefix);
                config = {{"backend", "s3"}, {"bucket", args.required("--bucket")},
                          {"prefix", prefix}, {"region", args.required("--region")}};
            }
            args.finish();
            std::cout << initialize(state, config).dump() << '\n';
            return 0;
        }
        std::string source, snapshot, target;
        BackupOptions options;
        if (args.command == "backup") {
            source = args.required("--source");
            options.workers = static_cast<size_t>(number(args.take("--workers", "4"), 1, 64));
            options.memory_budget = number(args.take("--memory-mib", "128"), 32, 4096) * 1024 * 1024;
        } else if (args.command == "resume" || args.command == "status") {
            job = args.required("--job");
        } else if (args.command == "verify" || args.command == "restore") {
            snapshot = args.required("--snapshot");
            if (args.command == "restore") {
                target = args.required("--target");
            }
        } else {
            require(args.command == "list", "configuration", "unknown command; see --help");
        }
        args.finish();
        if (args.command == "status") {
            std::cout << job_status(state, job).dump() << '\n';
            return 0;
        }
        Repository repository(state);
        engine = std::make_unique<Engine>(repository);
        Json result;
        if (args.command == "backup") {
            result = engine->backup(source, options);
        } else if (args.command == "resume") {
            result = engine->resume(job);
        } else if (args.command == "status") {
            result = engine->status(job);
        } else if (args.command == "verify") {
            result = engine->verify(snapshot);
        } else if (args.command == "restore") {
            result = engine->restore(snapshot, target);
        } else {
            repository.store().list("snapshots", [&](const std::string& key) {
                require(key.starts_with("snapshots/") && key.ends_with(".json"),
                        "integrity", "unexpected snapshot key");
                auto id = key.substr(10, key.size() - 15);
                auto value = repository.commit(id);
                value["ok"] = true;
                std::cout << value.dump() << '\n';
            });
            return 0;
        }
        result["command_elapsed_seconds"] = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - command_started).count();
        std::cout << result.dump() << '\n';
        return 0;
    } catch (...) {
        auto error = describe_error(std::current_exception());
        const auto& category = error.category();
        if (engine && !engine->current_job().empty()) {
            job = engine->current_job();
        }
        Json result{{"ok", false}, {"error", category}, {"message", error.what()}};
        if (!job.empty()) {
            result["job_id"] = job;
        }
        std::cerr << result.dump(-1, ' ', false, Json::error_handler_t::replace) << '\n';
        return category == "configuration" ? 2 : 1;
    }
}
