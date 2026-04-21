#include "esp_http_client_transport.hpp"

#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace esp_opentelemetry {
namespace {

constexpr const char* TAG = "esp_opentelemetry_http";

namespace http_client = opentelemetry::ext::http::client;
using opentelemetry::nostd::string_view;

class EspRequest : public http_client::Request {
public:
  void SetMethod(http_client::Method method) noexcept override {
    method_ = method;
  }
  void SetUri(string_view uri) noexcept override {
    uri_.assign(uri.data(), uri.size());
  }
  void SetSslOptions(const http_client::HttpSslOptions& opts) noexcept override {
    ssl_ = opts;
  }
  void SetBody(http_client::Body& body) noexcept override { body_ = body; }
  void AddHeader(string_view name, string_view value) noexcept override {
    headers_.emplace(std::string(name.data(), name.size()),
                     std::string(value.data(), value.size()));
  }
  void ReplaceHeader(string_view name, string_view value) noexcept override {
    std::string key(name.data(), name.size());
    headers_.erase(key);
    headers_.emplace(std::move(key),
                     std::string(value.data(), value.size()));
  }
  void SetTimeoutMs(std::chrono::milliseconds ms) noexcept override {
    timeout_ms_ = ms;
  }
  void SetCompression(
      const http_client::Compression& compression) noexcept override {
    compression_ = compression;
  }
  void EnableLogging(bool enabled) noexcept override { log_enabled_ = enabled; }
  void SetRetryPolicy(const http_client::RetryPolicy& policy) noexcept override {
    retry_ = policy;
  }

  http_client::Method method_{http_client::Method::Post};
  std::string uri_;
  http_client::HttpSslOptions ssl_;
  http_client::Body body_;
  http_client::Headers headers_;
  std::chrono::milliseconds timeout_ms_{std::chrono::seconds(30)};
  http_client::Compression compression_{http_client::Compression::kNone};
  bool log_enabled_{false};
  http_client::RetryPolicy retry_{};
};

class EspResponse : public http_client::Response {
public:
  const http_client::Body& GetBody() const noexcept override { return body_; }
  bool ForEachHeader(opentelemetry::nostd::function_ref<
                     bool(string_view, string_view)> cb) const noexcept override {
    for (const auto& kv : headers_) {
      if (!cb(kv.first, kv.second)) return false;
    }
    return true;
  }
  bool ForEachHeader(const string_view& key,
                     opentelemetry::nostd::function_ref<
                         bool(string_view, string_view)> cb) const noexcept override {
    for (const auto& kv : headers_) {
      if (kv.first.size() == key.size() &&
          std::equal(kv.first.begin(), kv.first.end(), key.data())) {
        if (!cb(kv.first, kv.second)) return false;
      }
    }
    return true;
  }
  http_client::StatusCode GetStatusCode() const noexcept override {
    return status_;
  }

  http_client::Body body_;
  http_client::Headers headers_;
  http_client::StatusCode status_{0};
};

class EspSession : public http_client::Session,
                   public std::enable_shared_from_this<EspSession> {
public:
  explicit EspSession(std::string url) : url_(std::move(url)) {}

  std::shared_ptr<http_client::Request> CreateRequest() noexcept override {
    request_ = std::make_shared<EspRequest>();
    return request_;
  }

  void SendRequest(
      std::shared_ptr<http_client::EventHandler> handler) noexcept override {
    if (!request_) {
      if (handler) handler->OnEvent(http_client::SessionState::CreateFailed,
                                    "no request");
      return;
    }
    active_.store(true, std::memory_order_release);
    auto self   = shared_from_this();
    auto req    = request_;
    auto handle = handler;
    // OtlpHttpExporter calls SendRequest from BatchSpanProcessor's worker
    // thread but still expects asynchronous semantics - spawn a task so the
    // caller can continue while esp_http_client blocks on the socket.
    std::thread([self, req, handle]() {
      self->PerformSync(*req, handle.get());
      self->active_.store(false, std::memory_order_release);
    }).detach();
  }

  bool IsSessionActive() noexcept override {
    return active_.load(std::memory_order_acquire);
  }

  bool CancelSession() noexcept override {
    cancelled_.store(true, std::memory_order_release);
    return true;
  }

  bool FinishSession() noexcept override {
    cancelled_.store(true, std::memory_order_release);
    return true;
  }

private:
  void PerformSync(EspRequest& req, http_client::EventHandler* handler) {
    std::string full_url = BuildUrl(req.uri_);

    esp_http_client_config_t cfg = {};
    cfg.url                      = full_url.c_str();
    cfg.method                   = ToEspMethod(req.method_);
    cfg.timeout_ms               = static_cast<int>(req.timeout_ms_.count());
    cfg.disable_auto_redirect    = false;
    if (req.ssl_.ssl_insecure_skip_verify) {
      cfg.skip_cert_common_name_check = true;
    }
    if (!req.ssl_.ssl_ca_cert_string.empty()) {
      cfg.cert_pem = req.ssl_.ssl_ca_cert_string.c_str();
    }

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
      if (handler) {
        handler->OnEvent(http_client::SessionState::CreateFailed,
                         "esp_http_client_init");
      }
      return;
    }

    for (const auto& kv : req.headers_) {
      esp_http_client_set_header(client, kv.first.c_str(), kv.second.c_str());
    }
    if (!req.body_.empty()) {
      esp_http_client_set_post_field(
          client, reinterpret_cast<const char*>(req.body_.data()),
          static_cast<int>(req.body_.size()));
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
      if (handler) {
        handler->OnEvent(http_client::SessionState::NetworkError,
                         esp_err_to_name(err));
      }
      esp_http_client_cleanup(client);
      return;
    }

    auto response    = std::make_unique<EspResponse>();
    response->status_ =
        static_cast<http_client::StatusCode>(esp_http_client_get_status_code(client));
    int content_length = esp_http_client_get_content_length(client);
    if (content_length > 0) {
      response->body_.resize(static_cast<size_t>(content_length));
      int read = esp_http_client_read(
          client, reinterpret_cast<char*>(response->body_.data()),
          content_length);
      if (read < 0) read = 0;
      response->body_.resize(static_cast<size_t>(read));
    }

    if (handler) {
      handler->OnResponse(*response);
      handler->OnEvent(http_client::SessionState::Response, {});
    }
    esp_http_client_cleanup(client);
  }

  std::string BuildUrl(const std::string& uri) const {
    if (uri.empty()) return url_;
    if (uri.front() == '/') return url_ + uri;
    return uri;
  }

  static esp_http_client_method_t ToEspMethod(http_client::Method m) {
    switch (m) {
      case http_client::Method::Get:     return HTTP_METHOD_GET;
      case http_client::Method::Post:    return HTTP_METHOD_POST;
      case http_client::Method::Put:     return HTTP_METHOD_PUT;
      case http_client::Method::Delete:  return HTTP_METHOD_DELETE;
      case http_client::Method::Head:    return HTTP_METHOD_HEAD;
      case http_client::Method::Options: return HTTP_METHOD_OPTIONS;
      case http_client::Method::Patch:   return HTTP_METHOD_PATCH;
    }
    return HTTP_METHOD_POST;
  }

  std::string url_;
  std::shared_ptr<EspRequest> request_;
  std::atomic<bool> active_{false};
  std::atomic<bool> cancelled_{false};
};

class EspHttpClient : public http_client::HttpClient {
public:
  std::shared_ptr<http_client::Session> CreateSession(
      string_view url) noexcept override {
    auto session =
        std::make_shared<EspSession>(std::string(url.data(), url.size()));
    std::lock_guard<std::mutex> lock(mu_);
    sessions_.insert(session);
    return session;
  }

  bool CancelAllSessions() noexcept override {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& s : sessions_) s->CancelSession();
    return true;
  }

  bool FinishAllSessions() noexcept override {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& s : sessions_) s->FinishSession();
    sessions_.clear();
    return true;
  }

  void SetMaxSessionsPerConnection(std::size_t) noexcept override {}

private:
  std::mutex mu_;
  std::unordered_set<std::shared_ptr<EspSession>> sessions_;
};

}  // namespace

std::shared_ptr<http_client::HttpClient> MakeEspHttpClient() {
  return std::make_shared<EspHttpClient>();
}

}  // namespace esp_opentelemetry

// --- Replace the default opentelemetry-cpp HTTP transport factory -----------
//
// opentelemetry_http_client_curl ships three free functions that the
// OTLP exporter calls via HttpClientFactory::Create / CreateSync. The
// default implementation uses libcurl. We build the same target from
// this source file instead (see CMakeLists.txt), redirecting the factory
// to our esp_http_client-backed client. This avoids cross-compiling libcurl
// for Xtensa while keeping the opentelemetry-cpp link graph intact.

#include "opentelemetry/ext/http/client/http_client_factory.h"
#include "opentelemetry/sdk/common/thread_instrumentation.h"

namespace opentelemetry {
namespace ext {
namespace http {
namespace client {

std::shared_ptr<HttpClient> HttpClientFactory::Create() {
  return esp_opentelemetry::MakeEspHttpClient();
}

std::shared_ptr<HttpClient> HttpClientFactory::Create(
    const std::shared_ptr<sdk::common::ThreadInstrumentation>& /*unused*/) {
  return esp_opentelemetry::MakeEspHttpClient();
}

std::shared_ptr<HttpClientSync> HttpClientFactory::CreateSync() {
  // HttpClientSync is unused by the OTLP exporter; return nullptr.
  return nullptr;
}

}  // namespace client
}  // namespace http
}  // namespace ext
}  // namespace opentelemetry
