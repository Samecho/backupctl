#include "backup/s3_store.h"
#include "backup/engine.h"
#include "backup/error.h"
#include "backup/fs.h"

#include <aws/core/Aws.h>
#include <aws/core/client/DefaultRetryStrategy.h>
#include <aws/core/utils/memory/stl/AWSStreamFwd.h>
#include <aws/core/utils/threading/Executor.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/model/GetObjectRequest.h>
#include <aws/s3/model/HeadObjectRequest.h>
#include <aws/s3/model/ListObjectsV2Request.h>
#include <aws/s3/model/PutObjectRequest.h>

#include <fcntl.h>
#include <fstream>
#include <cstdlib>
#include <sstream>
#include <streambuf>
#include <unistd.h>

namespace backup {
namespace fs = std::filesystem;
namespace {
struct AwsRuntime {
    Aws::SDKOptions options;
    AwsRuntime() { Aws::InitAPI(options); }
    ~AwsRuntime() { Aws::ShutdownAPI(options); }
};

std::shared_ptr<AwsRuntime> runtime() {
    static auto instance = std::make_shared<AwsRuntime>();
    return instance;
}

void s3_error(const Aws::S3::S3Error& error) {
    check_cancelled();
    int status = static_cast<int>(error.GetResponseCode());
    std::string category = "configuration";
    std::string details = std::string(error.GetExceptionName().c_str()) + " " + error.GetMessage().c_str();
    if (details.find("checksum") != std::string::npos || details.find("Checksum") != std::string::npos ||
        details.find("BadDigest") != std::string::npos || details.find("InvalidDigest") != std::string::npos) {
        category = "integrity";
    } else if (details.find("NoSuchBucket") != std::string::npos) {
        category = "configuration";
    } else if (status == 403 || status == 401) {
        category = "permission";
    } else if (status == 404) {
        category = "integrity";
    } else if (status <= 0 || status >= 500 || status == 408 || status == 429 || status == 409) {
        category = "network_exhausted";
    }
    // Exception names identify the failure without logging signed URLs or credentials.
    throw Error(category, "S3 request failed: " + std::string(error.GetExceptionName().c_str()) +
                              " (HTTP " + std::to_string(status) + ")");
}

Metadata metadata_from(const Aws::Map<Aws::String, Aws::String>& input) {
    Metadata result;
    for (const auto& [name, value] : input) {
        result.emplace(name.c_str(), value.c_str());
    }
    return result;
}

struct Sink {
    Bytes* bytes = nullptr;
    int fd = -1;
    uint64_t limit = 0;
    uint64_t count = 0;
    bool exceeded = false;
    std::exception_ptr failure;
    std::unique_ptr<Sha256> hash;

    void reset() {
        count = 0;
        exceeded = false;
        failure = nullptr;
        hash = std::make_unique<Sha256>();
        if (bytes) {
            bytes->clear();
        } else if (ftruncate(fd, 0) < 0 || lseek(fd, 0, SEEK_SET) < 0) {
            io_error("reset download file");
        }
    }
};

class LimitBuffer : public std::streambuf {
public:
    explicit LimitBuffer(Sink& sink) : sink_(sink) {}
protected:
    std::streamsize xsputn(const char* data, std::streamsize size) override {
        if (size < 0 || static_cast<uint64_t>(size) > sink_.limit - sink_.count) {
            sink_.exceeded = true;
            return 0;
        }
        try {
            check_cancelled();
            auto bytes = std::span(reinterpret_cast<const unsigned char*>(data),
                                    static_cast<size_t>(size));
            sink_.hash->update(bytes);
            if (sink_.bytes) {
                sink_.bytes->insert(sink_.bytes->end(), bytes.begin(), bytes.end());
            } else {
                write_all(sink_.fd, data, static_cast<size_t>(size));
            }
            sink_.count += static_cast<uint64_t>(size);
            return size;
        } catch (...) {
            sink_.failure = std::current_exception();
            return 0;
        }
    }
    int_type overflow(int_type character) override {
        if (traits_type::eq_int_type(character, traits_type::eof())) {
            return traits_type::not_eof(character);
        }
        char value = traits_type::to_char_type(character);
        return xsputn(&value, 1) == 1 ? character : traits_type::eof();
    }
private:
    Sink& sink_;
};

class DownloadStream : public Aws::IOStream {
public:
    explicit DownloadStream(Sink& sink) : Aws::IOStream(nullptr), buffer_(sink) { rdbuf(&buffer_); }
private:
    LimitBuffer buffer_;
};
} // namespace

struct S3Store::Impl {
    std::shared_ptr<AwsRuntime> sdk = runtime();
    std::unique_ptr<Aws::S3::S3Client> client;
    Aws::String bucket;
    std::string prefix;
    StoreMetrics& metrics;

    Impl(const Json& config, StoreMetrics& counters) : metrics(counters) {
        bucket = string_field(config, "bucket").c_str();
        prefix = string_field(config, "prefix") + "/";
        validate_path(prefix.substr(0, prefix.size() - 1));
        Aws::S3::S3ClientConfiguration options;
        options.region = string_field(config, "region").c_str();
        options.verifySSL = true;
        options.maxConnections = 8;
        options.connectTimeoutMs = 3000;
        options.requestTimeoutMs = 15000;
        options.httpRequestTimeoutMs = 30000;
#ifdef BACKUPCTL_TESTING
        if (const char* endpoint = std::getenv("BACKUPCTL_TEST_S3_ENDPOINT")) {
            require(std::string(endpoint).starts_with("https://127.0.0.1:"),
                    "configuration", "SDK contract tests require a loopback HTTPS endpoint");
            options.endpointOverride = endpoint;
            options.useVirtualAddressing = false;
            const char* certificate = std::getenv("BACKUPCTL_TEST_CA");
            require(certificate != nullptr, "configuration", "test CA is missing");
            options.caFile = certificate;
        }
#endif
        options.retryStrategy = Aws::MakeShared<Aws::Client::DefaultRetryStrategy>("backupctl", 2, 100);
        options.executor = Aws::MakeShared<Aws::Utils::Threading::PooledThreadExecutor>("backupctl", 2);
        client = std::make_unique<Aws::S3::S3Client>(options);
    }

    Aws::String key(const std::string& relative) const {
        validate_path(relative);
        return (prefix + relative).c_str();
    }

    template<class Request>
    void configure(Request& request, const std::string& relative, std::atomic<uint64_t>& attempts) {
        check_cancelled();
        request.SetBucket(bucket);
        request.SetKey(key(relative));
        ++attempts;
        request.SetRequestRetryHandler([&attempts](const Aws::AmazonWebServiceRequest&) { ++attempts; });
        request.SetContinueRequestHandler([](const Aws::Http::HttpRequest*) { return !cancelled; });
    }

    bool upload(const std::string& relative, const std::shared_ptr<Aws::IOStream>& body,
                uint64_t size, const Metadata& metadata, const std::string& hash) {
        Aws::S3::Model::PutObjectRequest request;
        configure(request, relative, metrics.put_attempts);
        request.SetIfNoneMatch("*");
        request.SetContentLength(static_cast<long long>(size));
        request.SetChecksumAlgorithm(Aws::S3::Model::ChecksumAlgorithm::SHA256);
        request.SetChecksumSHA256(checksum_base64(hash).c_str());
        request.SetServerSideEncryption(Aws::S3::Model::ServerSideEncryption::AES256);
        request.SetContentType("application/octet-stream");
        for (const auto& [name, value] : metadata) {
            request.AddMetadata(name.c_str(), value.c_str());
        }
        request.AddMetadata("encoded-sha256", hash.c_str());
        request.SetBody(body);
        auto outcome = client->PutObject(request);
        if (!outcome.IsSuccess()) {
            if (static_cast<int>(outcome.GetError().GetResponseCode()) == 412) {
                return false;
            }
            s3_error(outcome.GetError());
        }
        require(outcome.GetResult().GetChecksumSHA256() == checksum_base64(hash).c_str(),
                "integrity", "S3 did not confirm the encoded upload checksum");
        metrics.uploaded += size;
        ++metrics.put_created;
        return true;
    }

    void download(const std::string& relative, Sink& sink) {
        Aws::S3::Model::GetObjectRequest request;
        configure(request, relative, metrics.get_attempts);
        request.SetChecksumMode(Aws::S3::Model::ChecksumMode::ENABLED);
        request.SetResponseStreamFactory([&sink]() -> Aws::IOStream* {
            sink.reset();
            return Aws::New<DownloadStream>("backupctl", sink);
        });
        auto outcome = client->GetObject(request);
        if (sink.failure) {
            std::rethrow_exception(sink.failure);
        }
        require(!sink.exceeded, "integrity", "S3 object exceeded download limit");
        if (!outcome.IsSuccess()) {
            s3_error(outcome.GetError());
        }
        auto& result = outcome.GetResult();
        auto metadata = metadata_from(result.GetMetadata());
        auto hash = sink.hash->finish();
        auto found = metadata.find("encoded-sha256");
        require(result.GetContentLength() >= 0 &&
                    sink.count == static_cast<uint64_t>(result.GetContentLength()) &&
                    found != metadata.end() && found->second == hash &&
                    result.GetChecksumSHA256() == checksum_base64(hash).c_str(),
                "integrity", "S3 encoded download checksum or length mismatch");
        metrics.downloaded += sink.count;
    }
};

S3Store::S3Store(const Json& config) : impl_(std::make_unique<Impl>(config, metrics)) {}
S3Store::~S3Store() = default;

std::optional<ObjectInfo> S3Store::head(const std::string& key) {
    ++metrics.head;
    Aws::S3::Model::HeadObjectRequest request;
    impl_->configure(request, key, metrics.head_attempts);
    auto outcome = impl_->client->HeadObject(request);
    if (!outcome.IsSuccess()) {
        if (static_cast<int>(outcome.GetError().GetResponseCode()) == 404) {
            return std::nullopt;
        }
        if (static_cast<int>(outcome.GetError().GetResponseCode()) == 403) {
            // A prefix-scoped ListBucket policy can turn a missing-key HEAD into 403.
            // LIST the exact key before deciding whether this is absence or a denial.
            Aws::S3::Model::ListObjectsV2Request lookup;
            lookup.SetBucket(impl_->bucket);
            lookup.SetPrefix(impl_->key(key));
            lookup.SetMaxKeys(1);
            lookup.SetContinueRequestHandler([](const Aws::Http::HttpRequest*) { return !cancelled; });
            lookup.SetRequestRetryHandler([this](const Aws::AmazonWebServiceRequest&) { ++metrics.list_attempts; });
            ++metrics.list;
            ++metrics.list_attempts;
            auto listed = impl_->client->ListObjectsV2(lookup);
            if (!listed.IsSuccess()) {
                s3_error(listed.GetError());
            }
            bool exists = !listed.GetResult().GetContents().empty() &&
                listed.GetResult().GetContents().front().GetKey() == impl_->key(key);
            if (!exists) {
                return std::nullopt;
            }
        }
        s3_error(outcome.GetError());
    }
    auto& result = outcome.GetResult();
    require(result.GetContentLength() >= 0, "integrity", "negative S3 object length");
    return ObjectInfo{static_cast<uint64_t>(result.GetContentLength()), metadata_from(result.GetMetadata())};
}

bool S3Store::put(const std::string& key, std::span<const unsigned char> data,
                  const Metadata& metadata, const std::string& hash) {
    ++metrics.put;
    auto body = Aws::MakeShared<Aws::StringStream>("backupctl");
    body->write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    body->seekg(0);
    return impl_->upload(key, body, data.size(), metadata, hash);
}

bool S3Store::put_file(const std::string& key, const fs::path& file,
                       const Metadata& metadata, const std::string& hash) {
    ++metrics.put;
    auto body = Aws::MakeShared<Aws::FStream>("backupctl", file.c_str(), std::ios::in | std::ios::binary);
    require(body->is_open(), "io", "cannot open S3 upload file");
    return impl_->upload(key, body, fs::file_size(file), metadata, hash);
}

Bytes S3Store::get(const std::string& key, size_t limit) {
    ++metrics.get;
    Bytes bytes;
    bytes.reserve(limit);
    Sink sink;
    sink.bytes = &bytes;
    sink.limit = limit;
    impl_->download(key, sink);
    return bytes;
}

void S3Store::get_file(const std::string& key, const fs::path& destination, uint64_t limit) {
    ++metrics.get;
    Fd output(open(destination.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    Sink sink;
    sink.fd = output.get();
    sink.limit = limit;
    impl_->download(key, sink);
    sync_fd(output.get());
}

void S3Store::list(const std::string& prefix, const std::function<void(const std::string&)>& visit) {
    Aws::S3::Model::ListObjectsV2Request request;
    request.SetBucket(impl_->bucket);
    request.SetPrefix((impl_->prefix + prefix + "/").c_str());
    request.SetMaxKeys(1000);
    request.SetContinueRequestHandler([](const Aws::Http::HttpRequest*) { return !cancelled; });
    request.SetRequestRetryHandler([this](const Aws::AmazonWebServiceRequest&) { ++metrics.list_attempts; });
    do {
        check_cancelled();
        ++metrics.list;
        ++metrics.list_attempts;
        auto outcome = impl_->client->ListObjectsV2(request);
        if (!outcome.IsSuccess()) {
            s3_error(outcome.GetError());
        }
        const auto& result = outcome.GetResult();
        for (const auto& entry : result.GetContents()) {
            std::string key(entry.GetKey().c_str());
            require(key.starts_with(impl_->prefix), "integrity", "S3 key outside repository prefix");
            visit(key.substr(impl_->prefix.size()));
        }
        if (!result.GetIsTruncated()) {
            break;
        }
        require(!result.GetNextContinuationToken().empty(), "integrity", "missing S3 pagination token");
        require(result.GetNextContinuationToken() != request.GetContinuationToken(),
                "integrity", "S3 pagination did not advance");
        request.SetContinuationToken(result.GetNextContinuationToken());
    } while (true);
}

} // namespace backup
