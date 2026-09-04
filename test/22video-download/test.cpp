#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509v3.h>
#include <venice/venice.hpp>

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

struct X509Deleter {
  void operator()(X509 *value) const noexcept { X509_free(value); }
};

struct PrivateKeyDeleter {
  void operator()(EVP_PKEY *value) const noexcept { EVP_PKEY_free(value); }
};

struct PrivateKeyContextDeleter {
  void operator()(EVP_PKEY_CTX *value) const noexcept {
    EVP_PKEY_CTX_free(value);
  }
};

struct BioDeleter {
  void operator()(BIO *value) const noexcept { BIO_free(value); }
};

struct ExtensionDeleter {
  void operator()(X509_EXTENSION *value) const noexcept {
    X509_EXTENSION_free(value);
  }
};

class TestTlsIdentity final {
public:
  TestTlsIdentity() : m_private_key{make_private_key()} {
    m_certificate = make_certificate(m_private_key.get());
    m_certificate_pem = encode_certificate(m_certificate.get());
  }

  [[nodiscard]] auto certificate() const noexcept -> X509 * {
    return m_certificate.get();
  }
  [[nodiscard]] auto private_key() const noexcept -> EVP_PKEY * {
    return m_private_key.get();
  }
  [[nodiscard]] auto certificate_pem() const noexcept -> const std::string & {
    return m_certificate_pem;
  }

private:
  using Certificate = std::unique_ptr<X509, X509Deleter>;
  using PrivateKey = std::unique_ptr<EVP_PKEY, PrivateKeyDeleter>;

  [[nodiscard]] static auto make_private_key() -> PrivateKey {
    std::unique_ptr<EVP_PKEY_CTX, PrivateKeyContextDeleter> context{
        EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr)};
    if (!context || EVP_PKEY_keygen_init(context.get()) <= 0 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), 2048) <= 0)
      throw std::runtime_error{"could not initialize video download test key"};
    EVP_PKEY *generated = nullptr;
    const auto status = EVP_PKEY_keygen(context.get(), &generated);
    PrivateKey result{generated};
    if (status <= 0 || !result)
      throw std::runtime_error{"could not generate video download test key"};
    return result;
  }

  static void add_extension(X509 *certificate, int identifier,
                            const char *value) {
    X509V3_CTX context{};
    X509V3_set_ctx(&context, certificate, certificate, nullptr, nullptr, 0);
    std::string mutable_value{value};
    std::unique_ptr<X509_EXTENSION, ExtensionDeleter> extension{
        X509V3_EXT_conf_nid(nullptr, &context, identifier,
                            mutable_value.data())};
    if (!extension || X509_add_ext(certificate, extension.get(), -1) != 1)
      throw std::runtime_error{
          "could not extend video download test certificate"};
  }

  [[nodiscard]] static auto make_certificate(EVP_PKEY *private_key)
      -> Certificate {
    Certificate certificate{X509_new()};
    if (!certificate || X509_set_version(certificate.get(), 2) != 1 ||
        ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1) != 1 ||
        X509_gmtime_adj(X509_get_notBefore(certificate.get()), -3600) ==
            nullptr ||
        X509_gmtime_adj(X509_get_notAfter(certificate.get()),
                        10L * 365L * 24L * 60L * 60L) == nullptr ||
        X509_set_pubkey(certificate.get(), private_key) != 1)
      throw std::runtime_error{
          "could not create video download test certificate"};

    auto *name = X509_get_subject_name(certificate.get());
    constexpr std::string_view common_name{"media.example.test"};
    if (name == nullptr ||
        X509_NAME_add_entry_by_txt(
            name, "CN", MBSTRING_ASC,
            reinterpret_cast<const unsigned char *>(common_name.data()),
            static_cast<int>(common_name.size()), -1, 0) != 1 ||
        X509_set_issuer_name(certificate.get(), name) != 1)
      throw std::runtime_error{
          "could not name video download test certificate"};

    add_extension(certificate.get(), NID_basic_constraints, "critical,CA:TRUE");
    add_extension(certificate.get(), NID_key_usage,
                  "critical,digitalSignature,keyEncipherment,keyCertSign");
    add_extension(certificate.get(), NID_subject_alt_name,
                  "DNS:media.example.test");
    if (X509_sign(certificate.get(), private_key, EVP_sha256()) <= 0)
      throw std::runtime_error{
          "could not sign video download test certificate"};
    return certificate;
  }

  [[nodiscard]] static auto encode_certificate(X509 *certificate)
      -> std::string {
    std::unique_ptr<BIO, BioDeleter> output{BIO_new(BIO_s_mem())};
    if (!output || PEM_write_bio_X509(output.get(), certificate) != 1)
      throw std::runtime_error{
          "could not encode video download test certificate"};
    char *data = nullptr;
    const auto size = BIO_get_mem_data(output.get(), &data);
    if (data == nullptr || size <= 0)
      throw std::runtime_error{"video download test certificate was empty"};
    return {data, static_cast<std::size_t>(size)};
  }

  PrivateKey m_private_key;
  Certificate m_certificate;
  std::string m_certificate_pem;
};

[[nodiscard]] auto test_tls_identity() -> const TestTlsIdentity & {
  static const TestTlsIdentity identity;
  return identity;
}

class TlsDownloadFixture final {
public:
  TlsDownloadFixture() {
    const auto &identity = test_tls_identity();
    m_server = std::make_unique<httplib::SSLServer>(identity.certificate(),
                                                    identity.private_key());
    if (!m_server->is_valid())
      throw std::runtime_error{
          "could not initialize video download TLS fixture"};

    m_server->Get("/exact", [this](const httplib::Request &req,
                                   httplib::Response &res) {
      const auto expected_host = "media.example.test:" + std::to_string(m_port);
      m_safe_request.store(req.get_header_value("Host") == expected_host &&
                               req.get_header_value("Accept") == "video/mp4" &&
                               !req.has_header("Authorization") &&
                               !req.has_header("SIGN-IN-WITH-X") &&
                               !req.has_header("PAYMENT-SIGNATURE") &&
                               !req.has_header("Idempotency-Key"),
                           std::memory_order_release);
      const std::string bytes{"a\0b\xff", 4};
      res.set_content(bytes.data(), bytes.size(), "video/mp4; fixture=true");
    });
    m_server->Get("/unknown-encoding",
                  [](const httplib::Request &, httplib::Response &res) {
                    res.set_header("Content-Encoding", "foreign-coding");
                    res.set_content("mp4", "video/mp4");
                  });
    m_server->Get("/chunked-overflow", [](const httplib::Request &,
                                          httplib::Response &res) {
      auto sent = std::make_shared<int>(0);
      res.set_chunked_content_provider(
          "video/mp4", [sent](std::size_t, httplib::DataSink &sink) {
            constexpr std::array<std::string_view, 3> chunks{"12", "34", "5"};
            if (*sent == static_cast<int>(chunks.size())) {
              sink.done();
              return true;
            }
            const auto chunk = chunks[static_cast<std::size_t>((*sent)++)];
            return sink.write(chunk.data(), chunk.size());
          });
    });
#ifdef CPPHTTPLIB_ZLIB_SUPPORT
    httplib::detail::gzip_compressor compressor;
    std::string encoded;
    if (!compressor.compress("12345", 5, true,
                             [&encoded](const char *data, std::size_t size) {
                               encoded.append(data, size);
                               return true;
                             }))
      throw std::runtime_error{"could not encode video download TLS fixture"};
    m_server->Get("/gzip-overflow",
                  [encoded = std::move(encoded)](const httplib::Request &,
                                                 httplib::Response &res) {
                    res.set_header("Content-Encoding", "gzip");
                    res.set_content(encoded, "video/mp4");
                  });
#endif
    m_server->Get(
        "/slow", [](const httplib::Request &, httplib::Response &res) {
          auto sent = std::make_shared<int>(0);
          res.set_chunked_content_provider(
              "video/mp4", [sent](std::size_t, httplib::DataSink &sink) {
                if (*sent == 8) {
                  sink.done();
                  return true;
                }
                std::this_thread::sleep_for(60ms);
                ++*sent;
                return sink.write("x", 1);
              });
        });

    m_port = m_server->bind_to_any_port("127.0.0.1");
    if (m_port <= 0)
      throw std::runtime_error{"could not bind video download TLS fixture"};
    m_thread = std::thread{[this] { m_server->listen_after_bind(); }};
    m_server->wait_until_ready();
  }

  ~TlsDownloadFixture() {
    if (m_server)
      m_server->stop();
    if (m_thread.joinable())
      m_thread.join();
  }

  TlsDownloadFixture(const TlsDownloadFixture &) = delete;
  auto operator=(const TlsDownloadFixture &) -> TlsDownloadFixture & = delete;

  [[nodiscard]] auto url(std::string host, std::string target) const
      -> venice::detail::VideoDownloadUrl {
    return {.host = std::move(host),
            .port = static_cast<std::uint16_t>(m_port),
            .target = std::move(target)};
  }

  [[nodiscard]] auto safe_request_seen() const noexcept -> bool {
    return m_safe_request.load(std::memory_order_acquire);
  }

private:
  std::unique_ptr<httplib::SSLServer> m_server;
  int m_port{};
  std::thread m_thread;
  std::atomic<bool> m_safe_request{};
};

class ScopedEnvironment final {
public:
  ScopedEnvironment(std::string name, std::string value)
      : m_name{std::move(name)} {
    if (const auto *existing = std::getenv(m_name.c_str()); existing != nullptr)
      m_previous = existing;
#ifdef _WIN32
    static_cast<void>(_putenv_s(m_name.c_str(), value.c_str()));
#else
    static_cast<void>(setenv(m_name.c_str(), value.c_str(), 1));
#endif
  }

  ~ScopedEnvironment() {
#ifdef _WIN32
    static_cast<void>(
        _putenv_s(m_name.c_str(), m_previous ? m_previous->c_str() : ""));
#else
    if (m_previous)
      static_cast<void>(setenv(m_name.c_str(), m_previous->c_str(), 1));
    else
      static_cast<void>(unsetenv(m_name.c_str()));
#endif
  }

  ScopedEnvironment(const ScopedEnvironment &) = delete;
  auto operator=(const ScopedEnvironment &) -> ScopedEnvironment & = delete;

private:
  std::string m_name;
  std::optional<std::string> m_previous;
};

struct ManualNow {
  Clock::time_point *value{};
  [[nodiscard]] auto operator()() const noexcept -> Clock::time_point {
    return *value;
  }
};

struct ScriptedResolver {
  std::map<std::string, std::vector<std::string>> answers{};
  std::vector<std::string> calls{};
  Clock::time_point *now{};
  std::chrono::milliseconds advance{};
  venice::CancelToken *cancel_during_call{};

  auto resolve(std::string_view host, Clock::time_point, venice::CancelToken *)
      -> std::expected<std::vector<std::string>, venice::Error> {
    calls.emplace_back(host);
    if (now != nullptr)
      *now += advance;
    if (cancel_during_call != nullptr)
      cancel_during_call->cancel();
    if (const auto found = answers.find(std::string{host});
        found != answers.end())
      return found->second;
    return std::vector<std::string>{"93.184.216.34"};
  }
};

struct DeterministicCaresDriver final : venice::detail::CaresResolverDriver {
  bool start_succeeds{true};
  bool process_succeeds{true};
  bool complete_after_process{};
  bool allocation_failure{};
  int result_status{ARES_SUCCESS};
  std::vector<std::string> result_addresses{};
  venice::CancelToken *cancel_during_process{};
  bool start_called{};
  bool cancel_called{};
  std::size_t process_calls{};
  std::vector<venice::detail::CaresSocketState::Socket> socket_states{};

  [[nodiscard]] auto start(std::string_view) -> bool override {
    start_called = true;
    return start_succeeds;
  }
  [[nodiscard]] auto complete() const noexcept -> bool override {
    return m_complete;
  }
  [[nodiscard]] auto allocation_failed() const noexcept -> bool override {
    return allocation_failure;
  }
  [[nodiscard]] auto sockets() const noexcept -> const
      std::vector<venice::detail::CaresSocketState::Socket> & override {
    return socket_states;
  }
  [[nodiscard]] auto process(const ares_fd_events_t *, std::size_t)
      -> bool override {
    ++process_calls;
    if (cancel_during_process != nullptr)
      cancel_during_process->cancel();
    if (complete_after_process)
      m_complete = true;
    return process_succeeds;
  }
  void cancel() noexcept override { cancel_called = true; }
  [[nodiscard]] auto status() const noexcept -> int override {
    return result_status;
  }
  [[nodiscard]] auto take_addresses() -> std::vector<std::string> override {
    return std::move(result_addresses);
  }

private:
  bool m_complete{};
};

struct FetchCall {
  venice::detail::VideoDownloadUrl url{};
  std::string address{};
  std::size_t maximum_bytes{};
  venice::RequestOptions options{};
};

struct ScriptedFetcher {
  // Deliberately exposes only fetch(). Successful and failing template tests
  // therefore prove the download path has no cleanup capability to invoke.
  std::vector<venice::detail::VideoDownloadResponse> responses{};
  std::vector<FetchCall> calls{};
  Clock::time_point *now{};
  std::chrono::milliseconds advance{};
  venice::CancelToken *cancel_during_call{};

  auto fetch(const venice::detail::VideoDownloadUrl &url,
             const std::string &address, std::size_t maximum_bytes,
             Clock::time_point, const venice::RequestOptions &options)
      -> std::expected<venice::detail::VideoDownloadResponse, venice::Error> {
    calls.push_back({url, address, maximum_bytes, options});
    if (now != nullptr)
      *now += advance;
    if (cancel_during_call != nullptr)
      cancel_during_call->cancel();
    if (responses.empty())
      return std::unexpected{
          venice::Error{venice::ErrorKind::Network, 0, "script exhausted", {}}};
    auto response = std::move(responses.front());
    responses.erase(responses.begin());
    return response;
  }
};

[[nodiscard]] auto
request(std::string url = "https://media.example.test/video.mp4")
    -> venice::VideoDownloadRequest {
  return {.url = std::move(url), .maximum_bytes = 8, .maximum_elapsed = 1s};
}

[[nodiscard]] auto ok(std::string bytes = "mp4")
    -> venice::detail::VideoDownloadResponse {
  return {200, "video/mp4", std::move(bytes), std::nullopt};
}

template <typename Result>
auto require_error(const Result &result, venice::ErrorKind kind,
                   std::string_view message) -> void {
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().kind == kind);
  CHECK(result.error().message == message);
  CHECK(result.error().body.empty());
  CHECK(result.error().metadata.headers.empty());
}

} // namespace

TEST_CASE("process-wide video download runtime shares owners and drains use",
          "[video-download][failure][lifecycle]") {
  // Arbitrary application-thread ordering is a caller precondition that this
  // header-only library cannot inspect. Pin the enforceable half here: shared
  // ownership, failed initialization, and draining every acquired resolver use.
  SECTION("initialization failure leaves acquisition closed") {
    venice::detail::CaresRuntimeCoordinator coordinator;
    int cleanups = 0;
    CHECK_FALSE(coordinator.initialize([] { return ARES_ENOMEM; }));
    CHECK_FALSE(coordinator.acquire());
    coordinator.release([&cleanups] { ++cleanups; });
    CHECK(cleanups == 0);
  }

  SECTION(
      "one initialization serves multiple owners and cleanup waits for use") {
    venice::detail::CaresRuntimeCoordinator coordinator;
    int initializations = 0;
    int cleanups = 0;
    const auto initialize = [&initializations] {
      ++initializations;
      return ARES_SUCCESS;
    };
    REQUIRE(coordinator.initialize(initialize));
    REQUIRE(coordinator.initialize(initialize));
    CHECK(initializations == 1);
    REQUIRE(coordinator.acquire());
    std::optional<venice::detail::CaresRuntimeUse> use{std::in_place,
                                                       coordinator};

    coordinator.release([&cleanups] { ++cleanups; });
    CHECK(cleanups == 0);
    std::jthread finish_use{[active = std::move(*use)]() mutable {
      (void)active;
      std::this_thread::sleep_for(15ms);
    }};
    use.reset();
    const auto cleanup_started = Clock::now();
    coordinator.release([&cleanups] { ++cleanups; });
    CHECK(Clock::now() - cleanup_started >= 10ms);
    finish_use.join();
    CHECK(cleanups == 1);
    CHECK_FALSE(coordinator.acquire());

    coordinator.release([&cleanups] { ++cleanups; });
    CHECK(cleanups == 1);
  }
}

TEST_CASE("production DNS adapter requires a live process-wide runtime owner",
          "[video-download][failure][lifecycle][dns]") {
  DeterministicCaresDriver driver;
  driver.complete_after_process = true;
  driver.result_addresses = {"93.184.216.34"};
  venice::detail::CaresVideoDownloadResolver resolver{driver};
  require_error(
      resolver.resolve("media.example.test", Clock::now() + 100ms, nullptr),
      venice::ErrorKind::InvalidArg,
      "video download runtime is not initialized");
  CHECK_FALSE(driver.start_called);

  {
    auto first = venice::VideoDownloadRuntime::initialize();
    REQUIRE(first.has_value());
    {
      auto second = venice::VideoDownloadRuntime::initialize();
      REQUIRE(second.has_value());
    }
    const auto resolved =
        resolver.resolve("media.example.test", Clock::now() + 100ms, nullptr);
    REQUIRE(resolved.has_value());
    REQUIRE(*resolved == std::vector<std::string>{"93.184.216.34"});
  }

  DeterministicCaresDriver after_cleanup;
  after_cleanup.complete_after_process = true;
  after_cleanup.result_addresses = {"93.184.216.34"};
  venice::detail::CaresVideoDownloadResolver stopped{after_cleanup};
  require_error(
      stopped.resolve("media.example.test", Clock::now() + 100ms, nullptr),
      venice::ErrorKind::InvalidArg,
      "video download runtime is not initialized");
  CHECK_FALSE(after_cleanup.start_called);
}

TEST_CASE("production DNS adapter handles bounded resolver outcomes",
          "[video-download][failure][dns]") {
  auto runtime = venice::VideoDownloadRuntime::initialize();
  REQUIRE(runtime.has_value());

  SECTION("no-answer status maps to a bounded DNS failure") {
    DeterministicCaresDriver driver;
    driver.complete_after_process = true;
    driver.result_status = ARES_ENODATA;
    venice::detail::CaresVideoDownloadResolver resolver{driver};
    require_error(
        resolver.resolve("empty.example.test", Clock::now() + 100ms, nullptr),
        venice::ErrorKind::Network, "video download DNS resolution failed");
  }

  SECTION("mixed-family answers are preserved and fail closed downstream") {
    DeterministicCaresDriver driver;
    driver.complete_after_process = true;
    driver.result_addresses = {"fc00::1", "93.184.216.34", "93.184.216.34"};
    venice::detail::CaresVideoDownloadResolver resolver{driver};
    const auto resolved =
        resolver.resolve("mixed.example.test", Clock::now() + 100ms, nullptr);
    REQUIRE(resolved.has_value());
    REQUIRE(*resolved == std::vector<std::string>{"93.184.216.34", "fc00::1"});
    require_error(venice::detail::validate_video_download_addresses(*resolved),
                  venice::ErrorKind::InvalidArg,
                  "video download destination is not public");
  }

  SECTION("deadline cancels an incomplete adapter") {
    DeterministicCaresDriver driver;
    venice::detail::CaresVideoDownloadResolver resolver{driver};
    const auto started = Clock::now();
    require_error(
        resolver.resolve("slow.example.test", started + 15ms, nullptr),
        venice::ErrorKind::Network, "video download deadline exceeded");
    CHECK(driver.process_calls >= 1);
    CHECK(driver.cancel_called);
    CHECK(Clock::now() - started < 250ms);
  }

  SECTION("cancellation interrupts an incomplete adapter") {
    venice::CancelToken cancellation;
    DeterministicCaresDriver driver;
    driver.cancel_during_process = &cancellation;
    venice::detail::CaresVideoDownloadResolver resolver{driver};
    require_error(
        resolver.resolve("slow.example.test", Clock::now() + 1s, &cancellation),
        venice::ErrorKind::Cancelled, "video download cancelled by caller");
    CHECK(driver.process_calls == 1);
    CHECK(driver.cancel_called);
  }

  SECTION("channel initialization and processing failures are bounded") {
    DeterministicCaresDriver start_failure;
    start_failure.start_succeeds = false;
    venice::detail::CaresVideoDownloadResolver failed_start{start_failure};
    require_error(
        failed_start.resolve("bad.example.test", Clock::now() + 100ms, nullptr),
        venice::ErrorKind::Network, "video download DNS initialization failed");

    DeterministicCaresDriver process_failure;
    process_failure.process_succeeds = false;
    venice::detail::CaresVideoDownloadResolver failed_process{process_failure};
    require_error(failed_process.resolve("bad.example.test",
                                         Clock::now() + 100ms, nullptr),
                  venice::ErrorKind::Network,
                  "video download DNS resolution failed");
    CHECK(process_failure.cancel_called);
  }
}

TEST_CASE("video download rejects unbounded requests before URL or DNS",
          "[video-download][failure][bounds]") {
  Clock::time_point now{};
  ScriptedResolver resolver;
  ScriptedFetcher fetcher;

  auto invalid = request();
  invalid.maximum_bytes = 0;
  require_error(venice::detail::download_video_with(invalid, {}, resolver,
                                                    fetcher, ManualNow{&now}),
                venice::ErrorKind::InvalidArg,
                "video download byte limit must be nonzero");
  invalid = request();
  invalid.maximum_elapsed = 0ms;
  require_error(venice::detail::download_video_with(invalid, {}, resolver,
                                                    fetcher, ManualNow{&now}),
                venice::ErrorKind::InvalidArg,
                "video download deadline must be nonzero");
  invalid.maximum_elapsed = -1ms;
  require_error(venice::detail::download_video_with(invalid, {}, resolver,
                                                    fetcher, ManualNow{&now}),
                venice::ErrorKind::InvalidArg,
                "video download deadline must be nonzero");
  invalid = request();
  for (const auto &options : {
           venice::RequestOptions{.connect_timeout = 0ms},
           venice::RequestOptions{.read_timeout = -1ms},
           venice::RequestOptions{.write_timeout = 0ms},
       }) {
    require_error(venice::detail::download_video_with(
                      invalid, options, resolver, fetcher, ManualNow{&now}),
                  venice::ErrorKind::InvalidArg,
                  "video download timeouts must be positive");
  }
  require_error(venice::detail::download_video_with(
                    invalid, {.maximum_response_bytes = std::size_t{0}},
                    resolver, fetcher, ManualNow{&now}),
                venice::ErrorKind::InvalidArg,
                "video download byte limit must be nonzero");
  now = Clock::time_point::max() - 1ms;
  invalid.maximum_elapsed = 2ms;
  require_error(venice::detail::download_video_with(invalid, {}, resolver,
                                                    fetcher, ManualNow{&now}),
                venice::ErrorKind::InvalidArg,
                "video download deadline is out of range");
  CHECK(resolver.calls.empty());
  CHECK(fetcher.calls.empty());
}

TEST_CASE("video download body limits reject declarations and receive overflow",
          "[video-download][failure][bounds][body]") {
  venice::detail::VideoDownloadBodyLimit exact{4};
  CHECK(exact.accept_declared("4"));
  CHECK(exact.accept(2));
  CHECK(exact.accept(2));
  CHECK_FALSE(exact.exceeded());
  CHECK_FALSE(exact.accept(1));
  CHECK(exact.exceeded());

  venice::detail::VideoDownloadBodyLimit declared{4};
  CHECK_FALSE(declared.accept_declared("5"));
  CHECK(declared.exceeded());

  venice::detail::VideoDownloadBodyLimit malformed{4};
  CHECK_FALSE(malformed.accept_declared("4x"));
  CHECK_FALSE(malformed.exceeded());

  venice::detail::VideoDownloadBodyLimit misleading{4};
  CHECK(misleading.accept_declared("1"));
  CHECK_FALSE(misleading.accept(5));
  CHECK(misleading.exceeded());

  httplib::Response duplicate;
  duplicate.headers.emplace("Content-Length", "4");
  duplicate.headers.emplace("Content-Length", "400");
  CHECK_FALSE(venice::detail::video_download_headers_within_limit(malformed,
                                                                  duplicate));

  venice::detail::VideoDownloadBodyLimit encoded{4};
  httplib::Response headers;
  headers.set_header("Content-Length", "500");
  headers.set_header("Content-Encoding", "gzip");
  CHECK(venice::detail::video_download_headers_within_limit(encoded, headers));
  CHECK(encoded.accept(4));

  venice::detail::VideoDownloadBodyLimit chunked{4};
  headers.headers.clear();
  headers.set_header("Content-Length", "500");
  headers.set_header("Transfer-Encoding", "chunked");
  CHECK(venice::detail::video_download_headers_within_limit(chunked, headers));
  CHECK_FALSE(chunked.accept(5));

  venice::detail::VideoDownloadBodyLimit unsupported_transfer{4};
  headers.headers.clear();
  headers.set_header("Transfer-Encoding", "foreign-coding");
  CHECK_FALSE(venice::detail::video_download_headers_within_limit(
      unsupported_transfer, headers));

  httplib::Response unsupported;
  unsupported.set_header("Content-Encoding", "foreign-coding");
  CHECK_FALSE(
      venice::detail::video_download_content_encoding_supported(unsupported));
  unsupported.headers.clear();
  unsupported.set_header("Content-Encoding", "identity");
  CHECK(venice::detail::video_download_content_encoding_supported(unsupported));
  unsupported.headers.emplace("Content-Encoding", "gzip");
  CHECK_FALSE(
      venice::detail::video_download_content_encoding_supported(unsupported));
  unsupported.headers.clear();
  unsupported.set_header("Content-Encoding", "gzip");
#ifdef CPPHTTPLIB_ZLIB_SUPPORT
  CHECK(venice::detail::video_download_content_encoding_supported(unsupported));
#else
  CHECK_FALSE(
      venice::detail::video_download_content_encoding_supported(unsupported));
#endif
}

TEST_CASE("video download transport pins TLS and sends only safe headers",
          "[video-download][transport][tls][credentials]") {
  ScopedEnvironment upper_proxy{"HTTPS_PROXY", "http://127.0.0.1:1"};
  ScopedEnvironment lower_proxy{"https_proxy", "http://127.0.0.1:1"};
  ScopedEnvironment all_proxy{"ALL_PROXY", "http://127.0.0.1:1"};
  ScopedEnvironment lower_all_proxy{"all_proxy", "http://127.0.0.1:1"};
  ScopedEnvironment no_proxy{"NO_PROXY", ""};
  ScopedEnvironment lower_no_proxy{"no_proxy", ""};
  TlsDownloadFixture server;
  venice::detail::HttplibVideoDownloadFetcher fetcher{
      test_tls_identity().certificate_pem()};
  const venice::RequestOptions options{
      .connect_timeout = 500ms,
      .read_timeout = 500ms,
      .write_timeout = 500ms,
      .authentication = venice::Authentication::bearer("foreign-secret"),
      .idempotency_key = "foreign-idempotency",
  };

  const auto response =
      fetcher.fetch(server.url("media.example.test", "/exact"), "127.0.0.1", 4,
                    Clock::now() + 1s, options);
  REQUIRE(response.has_value());
  CHECK(response->status == 200);
  CHECK(response->body == std::string{"a\0b\xff", 4});
  CHECK(response->content_type == "video/mp4");
  CHECK(server.safe_request_seen());
}

TEST_CASE("video download transport fails closed on TLS hostname mismatch",
          "[video-download][failure][transport][tls]") {
  TlsDownloadFixture server;
  venice::detail::HttplibVideoDownloadFetcher fetcher{
      test_tls_identity().certificate_pem()};

  require_error(fetcher.fetch(server.url("wrong.example.test", "/exact"),
                              "127.0.0.1", 4, Clock::now() + 1s, {}),
                venice::ErrorKind::Network, "video download transport failed");
  CHECK_FALSE(server.safe_request_seen());
}

TEST_CASE("video download transport bounds chunked and encoded bodies",
          "[video-download][failure][transport][body]") {
  TlsDownloadFixture server;
  venice::detail::HttplibVideoDownloadFetcher fetcher{
      test_tls_identity().certificate_pem()};

  require_error(
      fetcher.fetch(server.url("media.example.test", "/chunked-overflow"),
                    "127.0.0.1", 4, Clock::now() + 1s, {}),
      venice::ErrorKind::ResponseTooLarge,
      "video download exceeds its byte limit");
  require_error(
      fetcher.fetch(server.url("media.example.test", "/unknown-encoding"),
                    "127.0.0.1", 4, Clock::now() + 1s, {}),
      venice::ErrorKind::Parse,
      "video download response encoding is unsupported");
#ifdef CPPHTTPLIB_ZLIB_SUPPORT
  require_error(
      fetcher.fetch(server.url("media.example.test", "/gzip-overflow"),
                    "127.0.0.1", 4, Clock::now() + 1s, {}),
      venice::ErrorKind::ResponseTooLarge,
      "video download exceeds its byte limit");
#endif
}

TEST_CASE("video download transport interrupts slow receive at total deadline",
          "[video-download][failure][transport][deadline]") {
  TlsDownloadFixture server;
  venice::detail::HttplibVideoDownloadFetcher fetcher{
      test_tls_identity().certificate_pem()};
  const auto started = Clock::now();
  const auto response = fetcher.fetch(
      server.url("media.example.test", "/slow"), "127.0.0.1", 8, started + 35ms,
      {.connect_timeout = 250ms, .read_timeout = 250ms});
  const auto elapsed = Clock::now() - started;

  require_error(response, venice::ErrorKind::Network,
                "video download deadline exceeded");
  CHECK(elapsed < 500ms);
}

TEST_CASE("video download transport interrupts slow receive on cancellation",
          "[video-download][failure][transport][cancel]") {
  TlsDownloadFixture server;
  venice::detail::HttplibVideoDownloadFetcher fetcher{
      test_tls_identity().certificate_pem()};
  venice::CancelToken cancellation;
  std::jthread cancel{[&cancellation] {
    std::this_thread::sleep_for(35ms);
    cancellation.cancel();
  }};
  const auto started = Clock::now();
  const auto response = fetcher.fetch(server.url("media.example.test", "/slow"),
                                      "127.0.0.1", 8, started + 1s,
                                      {.connect_timeout = 250ms,
                                       .read_timeout = 250ms,
                                       .cancel = &cancellation});
  const auto elapsed = Clock::now() - started;

  require_error(response, venice::ErrorKind::Cancelled,
                "video download cancelled by caller");
  CHECK(elapsed < 500ms);
}

TEST_CASE("video download URL validation fails closed before DNS",
          "[video-download][failure][url]") {
  const std::vector<std::string> invalid{
      "",
      "http://public.example/video.mp4",
      "ftp://public.example/video.mp4",
      "https://",
      "https://user:secret@public.example/video.mp4",
      "https://public.example:0/video.mp4",
      "https://public.example:-1/video.mp4",
      "https://public.example:443x/video.mp4",
      "https://public.example:65536/video.mp4",
      "https://public.example/video.mp4#secret",
      "https://public.example/a b",
      "https://public.example/a\\b",
      "https://public.example/%zz",
      "https://exa_mple.test/video.mp4",
      "https://2001:db8::1/video.mp4",
      "https://[not-ipv6]/video.mp4",
  };

  for (const auto &url : invalid) {
    CAPTURE(url);
    Clock::time_point now{};
    ScriptedResolver resolver;
    ScriptedFetcher fetcher;
    require_error(venice::detail::download_video_with(
                      request(url), {}, resolver, fetcher, ManualNow{&now}),
                  venice::ErrorKind::InvalidArg, "invalid video download URL");
    CHECK(resolver.calls.empty());
    CHECK(fetcher.calls.empty());
  }
}

TEST_CASE("video download classifies public and special-use addresses",
          "[video-download][failure][ssrf]") {
  const std::vector<std::string> blocked{
      "0.0.0.0",
      "10.0.0.1",
      "100.64.0.1",
      "127.0.0.1",
      "169.254.169.254",
      "172.16.0.1",
      "192.0.0.1",
      "192.0.2.1",
      "192.88.99.1",
      "192.168.1.1",
      "198.18.0.1",
      "198.51.100.1",
      "203.0.113.1",
      "224.0.0.1",
      "255.255.255.255",
      "::",
      "::1",
      "::ffff:127.0.0.1",
      "64:ff9b::7f00:1",
      "64:ff9b:1::1",
      "100::1",
      "100:0:0:1::1",
      "2000:ffff::1",
      "2001::1",
      "2001:1ff:ffff::1",
      "2001:2::1",
      "2001:10::1",
      "2001:20::1",
      "2001:30::1",
      "2001:1000::1",
      "2001:11ff:ffff::1",
      "2001:4e00::1",
      "2001:7fff::1",
      "2001:c000::1",
      "2001:db8::1",
      "2002:0808:0808::1",
      "2003:4000::1",
      "2420::1",
      "25ff::1",
      "2610:200::1",
      "2611::1",
      "2620:200::1",
      "262f::1",
      "2640::1",
      "2810::1",
      "2a20::1",
      "2c10::1",
      "2d00::1",
      "3ffe::1",
      "3fff::1",
      "5f00::1",
      "fc00::1",
      "fe80::1",
      "ff02::1",
      "not-an-address",
  };
  for (const auto &address : blocked) {
    CAPTURE(address);
    CHECK_FALSE(venice::detail::video_download_address_is_public(address));
  }
  CHECK(venice::detail::video_download_address_is_public("8.8.8.8"));
  CHECK(venice::detail::video_download_address_is_public("100.63.255.255"));
  CHECK(venice::detail::video_download_address_is_public("100.128.0.1"));
  CHECK(venice::detail::video_download_address_is_public("172.15.255.255"));
  CHECK(venice::detail::video_download_address_is_public("172.32.0.1"));
  CHECK(venice::detail::video_download_address_is_public("93.184.216.34"));
  CHECK(venice::detail::video_download_address_is_public("192.31.196.1"));
  CHECK(venice::detail::video_download_address_is_public("2001:200::1"));
  CHECK(venice::detail::video_download_address_is_public("2001:fff:ffff::1"));
  CHECK(venice::detail::video_download_address_is_public("2001:1200::1"));
  CHECK(venice::detail::video_download_address_is_public("2001:3fff:ffff::1"));
  CHECK(venice::detail::video_download_address_is_public("2001:4000::1"));
  CHECK(venice::detail::video_download_address_is_public("2001:4dff:ffff::1"));
  CHECK(venice::detail::video_download_address_is_public("2001:5000::1"));
  CHECK(venice::detail::video_download_address_is_public("2001:5fff:ffff::1"));
  CHECK(venice::detail::video_download_address_is_public("2001:8000::1"));
  CHECK(venice::detail::video_download_address_is_public("2001:bfff:ffff::1"));
  CHECK(venice::detail::video_download_address_is_public("2003::1"));
  CHECK(venice::detail::video_download_address_is_public("2003:3fff::1"));
  CHECK(venice::detail::video_download_address_is_public("2400::1"));
  CHECK(venice::detail::video_download_address_is_public("241f::1"));
  CHECK(venice::detail::video_download_address_is_public("2600::1"));
  CHECK(venice::detail::video_download_address_is_public("260f::1"));
  CHECK(venice::detail::video_download_address_is_public("2610::1"));
  CHECK(venice::detail::video_download_address_is_public("2610:1ff::1"));
  CHECK(venice::detail::video_download_address_is_public("2620::1"));
  CHECK(venice::detail::video_download_address_is_public("2620:1ff::1"));
  CHECK(venice::detail::video_download_address_is_public("2630::1"));
  CHECK(venice::detail::video_download_address_is_public("263f::1"));
  CHECK(venice::detail::video_download_address_is_public("2800::1"));
  CHECK(venice::detail::video_download_address_is_public("280f::1"));
  CHECK(venice::detail::video_download_address_is_public("2a00::1"));
  CHECK(venice::detail::video_download_address_is_public("2a1f::1"));
  CHECK(venice::detail::video_download_address_is_public("2c00::1"));
  CHECK(venice::detail::video_download_address_is_public("2c0f::1"));
  CHECK(
      venice::detail::video_download_address_is_public("2606:4700:4700::1111"));
  CHECK(venice::detail::video_download_address_is_public("::ffff:8.8.8.8"));
}

TEST_CASE("one restricted DNS answer rejects the entire answer set",
          "[video-download][failure][ssrf][dns]") {
  Clock::time_point now{};
  ScriptedResolver resolver{
      .answers = {{"media.example.test", {"93.184.216.34", "127.0.0.1"}}}};
  ScriptedFetcher fetcher{.responses = {ok()}};
  require_error(venice::detail::download_video_with(request(), {}, resolver,
                                                    fetcher, ManualNow{&now}),
                venice::ErrorKind::InvalidArg,
                "video download destination is not public");
  REQUIRE(resolver.calls == std::vector<std::string>{"media.example.test"});
  CHECK(fetcher.calls.empty());
}

TEST_CASE("video download returns exact bounded MP4 bytes without credentials",
          "[video-download][success][credentials]") {
  Clock::time_point now{};
  ScriptedResolver resolver;
  std::string bytes{"a\0b\xff", 4};
  ScriptedFetcher fetcher{
      .responses = {{200, " VIDEO/MP4 ; fixture=true", bytes, std::nullopt}}};
  venice::RequestOptions options{
      .connect_timeout = 17ms,
      .read_timeout = 18ms,
      .write_timeout = 19ms,
      .authentication = venice::Authentication::bearer("must-not-leak"),
      .idempotency_key = "must-not-leak",
  };

  const auto result = venice::detail::download_video_with(
      request(), options, resolver, fetcher, ManualNow{&now});
  REQUIRE(result.has_value());
  CHECK(result->bytes == bytes);
  CHECK(result->media_type == "video/mp4");
  CHECK(result->metadata.headers.empty());
  REQUIRE(fetcher.calls.size() == 1);
  CHECK(fetcher.calls[0].address == "93.184.216.34");
  CHECK(fetcher.calls[0].url.host == "media.example.test");
  CHECK(fetcher.calls[0].maximum_bytes == 8);
  CHECK(fetcher.calls[0].options.connect_timeout == 17ms);
  CHECK(fetcher.calls[0].options.read_timeout == 18ms);
  CHECK(fetcher.calls[0].options.write_timeout == 19ms);
  CHECK_FALSE(fetcher.calls[0].options.authentication.has_value());
  CHECK_FALSE(fetcher.calls[0].options.idempotency_key.has_value());
  CHECK(fetcher.calls[0].options.maximum_response_bytes == 8);
}

TEST_CASE("shared response ceiling tightens the video request ceiling",
          "[video-download][failure][bounds]") {
  Clock::time_point now{};
  ScriptedResolver resolver;
  ScriptedFetcher fetcher{.responses = {ok("1234")}};
  require_error(venice::detail::download_video_with(
                    request(), {.maximum_response_bytes = std::size_t{3}},
                    resolver, fetcher, ManualNow{&now}),
                venice::ErrorKind::ResponseTooLarge,
                "video download exceeds its byte limit");
  REQUIRE(fetcher.calls.size() == 1);
  CHECK(fetcher.calls[0].maximum_bytes == 3);
  CHECK(fetcher.calls[0].options.maximum_response_bytes == 3);
}

TEST_CASE("video download rejects wrong MIME empty and oversized bodies",
          "[video-download][failure][media]") {
  const std::vector<
      std::pair<venice::detail::VideoDownloadResponse, venice::ErrorKind>>
      cases{
          {{200, "", "mp4", std::nullopt}, venice::ErrorKind::Parse},
          {{200, "text/plain", "secret", std::nullopt},
           venice::ErrorKind::Parse},
          {{200, "video/mp4", "", std::nullopt}, venice::ErrorKind::Parse},
          {{200, "video/mp4", "123456789", std::nullopt},
           venice::ErrorKind::ResponseTooLarge},
      };
  for (const auto &[response, kind] : cases) {
    Clock::time_point now{};
    ScriptedResolver resolver;
    ScriptedFetcher fetcher{.responses = {response}};
    const auto result = venice::detail::download_video_with(
        request(), {}, resolver, fetcher, ManualNow{&now});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().kind == kind);
    CHECK(result.error().body.empty());
    CHECK(result.error().metadata.headers.empty());
    CHECK(result.error().message.find("secret") == std::string::npos);
  }
}

TEST_CASE("video download follows bounded redirects and repins every hop",
          "[video-download][redirect][dns]") {
  Clock::time_point now{};
  ScriptedResolver resolver{
      .answers = {{"media.example.test", {"93.184.216.34"}},
                  {"cdn.example.test", {"8.8.8.8"}}}};
  ScriptedFetcher fetcher{
      .responses = {{302, "text/html", "redirect",
                     "https://cdn.example.test/a/../final.mp4?token=opaque"},
                    ok("done")}};

  const auto result = venice::detail::download_video_with(
      request(), {}, resolver, fetcher, ManualNow{&now});
  REQUIRE(result.has_value());
  CHECK(result->bytes == "done");
  REQUIRE(resolver.calls ==
          std::vector<std::string>{"media.example.test", "cdn.example.test"});
  REQUIRE(fetcher.calls.size() == 2);
  CHECK(fetcher.calls[0].address == "93.184.216.34");
  CHECK(fetcher.calls[1].address == "8.8.8.8");
  CHECK(fetcher.calls[1].url.target == "/final.mp4?token=opaque");
}

TEST_CASE("video download resolves relative and query-only redirects exactly",
          "[video-download][redirect]") {
  SECTION("relative path preserves empty segments") {
    const auto base = venice::detail::parse_video_download_url(
        "https://media.example.test/a//b/start.mp4?old=1");
    REQUIRE(base.has_value());
    const auto next = venice::detail::resolve_video_download_redirect(
        *base, "../next//clip.mp4?new=2");
    REQUIRE(next.has_value());
    CHECK(next->target == "/a//next//clip.mp4?new=2");
  }
  SECTION("query-only") {
    const auto base = venice::detail::parse_video_download_url(
        "https://media.example.test/video.mp4");
    REQUIRE(base.has_value());
    const auto next =
        venice::detail::resolve_video_download_redirect(*base, "?token=new");
    REQUIRE(next.has_value());
    CHECK(next->target == "/video.mp4?token=new");
  }
}

TEST_CASE("video download redirects reject loops downgrade and private targets",
          "[video-download][failure][redirect]") {
  SECTION("loop") {
    Clock::time_point now{};
    ScriptedResolver resolver;
    ScriptedFetcher fetcher{.responses = {{302, {}, {}, "/video.mp4"}}};
    require_error(venice::detail::download_video_with(request(), {}, resolver,
                                                      fetcher, ManualNow{&now}),
                  venice::ErrorKind::Http, "video download redirect loop");
  }
  SECTION("downgrade") {
    Clock::time_point now{};
    ScriptedResolver resolver;
    ScriptedFetcher fetcher{
        .responses = {{302, {}, {}, "http://public.example/video.mp4"}}};
    require_error(venice::detail::download_video_with(request(), {}, resolver,
                                                      fetcher, ManualNow{&now}),
                  venice::ErrorKind::InvalidArg, "invalid video download URL");
  }
  SECTION("non-hierarchical scheme") {
    Clock::time_point now{};
    ScriptedResolver resolver;
    ScriptedFetcher fetcher{.responses = {{302, {}, {}, "javascript:opaque"}}};
    require_error(venice::detail::download_video_with(request(), {}, resolver,
                                                      fetcher, ManualNow{&now}),
                  venice::ErrorKind::InvalidArg,
                  "invalid video download redirect");
  }
  SECTION("private redirect target") {
    Clock::time_point now{};
    ScriptedResolver resolver{
        .answers = {{"private.example", {"169.254.169.254"}}}};
    ScriptedFetcher fetcher{
        .responses = {{302, {}, {}, "https://private.example/latest"}}};
    require_error(venice::detail::download_video_with(request(), {}, resolver,
                                                      fetcher, ManualNow{&now}),
                  venice::ErrorKind::InvalidArg,
                  "video download destination is not public");
  }
  SECTION("oversized redirect body") {
    Clock::time_point now{};
    ScriptedResolver resolver;
    ScriptedFetcher fetcher{.responses = {{302, {}, "123456789", "/next.mp4"}}};
    require_error(venice::detail::download_video_with(request(), {}, resolver,
                                                      fetcher, ManualNow{&now}),
                  venice::ErrorKind::ResponseTooLarge,
                  "video download exceeds its byte limit");
    CHECK(fetcher.calls.size() == 1);
  }
}

TEST_CASE("video download caps redirects at five",
          "[video-download][failure][redirect][bounds]") {
  Clock::time_point now{};
  ScriptedResolver resolver;
  ScriptedFetcher fetcher;
  for (int index = 1; index <= 6; ++index)
    fetcher.responses.push_back(
        {302, {}, "foreign", "/redirect-" + std::to_string(index) + ".mp4"});
  require_error(venice::detail::download_video_with(request(), {}, resolver,
                                                    fetcher, ManualNow{&now}),
                venice::ErrorKind::Http,
                "video download redirect was not usable");
  CHECK(fetcher.calls.size() == 6);
}

TEST_CASE("video download whole-operation deadline includes DNS and receive",
          "[video-download][failure][deadline]") {
  SECTION("DNS consumes the deadline") {
    Clock::time_point now{};
    ScriptedResolver resolver{.now = &now, .advance = 1001ms};
    ScriptedFetcher fetcher{.responses = {ok()}};
    require_error(venice::detail::download_video_with(request(), {}, resolver,
                                                      fetcher, ManualNow{&now}),
                  venice::ErrorKind::Network,
                  "video download deadline exceeded");
    CHECK(fetcher.calls.empty());
  }
  SECTION("receive consumes the deadline") {
    Clock::time_point now{};
    ScriptedResolver resolver;
    ScriptedFetcher fetcher{
        .responses = {ok()}, .now = &now, .advance = 1001ms};
    require_error(venice::detail::download_video_with(request(), {}, resolver,
                                                      fetcher, ManualNow{&now}),
                  venice::ErrorKind::Network,
                  "video download deadline exceeded");
  }
}

TEST_CASE("video download cancellation wins before DNS and after each boundary",
          "[video-download][failure][cancel]") {
  SECTION("pre-cancelled") {
    Clock::time_point now{};
    venice::CancelToken cancellation;
    cancellation.cancel();
    ScriptedResolver resolver;
    ScriptedFetcher fetcher;
    require_error(venice::detail::download_video_with(
                      request(), {.cancel = &cancellation}, resolver, fetcher,
                      ManualNow{&now}),
                  venice::ErrorKind::Cancelled,
                  "video download cancelled by caller");
    CHECK(resolver.calls.empty());
    CHECK(fetcher.calls.empty());
  }
  SECTION("during DNS") {
    Clock::time_point now{};
    venice::CancelToken cancellation;
    ScriptedResolver resolver{.cancel_during_call = &cancellation};
    ScriptedFetcher fetcher{.responses = {ok()}};
    require_error(venice::detail::download_video_with(
                      request(), {.cancel = &cancellation}, resolver, fetcher,
                      ManualNow{&now}),
                  venice::ErrorKind::Cancelled,
                  "video download cancelled by caller");
    CHECK(fetcher.calls.empty());
  }
  SECTION("during receive") {
    Clock::time_point now{};
    venice::CancelToken cancellation;
    ScriptedResolver resolver;
    ScriptedFetcher fetcher{.responses = {ok()},
                            .cancel_during_call = &cancellation};
    require_error(venice::detail::download_video_with(
                      request(), {.cancel = &cancellation}, resolver, fetcher,
                      ManualNow{&now}),
                  venice::ErrorKind::Cancelled,
                  "video download cancelled by caller");
  }
}

TEST_CASE("video download HTTP errors are bounded and redact foreign data",
          "[video-download][failure][redaction]") {
  Clock::time_point now{};
  ScriptedResolver resolver;
  ScriptedFetcher fetcher{.responses = {{403, "text/plain", "secret",
                                         "https://secret.example/token"}}};
  const auto result = venice::detail::download_video_with(
      request("https://user-visible.example/path?secret=one"), {}, resolver,
      fetcher, ManualNow{&now});
  require_error(result, venice::ErrorKind::Auth,
                "video download returned a non-success status");
  CHECK(result.error().status == 403);
  CHECK(result.error().message.find("secret") == std::string::npos);
  CHECK(result.error().message.find("user-visible") == std::string::npos);
}
