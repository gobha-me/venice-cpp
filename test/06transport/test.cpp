// Transport timeouts and cancellation — VC-06 (#7).
//
// Charter: transport behavior that requires a peer — timeouts, cancellation,
// authentication, exact HTTP targets and response classification. Pure request
// bodies stay in test/02request/, pure path/query transforms in test/05query/,
// response parsers in their endpoint suites, and socket-free request guards in
// test/03guards/. This file inspects bodies only where the loopback peer is what
// proves the transport preserved or classified them correctly.
//
// ── Why this file binds a socket, when no other test does ──────────────────
//
// AGENTS.md's offline rule is "no API key and no network", and that stands:
// nothing here touches api.venice.ai or reads $VENICE_API_KEY. But a timeout is
// by definition a property of a peer that does not answer, and cancellation is
// a property of a call already in flight. Neither has an offline form — there
// is no pure function to extract the way percent_encode was extracted in
// VC-13. So this test brings its own peer: an httplib::Server on 127.0.0.1 at
// an ephemeral port, plain HTTP, in-process, torn down with the fixture.
//
// Client::host() splits base_url on "/api/", which is what makes that possible
// with no TLS and no change to the library: "http://127.0.0.1:<port>/api/v1"
// gives a host of "http://127.0.0.1:<port>" and a path prefix of "/api/v1".
//
// ── What makes these assertions non-flaky ─────────────────────────────────
//
// The timing cases are all one-sided, and the gap is three orders of magnitude
// rather than a tight margin. The default read timeout is 300 *seconds*, so a
// cancelled call is asserted to return in under five — a bound a loaded CI
// runner cannot plausibly miss, and one that a broken cancellation cannot
// plausibly meet. Nothing here asserts that something happened *quickly enough*
// in a sense that could drift.
//
// Where a direct assertion exists it is preferred over a timing one: §0 counts
// server-side hits rather than measuring a clock, because "no request was sent"
// is the actual contract, and a clock cannot tell it apart from "a request was
// sent and aborted".
//
// Failure matrix first, happy path last.

#include <catch2/catch_test_macros.hpp>

#ifndef _WIN32
#include <csignal>
#include <pthread.h>
#endif

#include <array>
#include <atomic>
#include <chrono>
#include <expected>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <venice/venice.hpp>

using venice::ChatRequest;
using venice::Client;
using venice::EmbeddingRequest;
using venice::ErrorKind;
using venice::ImageBackgroundRemovalRequest;
using venice::ImageEditRequest;
using venice::ImageGenerationRequest;
using venice::ImageUpscaleRequest;
using venice::Message;
using venice::MultiImageEditRequest;
using venice::OpenAIImageGenerationRequest;
using venice::ResponsesRequest;
using venice::Authentication;

using namespace std::chrono_literals;

namespace {

// Every stall in this file is bounded *and* interruptible. Bounded so a hung
// handler cannot wedge the suite; interruptible so teardown does not have to
// wait out the bound. A plain sleep would satisfy neither — httplib's listen()
// does not return until its worker threads do, so a handler sleeping ten
// seconds is ten seconds added to the test run even after everything has been
// asserted.
class Gate {
 public:
  void wait(std::chrono::milliseconds cap) {
    std::unique_lock<std::mutex> lock{m_mu};
    m_cv.wait_for(lock, cap, [this] { return m_open; });
  }

  void open() {
    {
      const std::lock_guard<std::mutex> lock{m_mu};
      m_open = true;
    }
    m_cv.notify_all();
  }

 private:
  std::mutex m_mu;
  std::condition_variable m_cv;
  bool m_open = false;
};

struct CapturedTransformPart {
  std::string name{};
  std::string filename{};
  std::string content_type{};
  std::string content{};
};

struct CapturedTransform {
  std::string path{};
  std::string content_type{};
  std::string authorization{};
  std::string siwx{};
  std::string body{};
  std::vector<CapturedTransformPart> parts{};
};

enum class Web3ChallengeMode { Success, HttpError, WrongMedia, WrongShape, MalformedJson };

struct CapturedWeb3Request {
  std::string method{};
  std::string path{};
  std::string content_type{};
  std::string authorization{};
  std::string siwx{};
  std::string payment{};
  std::string body{};
};

struct CapturedX402Request {
  std::string method{};
  std::string path{};
  std::string content_type{};
  std::string authorization{};
  std::string siwx{};
  std::string payment{};
  std::string body{};
};

// Two independent origins make redirect leakage observable without touching the
// internet. The first origin returns only 3xx responses; the second counts every
// request that reaches it. A separate same-origin target pins the stronger
// policy: Venice publishes no redirect contract, so no Location is followed.
class RedirectFixture {
 public:
  RedirectFixture() {
    const auto capture_destination = [this](const httplib::Request&,
                                            httplib::Response& res) {
      ++m_destination_hits;
      res.set_content("destination must not be reached", "text/plain");
    };
    m_destination.Get("/captured", capture_destination);
    m_destination.Post("/captured", capture_destination);
    m_destination_port = m_destination.bind_to_any_port("127.0.0.1");
    m_destination_thread =
        std::thread{[this] { m_destination.listen_after_bind(); }};
    m_destination.wait_until_ready();

    const auto redirect = [this](const httplib::Request& req,
                                 httplib::Response& res) {
      const int status = req.has_param("status")
                             ? std::stoi(req.get_param_value("status"))
                             : 307;
      const bool same_origin = req.has_param("same-origin");
      res.status = status;
      res.set_header("Location", same_origin
                                     ? "/api/v1/transport/followed"
                                     : destination_url() + "/captured");
      res.set_header("X-Protocol-Trace", "origin-redirect");
      res.set_content("redirect blocked", "text/plain");
    };
    m_origin.Get("/api/v1/transport/redirect", redirect);
    m_origin.Post("/api/v1/transport/redirect", redirect);
    m_origin.Get("/api/v1/models", redirect);
    m_origin.Post("/api/v1/chat/completions", redirect);
    m_origin.Post("/api/v1/audio/speech",
                  [this](const httplib::Request&, httplib::Response& res) {
                    res.status = 308;
                    res.set_header("Location", destination_url() + "/captured");
                    res.set_header("X-Protocol-Trace", "origin-redirect");
                    res.set_content("redirect blocked", "text/plain");
                  });
    const auto capture_same_origin = [this](const httplib::Request&,
                                            httplib::Response& res) {
      ++m_same_origin_hits;
      res.set_content("same origin must not be reached", "text/plain");
    };
    m_origin.Get("/api/v1/transport/followed", capture_same_origin);
    m_origin.Post("/api/v1/transport/followed", capture_same_origin);
    m_origin_port = m_origin.bind_to_any_port("127.0.0.1");
    m_origin_thread = std::thread{[this] { m_origin.listen_after_bind(); }};
    m_origin.wait_until_ready();
  }

  ~RedirectFixture() {
    m_origin.stop();
    m_destination.stop();
    m_origin_thread.join();
    m_destination_thread.join();
  }

  RedirectFixture(const RedirectFixture&) = delete;
  auto operator=(const RedirectFixture&) -> RedirectFixture& = delete;
  RedirectFixture(RedirectFixture&&) = delete;
  auto operator=(RedirectFixture&&) -> RedirectFixture& = delete;

  [[nodiscard]] auto origin_base_url() const -> std::string {
    return "http://127.0.0.1:" + std::to_string(m_origin_port) + "/api/v1";
  }

  [[nodiscard]] auto destination_hits() const -> int {
    return m_destination_hits.load();
  }

  [[nodiscard]] auto same_origin_hits() const -> int {
    return m_same_origin_hits.load();
  }

 private:
  [[nodiscard]] auto destination_url() const -> std::string {
    return "http://127.0.0.1:" + std::to_string(m_destination_port);
  }

  httplib::Server m_origin;
  httplib::Server m_destination;
  std::thread m_origin_thread;
  std::thread m_destination_thread;
  int m_origin_port = 0;
  int m_destination_port = 0;
  std::atomic<int> m_destination_hits{0};
  std::atomic<int> m_same_origin_hits{0};
};

// A peer that can be made to stop answering.
//
//   GET  /api/v1/api_keys/rate_limits — accepts, then never answers (until
//        teardown). This is the "server stalls after accepting" shape: the
//        client is blocked waiting for a status line that is not coming.
//   POST /api/v1/chat/completions     — writes two SSE content frames, then
//        stalls. The "quiet gap mid-stream" shape, and the one on_token alone
//        cannot escape.
//   GET  /api/v1/models               — answers immediately. The control.
//   GET  /api/v1/characters/{slug}    — echoes the exact encoded target or
//                                        returns detail-specific failures.
//   GET  /api/v1/characters/{slug}/reviews — the same, one route deeper, and
//        registered ahead of the catch-all above it.
//   GET  /api/v1/models/traits  and  /api/v1/models/compatibility_mapping —
//        echo the encoded target and the Authorization header *inside* `data`,
//        where the typed map surface makes them assertable. The mapping route
//        refuses type=all with the real 400 body; the traits route answers
//        type=no-data with a 200 that has no `data` at all.
//
// Request counts are exposed so a test can assert a call did not happen, which
// no clock reading can establish.
class TestServer {
 public:
  explicit TestServer(Web3ChallengeMode web3_challenge_mode =
                          Web3ChallengeMode::Success)
      : m_web3_challenge_mode(web3_challenge_mode) {
    const auto echo = [](const httplib::Request& req, httplib::Response& res) {
      nlohmann::json body;
      body["method"] = req.method;
      body["target"] = req.target;
      body["content_type"] = req.get_header_value("Content-Type");
      body["x_test"] = req.get_header_value("X-Test-Request");
      body["authorization"] = req.get_header_value("Authorization");
      body["siwx"] = req.get_header_value("SIGN-IN-WITH-X");
      body["payment"] = req.get_header_value("PAYMENT-SIGNATURE");
      body["body"] = req.body;
      res.set_header("X-Test-Response", "retained");
      res.set_content(body.dump(), "application/json; charset=utf-8");
    };
    m_svr.Get("/api/v1/transport/echo", echo);
    m_svr.Post("/api/v1/transport/echo", echo);
    m_svr.Patch("/api/v1/transport/echo", echo);
    m_svr.Delete("/api/v1/transport/echo", echo);

    m_svr.Post("/api/v1/transport/multipart",
               [](const httplib::Request& req, httplib::Response& res) {
                 nlohmann::json parts = nlohmann::json::array();
                 for (const auto& [name, part] : req.files) {
                   parts.push_back({{"name", name},
                                    {"filename", part.filename},
                                    {"content_type", part.content_type},
                                    {"content", part.content}});
                 }
                 res.set_header("X-Multipart-Seen", "yes");
                 res.set_content(nlohmann::json{{"parts", std::move(parts)}}.dump(),
                                 "application/json");
               });
    m_svr.Post("/api/v1/transport/multipart-stall",
               [this](const httplib::Request&, httplib::Response& res) {
                 ++m_multipart_stall_hits;
                 m_gate.wait(kStallCap);
                 res.set_content("{}", "application/json");
               });

    m_svr.Get("/api/v1/transport/text", [](const httplib::Request&, httplib::Response& res) {
      res.set_header("X-Test-Response", "text");
      res.set_content("plain response", "Text/Plain; charset=UTF-8");
    });
    m_svr.Get("/api/v1/transport/csv", [](const httplib::Request&, httplib::Response& res) {
      res.set_header("X-Test-Response", "csv");
      res.set_content("a,b\n1,2\n", "text/csv; header=present");
    });
    m_svr.Get("/api/v1/transport/binary", [](const httplib::Request&, httplib::Response& res) {
      const std::array bytes{char{0}, char{1}, static_cast<char>(0xFF), char{'x'}};
      res.set_header("X-Test-Response", "binary");
      res.set_content(bytes.data(), bytes.size(), "Application/Octet-Stream; version=1");
    });
    m_svr.Get("/api/v1/transport/missing-type",
              [](const httplib::Request&, httplib::Response& res) {
                res.set_header("X-Protocol-Trace", "parse-metadata");
                res.body = R"({"ok":true})";
              });
    m_svr.Get("/api/v1/transport/wrong-type", [](const httplib::Request&, httplib::Response& res) {
      res.set_header("X-Protocol-Trace", "parse-metadata");
      res.set_content(R"({"ok":true})", "text/plain");
    });
    m_svr.Get("/api/v1/transport/vendor-json", [](const httplib::Request&, httplib::Response& res) {
      res.set_content(R"({"kind":"problem"})", "Application/Problem+JSON; charset=UTF-8");
    });
    m_svr.Get("/api/v1/transport/malformed-json",
              [](const httplib::Request&, httplib::Response& res) {
                res.set_header("X-Protocol-Trace", "parse-metadata");
                res.set_content("{", "application/json");
              });
    m_svr.Get("/api/v1/transport/http-error", [](const httplib::Request&, httplib::Response& res) {
      res.status = 418;
      res.set_header("X-Protocol-Trace", "http-metadata");
      res.set_content("not JSON and that does not matter", "text/plain");
    });

    m_svr.Get("/api/v1/api_keys/rate_limits",
              [this](const httplib::Request& req, httplib::Response& res) {
                // Historical timeout/cancellation tests use their ordinary
                // fixture token and need this route to stall. VC-43's typed
                // contract tests opt into the non-stalling response with a
                // distinct synthetic bearer.
                if (req.get_header_value("Authorization") != "Bearer api-key-fixture") {
                  ++m_stall_hits;
                  m_gate.wait(kStallCap);
                  res.set_content("{}", "application/json");
                  return;
                }
                ++m_api_key_hits;
                res.set_header("X-Test-Response", "api-key-rate-limits");
                res.set_content(
                    nlohmann::json{
                        {"data",
                         {{"accessPermitted", true},
                          {"apiTier", {{"id", "paid"}, {"isCharged", true}}},
                          {"balances", {{"USD", 0}, {"DIEM", 2.5}}},
                          {"keyExpiration", nullptr},
                          {"nextEpochBegins", "later"},
                          {"rateLimits",
                           nlohmann::json::array({{{"apiModelId", "fixture-model"},
                                                  {"rateLimits",
                                                   nlohmann::json::array(
                                                       {{{"amount", 10}, {"type", "RPM"}}})}}})},
                          {"target", req.target},
                          {"authorization", req.get_header_value("Authorization")}}},
                        {"futureEnvelope", true}}
                        .dump(),
                    "application/json");
              });

    m_svr.Get("/api/v1/api_keys/rate_limits/log",
              [this](const httplib::Request& req, httplib::Response& res) {
                ++m_api_key_hits;
                res.set_header("X-Test-Response", "api-key-rate-limit-logs");
                res.set_content(
                    nlohmann::json{
                        {"data",
                         nlohmann::json::array({{{"apiKeyId", "fixture-id"},
                                                  {"modelId", "fixture-model"},
                                                  {"rateLimitTier", "paid"},
                                                  {"rateLimitType", "RPM"},
                                                  {"timestamp", "now"},
                                                  {"target", req.target}}})},
                        {"object", "list"},
                        {"authorization", req.get_header_value("Authorization")}}
                        .dump(),
                    "application/json");
              });

    const auto key_object = [](const httplib::Request& req) {
      return nlohmann::json{{"apiKeyType", "INFERENCE"},
                            {"consumptionLimits", {{"usd", 0}, {"diem", nullptr}}},
                            {"limitPeriod", "EPOCH"},
                            {"createdAt", "created"},
                            {"description", "fixture"},
                            {"expiresAt", nullptr},
                            {"id", "fixture-id"},
                            {"last6Chars", "ABC123"},
                            {"lastUsedAt", nullptr},
                            {"target", req.target},
                            {"method", req.method},
                            {"authorization", req.get_header_value("Authorization")}};
    };

    m_svr.Get("/api/v1/api_keys",
              [this, key_object](const httplib::Request& req, httplib::Response& res) {
                ++m_api_key_hits;
                res.set_header("X-Test-Response", "api-key-list");
                res.set_content(
                    nlohmann::json{{"data", nlohmann::json::array({key_object(req)})},
                                   {"object", "list"},
                                   {"futureEnvelope", true}}
                        .dump(),
                    "application/json");
              });

    m_svr.Post("/api/v1/api_keys",
               [this](const httplib::Request& req, httplib::Response& res) {
                 ++m_api_key_hits;
                 res.set_header("X-Test-Response", "api-key-create");
                 const auto body = nlohmann::json::parse(req.body);
                 const nlohmann::json secret_error{
                     {"data", {{"apiKey", "SYNTHETIC_SECRET_RETURNED_ONCE"}}},
                     {"error", "synthetic failure"}};
                 if (body.value("forceHttpError", false)) {
                   res.status = 400;
                   res.set_content(secret_error.dump(), "application/json");
                   return;
                 }
                 if (body.value("forceMalformed", false)) {
                   res.set_content(secret_error.dump(), "application/json");
                   return;
                 }
                 res.set_content(
                     nlohmann::json{
                         {"data",
                          {{"apiKey", "SYNTHETIC_SECRET_RETURNED_ONCE"},
                           {"apiKeyType", "INFERENCE"},
                           {"consumptionLimit", {{"usd", 0}}},
                           {"limitPeriod", "EPOCH"},
                           {"expiresAt", nullptr},
                           {"id", "fixture-created-id"}}},
                         {"success", true},
                         {"seenBody", body},
                         {"authorization", req.get_header_value("Authorization")}}
                         .dump(),
                     "application/json");
               });

    m_svr.Patch("/api/v1/api_keys",
                [this, key_object](const httplib::Request& req, httplib::Response& res) {
                  ++m_api_key_hits;
                  res.set_header("X-Test-Response", "api-key-update");
                  res.set_content(
                      nlohmann::json{{"data", key_object(req)},
                                     {"success", true},
                                     {"seenBody", nlohmann::json::parse(req.body)}}
                          .dump(),
                      "application/json");
                });

    m_svr.Delete("/api/v1/api_keys",
                 [this](const httplib::Request& req, httplib::Response& res) {
                   ++m_api_key_hits;
                   res.set_header("X-Test-Response", "api-key-delete");
                   res.set_content(
                       nlohmann::json{{"success", true},
                                      {"target", req.target},
                                      {"authorization", req.get_header_value("Authorization")}}
                           .dump(),
                       "application/json");
                 });

    m_svr.Get("/api/v1/api_keys/generate_web3_key",
              [this](const httplib::Request& req, httplib::Response& res) {
                ++m_web3_api_key_hits;
                capture_web3(req);
                res.set_header("X-Test-Response", "web3-api-key-challenge");
                switch (m_web3_challenge_mode) {
                  case Web3ChallengeMode::HttpError:
                    res.status = 401;
                    res.set_header("X-Protocol-Trace", "web3-challenge-http");
                    res.set_content(
                        nlohmann::json{
                            {"error", "denied"},
                            {"data", {{"token", "SYNTHETIC_CHALLENGE_SECRET"}}},
                            {"signature", "SYNTHETIC_SIGNATURE_SECRET"},
                            {"apiKey", "SYNTHETIC_API_KEY_SECRET"}}
                            .dump(),
                        "application/json");
                    return;
                  case Web3ChallengeMode::WrongMedia:
                    res.set_content("SYNTHETIC_CHALLENGE_SECRET", "text/plain");
                    return;
                  case Web3ChallengeMode::WrongShape:
                    res.set_content(
                        nlohmann::json{{"success", "yes"},
                                       {"data", {{"token", "SYNTHETIC_CHALLENGE_SECRET"}}}}
                            .dump(),
                        "application/json");
                    return;
                  case Web3ChallengeMode::MalformedJson:
                    res.set_content("{SYNTHETIC_CHALLENGE_SECRET", "application/json");
                    return;
                  case Web3ChallengeMode::Success:
                    res.set_content(
                        nlohmann::json{
                            {"data",
                             {{"token", "SYNTHETIC_CHALLENGE_SECRET"},
                              {"future", true}}},
                            {"success", true},
                            {"futureEnvelope",
                             {{"signature", "SYNTHETIC_SIGNATURE_SECRET"}}}}
                            .dump(),
                        "application/json");
                    return;
                }
              });

    m_svr.Post("/api/v1/api_keys/generate_web3_key",
               [this](const httplib::Request& req, httplib::Response& res) {
                 ++m_web3_api_key_hits;
                 capture_web3(req);
                 res.set_header("X-Test-Response", "web3-api-key-create");
                 const auto body = nlohmann::json::parse(req.body);
                 const auto secret_response = [&] {
                   return nlohmann::json{
                       {"data",
                        {{"apiKey", "SYNTHETIC_WEB3_API_KEY_SECRET"},
                         {"apiKeyType", body.at("apiKeyType")},
                         {"consumptionLimit", body.value("consumptionLimit",
                                                         nlohmann::json::object())},
                         {"limitPeriod", body.value("limitPeriod", "EPOCH")},
                         {"description", body.value("description", "Web3 API Key")},
                         {"expiresAt", body.value("expiresAt", nlohmann::json(nullptr))},
                         {"id", "fixture-web3-key-id"}}},
                       {"success", true},
                       {"seenBody", body},
                   };
                 };

                 if (const auto status = body.value("forceStatus", 0); status != 0) {
                   res.status = status;
                   res.set_header("X-Protocol-Trace", "web3-create-http");
                   res.set_content(
                       nlohmann::json{{"error", "denied"},
                                      {"token", body.at("token")},
                                      {"signature", body.at("signature")},
                                      {"apiKey", "SYNTHETIC_WEB3_API_KEY_SECRET"}}
                           .dump(),
                       "application/json");
                   return;
                 }
                 if (body.value("forceWrongMedia", false)) {
                   res.set_content("SYNTHETIC_CHALLENGE_SECRET", "text/plain");
                   return;
                 }
                 if (body.value("forceMalformedJson", false)) {
                   res.set_content("{SYNTHETIC_SIGNATURE_SECRET", "application/json");
                   return;
                 }
                 if (body.value("forceWrongShape", false)) {
                   res.set_content(
                       nlohmann::json{{"data",
                                       {{"token", body.at("token")},
                                        {"signature", body.at("signature")},
                                        {"apiKey", "SYNTHETIC_WEB3_API_KEY_SECRET"}}}}
                           .dump(),
                       "application/json");
                   return;
                 }
                 res.set_content(secret_response().dump(), "application/json");
               });

    // Registered after the exact rate-limit routes so a decoded path cannot
    // turn their suffix into an API-key id.
    m_svr.Get(R"(/api/v1/api_keys/(.*))",
              [this, key_object](const httplib::Request& req, httplib::Response& res) {
                ++m_api_key_hits;
                constexpr std::array<int, 5> kErrorStatuses{400, 401, 402, 429, 500};
                for (const int status : kErrorStatuses) {
                  if (req.target != "/api/v1/api_keys/status-" +
                                        std::to_string(status))
                    continue;
                  res.status = status;
                  res.set_header("X-Protocol-Trace", "api-key-auth-error");
                  res.set_content(R"({"error":"denied"})", "text/plain");
                  return;
                }
                if (req.target == "/api/v1/api_keys/wrong-media") {
                  res.set_header("X-Protocol-Trace", "api-key-wrong-media");
                  res.set_content("not json", "text/plain");
                  return;
                }
                if (req.target == "/api/v1/api_keys/wrong-shape") {
                  res.set_content("[]", "application/json");
                  return;
                }
                res.set_header("X-Test-Response", "api-key-detail");
                res.set_content(
                    nlohmann::json{{"data", key_object(req)}, {"futureEnvelope", true}}.dump(),
                    "application/json");
              });

    // The two catalogue sub-paths (VC-38, #59), registered ahead of
    // /api/v1/models below. Unlike the characters pair further down there is no
    // shadowing hazard to defuse here — "/api/v1/models" carries no regex
    // metacharacter, so httplib registers it as an exact string match and it
    // cannot swallow "/api/v1/models/traits". The ordering is insurance against
    // the day someone turns that route into a pattern, and the reason is written
    // here so a future reader does not conclude the two cases were reasoned
    // about differently.
    //
    // Both handlers echo into `data` itself rather than beside it. That is not
    // laziness: `data` values are strings and therefore land in `entries`, where
    // a test can assert the exact encoded wire target and the Authorization
    // header the client sent through the public typed surface, with no special
    // access to the response.
    m_svr.Get("/api/v1/models/traits",
              [this](const httplib::Request& req, httplib::Response& res) {
                ++m_traits_hits;
                if (req.target.find("type=no-data") != std::string::npos) {
                  // A 200 whose body is the envelope minus its data. The parser
                  // must call this Parse rather than reporting a two-entry map;
                  // see the headline case in test/09catalogue/.
                  res.set_content(R"({"object":"list","type":"text"})", "application/json");
                  return;
                }
                res.set_content(
                    nlohmann::json{
                        {"data",
                         {{"target", req.target},
                          // "present:" prefix, because httplib's
                          // get_header_value returns "" both for a missing
                          // header and for one sent with an empty value — and
                          // telling those apart is the entire point of the
                          // public-auth case below.
                          {"authorization", req.has_header("Authorization")
                                                ? "present:" + req.get_header_value("Authorization")
                                                : std::string{}}}},
                        {"object", "list"},
                        {"type", "text"}}
                        .dump(),
                    "application/json");
              });

    m_svr.Get("/api/v1/models/compatibility_mapping",
              [this](const httplib::Request& req, httplib::Response& res) {
                ++m_compat_hits;
                if (req.target.find("type=all") != std::string::npos) {
                  // The real refusal, captured 2026-08-11. /models/traits accepts
                  // type=all and this operation does not, despite identical
                  // `parameters` blocks in the OpenAPI document — so this branch
                  // is the fixture half of the decision to pass `type` through
                  // rather than validate it in the client. What the caller must
                  // get back is the server's own message naming the accepted set;
                  // a local InvalidArg could not have produced it.
                  res.status = 400;
                  res.set_header("X-Protocol-Trace", "compat-enum-refused");
                  res.set_content(
                      R"({"error":"Invalid request parameters","details":{"_errors":[],)"
                      R"("type":{"_errors":["Invalid enum value. Expected 'asr' | 'embedding' )"
                      R"(| 'image' | 'music' | 'text' | 'tts' | 'upscale' | 'inpaint' | )"
                      R"('video', received 'all'"]}},"issues":[{"received":"all",)"
                      R"("code":"invalid_enum_value","path":["type"]}]})",
                      "application/json");
                  return;
                }
                res.set_content(
                    nlohmann::json{
                        {"data",
                         {{"target", req.target},
                          // "present:" prefix, because httplib's
                          // get_header_value returns "" both for a missing
                          // header and for one sent with an empty value — and
                          // telling those apart is the entire point of the
                          // public-auth case below.
                          {"authorization", req.has_header("Authorization")
                                                ? "present:" + req.get_header_value("Authorization")
                                                : std::string{}}}},
                        {"object", "list"},
                        {"type", "text"}}
                        .dump(),
                    "application/json");
              });

    m_svr.Get("/api/v1/models", [this](const httplib::Request& req, httplib::Response& res) {
      ++m_models_hits;
      note_header_injection(req);
      const nlohmann::json model{{"id", "test-model"},
                                 {"type", "text"},
                                 {"authorization", req.get_header_value("Authorization")},
                                 {"siwx", req.get_header_value("SIGN-IN-WITH-X")},
                                 {"payment", req.get_header_value("PAYMENT-SIGNATURE")}};
      res.set_content(nlohmann::json{{"data", nlohmann::json::array({model})}}.dump(),
                      "application/json");
    });

    m_svr.Post("/api/v1/embeddings",
               [this](const httplib::Request& req, httplib::Response& res) {
                 ++m_embeddings_hits;
                 const auto bearer = req.get_header_value("Authorization");
                 const auto siwx = req.get_header_value("SIGN-IN-WITH-X");
                 if (bearer.empty() && siwx.empty()) {
                   res.status = 401;
                   res.set_content(R"({"error":"missing test authentication"})",
                                   "application/json");
                   return;
                 }
                 if (siwx == "needs-payment") {
                   res.status = 402;
                   res.set_header("PAYMENT-REQUIRED", "embedding-payment-requirements");
                   res.set_content(R"({"code":"PAYMENT_REQUIRED"})", "application/json");
                   return;
                 }
                 const auto body = nlohmann::json::parse(req.body);
                 const auto format = body.value("encoding_format", std::string{"float"});
                 if (format == "unsupported-media") {
                   res.status = 415;
                   res.set_content(R"({"error":"unsupported media"})", "application/json");
                   return;
                 }
                 if (format == "rate-limit") {
                   res.status = 429;
                   res.set_content(R"({"error":"slow down"})", "application/json");
                   return;
                 }

                 nlohmann::json embedding = nlohmann::json::array({0.25, -1, 2.5});
                 if (format == "base64") embedding = "AQIDBA==";
                 if (format == "malformed") embedding = nlohmann::json::array({0.25, "bad"});
                 const nlohmann::json response{
                     {"data", nlohmann::json::array({{{"embedding", std::move(embedding)},
                                                       {"index", 0},
                                                       {"object", "embedding"}}})},
                     {"model", body.at("model")},
                     {"object", "list"},
                     {"usage", {{"prompt_tokens", 3}, {"total_tokens", 3}}},
                     {"seen_body", body},
                     {"seen_authorization", bearer},
                     {"seen_siwx", siwx}};
                 res.set_header("X-Balance-Remaining", "4.230000");
                 res.set_header("X-Protocol-Trace", "embedding-success");
                 res.set_content(response.dump(), "application/json; charset=utf-8");
               });

    m_svr.Post("/api/v1/image/generate",
               [this](const httplib::Request& req, httplib::Response& res) {
                 ++m_native_image_hits;
                 const auto bearer = req.get_header_value("Authorization");
                 const auto siwx = req.get_header_value("SIGN-IN-WITH-X");
                 if (bearer.empty() && siwx.empty()) {
                   res.status = 401;
                   res.set_content(R"({"error":"missing image authentication"})",
                                   "application/json");
                   return;
                 }
                 if (siwx == "image-needs-payment") {
                   res.status = 402;
                   res.set_header("PAYMENT-REQUIRED", "image-payment-requirements");
                   res.set_content(R"({"code":"PAYMENT_REQUIRED"})", "application/json");
                   return;
                 }
                 const auto body = nlohmann::json::parse(req.body);
                 const auto prompt = body.at("prompt").get<std::string>();
                 if (prompt == "stall") {
                   ++m_image_stall_hits;
                   m_gate.wait(kStallCap);
                   res.set_content("{}", "application/json");
                   return;
                 }
                 const auto endpoint_error = [&](int status, const char* message) {
                   res.status = status;
                   res.set_header("X-Protocol-Trace", "image-error");
                   res.set_content(nlohmann::json{{"error", message}}.dump(),
                                   "text/plain");
                 };
                 if (prompt == "bad-request") return endpoint_error(400, "bad image request");
                 if (prompt == "unauthorized") return endpoint_error(401, "unauthorized");
                 if (prompt == "unsupported-media") return endpoint_error(415, "unsupported media");
                 if (prompt == "rate-limit") return endpoint_error(429, "slow down");
                 if (prompt == "capacity") return endpoint_error(503, "at capacity");
                 if (prompt == "wrong-media") {
                   res.set_header("X-Balance-Remaining", "7.500000");
                   res.set_content("not image data", "text/plain");
                   return;
                 }
                 if (prompt == "malformed") {
                   res.set_header("X-Balance-Remaining", "7.500000");
                   res.set_content(R"({"id":"img_bad","images":[],"timing":{}})",
                                   "application/json");
                   return;
                 }

                 res.set_header("X-Balance-Remaining", "7.500000");
                 res.set_header("PAYMENT-RESPONSE", "image-payment-receipt");
                 res.set_header("X-Protocol-Trace", "native-image-success");
                 res.set_header("x-venice-is-blurred", "false");
                 if ((body.value("return_binary", false) &&
                      prompt != "json-despite-media") ||
                     prompt == "media-despite-json") {
                   const std::array bytes{char{'P'}, char{'N'}, char{0},
                                          static_cast<char>(0xFF), char{'G'}};
                   const auto format = body.value("format", std::string{"png"});
                   const char* media_type = format == "jpeg" ? "Image/JPEG; fixture=true"
                                            : format == "webp" ? "Image/WebP; fixture=true"
                                                               : "Image/PNG; fixture=true";
                   res.set_content(bytes.data(), bytes.size(), media_type);
                   return;
                 }

                 const nlohmann::json response{
                     {"id", "img_fixture"},
                     {"images", nlohmann::json::array({"AAEC"})},
                     {"request", body},
                     {"timing",
                      {{"inferenceDuration", 10.5},
                       {"inferencePreprocessingTime", 2},
                       {"inferenceQueueTime", 1.25},
                       {"total", 13.75}}},
                     {"seen_authorization", bearer},
                     {"seen_siwx", siwx},
                     {"seen_content_type", req.get_header_value("Content-Type")}};
                 res.set_content(response.dump(), "application/json; charset=utf-8");
               });

    m_svr.Post("/api/v1/images/generations",
               [this](const httplib::Request& req, httplib::Response& res) {
                 ++m_openai_image_hits;
                 const auto bearer = req.get_header_value("Authorization");
                 const auto siwx = req.get_header_value("SIGN-IN-WITH-X");
                 if (bearer.empty() && siwx.empty()) {
                   res.status = 401;
                   res.set_content(R"({"error":"missing image authentication"})",
                                   "application/json");
                   return;
                 }
                 const auto body = nlohmann::json::parse(req.body);
                 const auto prompt = body.at("prompt").get<std::string>();
                 if (prompt == "malformed") {
                   res.set_content(R"({"created":"now","data":[]})", "application/json");
                   return;
                 }
                 res.set_header("X-Balance-Remaining", "6.250000");
                 res.set_header("X-Protocol-Trace", "openai-image-success");
                 res.set_content(
                     nlohmann::json{
                         {"created", 1786644000},
                         {"data",
                          nlohmann::json::array({{{"b64_json", "AAEC"}},
                                                 {{"url", "data:image/png;base64,AwQF"}}})},
                         {"seen_body", body},
                         {"seen_authorization", bearer},
                         {"seen_siwx", siwx},
                         {"seen_content_type", req.get_header_value("Content-Type")}}
                         .dump(),
                     "application/json; charset=utf-8");
               });

    m_svr.Get("/api/v1/image/styles",
              [this](const httplib::Request& req, httplib::Response& res) {
                ++m_image_styles_hits;
                const bool malformed =
                    req.get_header_value("Authorization") == "Bearer malformed-styles";
                if (malformed) {
                  res.set_content(R"({"object":"list"})", "application/json");
                  return;
                }
                const std::string authorization = req.has_header("Authorization")
                                                      ? "present:" + req.get_header_value("Authorization")
                                                      : std::string{};
                res.set_header("X-Protocol-Trace", "image-styles-success");
                res.set_content(
                    nlohmann::json{{"data", nlohmann::json::array(
                                                {"Anime", 7, "authorization:" + authorization})},
                                   {"object", "list"}}
                        .dump(),
                    "application/json; charset=utf-8");
              });

    const auto transform = [this](const httplib::Request& req,
                                  httplib::Response& res,
                                  std::string_view operation) {
      ++m_image_transform_hits;
      const auto bearer = req.get_header_value("Authorization");
      const auto siwx = req.get_header_value("SIGN-IN-WITH-X");
      if (bearer.empty() && siwx.empty()) {
        res.status = 401;
        res.set_content(R"({"error":"missing transform authentication"})",
                        "application/json");
        return;
      }
      if (siwx == "transform-needs-payment") {
        res.status = 402;
        res.set_header("PAYMENT-REQUIRED", "transform-payment-requirements");
        res.set_content(R"({"code":"PAYMENT_REQUIRED"})", "application/json");
        return;
      }

      CapturedTransform capture;
      capture.path = req.path;
      capture.content_type = req.get_header_value("Content-Type");
      capture.authorization = bearer;
      capture.siwx = siwx;
      capture.body = req.body;
      for (const auto& [name, part] : req.files)
        capture.parts.push_back({name, part.filename, part.content_type, part.content});
      {
        const std::lock_guard<std::mutex> lock{m_transform_mu};
        m_last_transform = std::move(capture);
      }

      std::string control;
      std::string output_format;
      if (req.is_multipart_form_data()) {
        if (const auto it = req.files.find("prompt"); it != req.files.end())
          control = it->second.content;
        if (const auto it = req.files.find("output_format"); it != req.files.end())
          output_format = it->second.content;
      } else {
        const auto body = nlohmann::json::parse(req.body);
        control = body.value("prompt", std::string{});
        output_format = body.value("output_format", std::string{});
      }

      if (control == "stall") {
        ++m_image_transform_stall_hits;
        m_gate.wait(kStallCap);
        res.set_content("late", "image/png");
        return;
      }
      const auto endpoint_error = [&](int status, const char* message) {
        res.status = status;
        res.set_header("X-Protocol-Trace", "transform-error");
        res.set_content(nlohmann::json{{"error", message}}.dump(), "text/plain");
      };
      if (control == "bad-request") return endpoint_error(400, "bad transform request");
      if (control == "unauthorized") return endpoint_error(401, "unauthorized");
      if (control == "unsupported-media") return endpoint_error(415, "unsupported media");
      if (control == "rate-limit") return endpoint_error(429, "slow down");
      if (control == "server-error") return endpoint_error(500, "server error");
      if (control == "capacity") return endpoint_error(503, "at capacity");
      if (control == "wrong-media") {
        res.set_header("X-Balance-Remaining", "5.500000");
        res.set_content("not image data", "text/plain");
        return;
      }

      res.set_header("X-Balance-Remaining", "5.500000");
      res.set_header("PAYMENT-RESPONSE", "transform-payment-receipt");
      res.set_header("X-Protocol-Trace", std::string{operation});
      const std::array bytes{char{'I'}, char{'M'}, char{0},
                             static_cast<char>(0xFF), char{'G'}};
      const char* media_type = output_format == "jpeg" ? "Image/JPEG; fixture=true"
                               : output_format == "webp" ? "Image/WebP; fixture=true"
                                                           : "Image/PNG; fixture=true";
      res.set_content(bytes.data(), bytes.size(), media_type);
    };

    m_svr.Post("/api/v1/image/upscale",
               [transform](const httplib::Request& req, httplib::Response& res) {
                 transform(req, res, "upscale-success");
               });
    m_svr.Post("/api/v1/image/edit",
               [transform](const httplib::Request& req, httplib::Response& res) {
                 transform(req, res, "edit-success");
               });
    m_svr.Post("/api/v1/image/multi-edit",
               [transform](const httplib::Request& req, httplib::Response& res) {
                 transform(req, res, "multi-edit-success");
               });
    m_svr.Post("/api/v1/image/background-remove",
               [transform](const httplib::Request& req, httplib::Response& res) {
                 transform(req, res, "background-remove-success");
               });

    m_svr.Post("/api/v1/audio/speech",
               [this](const httplib::Request& req, httplib::Response& res) {
                 ++m_audio_hits;
                 const auto bearer = req.get_header_value("Authorization");
                 const auto siwx = req.get_header_value("SIGN-IN-WITH-X");
                 if (bearer.empty() && siwx.empty()) {
                   res.status = 401;
                   res.set_content(R"({"error":"missing audio authentication"})",
                                   "application/json");
                   return;
                 }
                 if (siwx == "audio-needs-payment") {
                   res.status = 402;
                   res.set_header("PAYMENT-REQUIRED", "audio-payment-requirements");
                   res.set_content(R"({"code":"PAYMENT_REQUIRED"})", "application/json");
                   return;
                 }
                 const auto body = nlohmann::json::parse(req.body);
                 const auto input = body.at("input").get<std::string>();
                 if (input == "bad-request") {
                   res.status = 400;
                   res.set_content(R"({"error":"bad speech request"})", "text/plain");
                   return;
                 }
                 if (input == "wrong-media") {
                   res.set_content("not audio", "text/plain");
                   return;
                 }

                 res.set_header("X-Balance-Remaining", "3.250000");
                 res.set_header("PAYMENT-RESPONSE", "audio-payment-receipt");
                 res.set_header("X-Protocol-Trace", "speech-success");
                 const auto format = body.value("response_format", std::string{"mp3"});
                 const auto media_type = [&] {
                   if (format == "aac") return std::string{"audio/aac"};
                   if (format == "flac") return std::string{"audio/flac"};
                   if (format == "opus") return std::string{"audio/opus"};
                   if (format == "pcm") return std::string{"audio/pcm"};
                   if (format == "wav") return std::string{"audio/wav"};
                   return std::string{"audio/mpeg"};
                 }();
                 if (!body.at("streaming").get<bool>()) {
                   const std::array bytes{char{'A'}, char{0}, char{'U'}, char{'D'}};
                   res.set_content(bytes.data(), bytes.size(), media_type + "; fixture=true");
                   return;
                 }

                 auto sent = std::make_shared<int>(0);
                 const bool stall = input == "stall";
                 res.set_chunked_content_provider(
                     media_type + "; fixture=true",
                     [this, sent, stall](size_t, httplib::DataSink& sink) {
                       static constexpr std::array<std::string_view, 2> kChunks{
                           std::string_view{"S\0", 2}, std::string_view{"TR", 2}};
                       if (*sent < static_cast<int>(kChunks.size())) {
                         const auto chunk = kChunks[static_cast<std::size_t>(*sent)];
                         ++*sent;
                         return sink.write(chunk.data(), chunk.size());
                       }
                       if (stall) m_gate.wait(kStallCap);
                       sink.done();
                       return true;
                     });
               });

    const auto form_value = [](const httplib::Request& req, const char* name) {
      const auto it = req.files.find(name);
      return it == req.files.end() ? std::string{} : it->second.content;
    };

    m_svr.Post("/api/v1/audio/transcriptions",
               [this, form_value](const httplib::Request& req, httplib::Response& res) {
                 ++m_audio_hits;
                 const auto model = form_value(req, "model");
                 if (model == "payload-too-large") {
                   res.status = 413;
                   res.set_content(R"({"error":"too large"})", "application/json");
                   return;
                 }
                 if (model == "unsupported-media") {
                   res.status = 415;
                   res.set_content(R"({"error":"unsupported"})", "application/json");
                   return;
                 }
                 if (model == "validation-error") {
                   res.status = 422;
                   res.set_content(R"({"error":"empty audio"})", "application/json");
                   return;
                 }
                 if (model == "wrong-media") {
                   res.set_content("wrong", "audio/mpeg");
                   return;
                 }
                 if (model == "malformed") {
                   res.set_content(R"({"text":7})", "application/json");
                   return;
                 }
                 const auto file = req.files.find("file");
                 const auto file_size = file == req.files.end() ? 0U : file->second.content.size();
                 const auto response_format = form_value(req, "response_format");
                 res.set_header("X-Balance-Remaining", "3.000000");
                 if (response_format == "text") {
                   res.set_content("fixture transcript", "text/plain; charset=utf-8");
                   return;
                 }
                 res.set_content(
                     nlohmann::json{
                         {"text", "fixture transcript"},
                         {"duration", 1.25},
                         {"timestamps",
                          {{"word", nlohmann::json::array(
                                        {{{"word", "fixture"}, {"start", 0}, {"end", 1.25}}})}}},
                         {"seen_file_size", file_size},
                         {"seen_filename", file == req.files.end() ? "" : file->second.filename},
                         {"seen_media_type",
                          file == req.files.end() ? "" : file->second.content_type},
                         {"seen_timestamps", form_value(req, "timestamps")},
                         {"seen_language", form_value(req, "language")}}
                         .dump(),
                     "application/json; charset=utf-8");
               });

    m_svr.Post("/api/v1/audio/voices",
               [this, form_value](const httplib::Request& req, httplib::Response& res) {
                 ++m_audio_hits;
                 const auto file = req.files.find("file");
                 const auto model = form_value(req, "model");
                 if (model == "wrong-media") {
                   res.set_content("not json", "text/plain");
                   return;
                 }
                 if (model == "malformed") {
                   res.set_content(R"({"id":7,"model":"malformed"})",
                                   "application/json");
                   return;
                 }
                 res.set_header("X-Balance-Remaining", "2.750000");
                 res.set_content(
                     nlohmann::json{
                         {"id", "vv_fixture"},
                         {"model", model},
                         {"seen_file_size", file == req.files.end() ? 0U : file->second.content.size()},
                         {"seen_filename", file == req.files.end() ? "" : file->second.filename},
                         {"seen_media_type",
                          file == req.files.end() ? "" : file->second.content_type}}
                         .dump(),
                     "application/json");
               });

    m_svr.Post("/api/v1/audio/quote",
               [this](const httplib::Request& req, httplib::Response& res) {
                 ++m_audio_hits;
                 const auto body = nlohmann::json::parse(req.body);
                 if (body.at("model") == "malformed") {
                   res.set_content(R"({"quote":"unknown"})", "application/json");
                   return;
                 }
                 res.set_content(nlohmann::json{{"quote", 0.75},
                                                {"seen_body", body},
                                                {"seen_authorization",
                                                 req.get_header_value("Authorization")},
                                                {"seen_siwx",
                                                 req.get_header_value("SIGN-IN-WITH-X")}}
                                     .dump(),
                                 "application/json");
               });

    m_svr.Post("/api/v1/audio/queue",
               [this](const httplib::Request& req, httplib::Response& res) {
                 ++m_audio_hits;
                 const auto body = nlohmann::json::parse(req.body);
                 if (body.at("model") == "malformed") {
                   res.set_content(R"({"model":"malformed","queue_id":7,"status":"QUEUED"})",
                                   "application/json");
                   return;
                 }
                 res.set_content(nlohmann::json{{"model", body.at("model")},
                                                {"queue_id", "queue-fixture"},
                                                {"status", "QUEUED"},
                                                {"seen_body", body},
                                                {"seen_authorization",
                                                 req.get_header_value("Authorization")},
                                                {"seen_siwx",
                                                 req.get_header_value("SIGN-IN-WITH-X")}}
                                     .dump(),
                                 "application/json");
               });

    m_svr.Post("/api/v1/audio/retrieve",
               [this](const httplib::Request& req, httplib::Response& res) {
                 ++m_audio_hits;
                 const auto body = nlohmann::json::parse(req.body);
                 const auto queue_id = body.at("queue_id").get<std::string>();
                 if (queue_id == "missing") {
                   res.status = 404;
                   res.set_content(R"({"error":"not found"})", "application/json");
                   return;
                 }
                 if (queue_id == "wrong-media") {
                   res.set_content("wrong", "video/mp4");
                   return;
                 }
                 if (queue_id == "malformed") {
                   res.set_content(
                       R"({"status":"PROCESSING","average_execution_time":"soon","execution_duration":1})",
                       "application/json");
                   return;
                 }
                 res.set_header("X-Balance-Remaining", "2.500000");
                 if (queue_id == "media") {
                   const std::array bytes{char{'M'}, char{0}, char{'P'}, char{'3'}};
                   res.set_content(bytes.data(), bytes.size(), "audio/mpeg; fixture=true");
                   return;
                 }
                 res.set_content(nlohmann::json{{"status", "PROCESSING"},
                                                {"average_execution_time", 20000},
                                                {"execution_duration", 5200},
                                                {"seen_body", body}}
                                     .dump(),
                                 "application/json");
               });

    m_svr.Post("/api/v1/audio/complete",
               [this](const httplib::Request& req, httplib::Response& res) {
                 ++m_audio_hits;
                 const auto body = nlohmann::json::parse(req.body);
                 if (body.at("queue_id") == "malformed") {
                   res.set_content(R"({"success":"yes"})", "application/json");
                   return;
                 }
                 res.set_content(nlohmann::json{{"success", body.at("queue_id") != "retry"},
                                                {"seen_body", body}}
                                     .dump(),
                                 "application/json");
               });

    const auto video_error = [](httplib::Response& res, int status,
                                std::string_view message) {
      res.status = status;
      res.set_content(nlohmann::json{{"error", message}}.dump(),
                      "application/json");
    };

    m_svr.Post("/api/v1/video/quote",
               [this](const httplib::Request& req, httplib::Response& res) {
                 ++m_video_hits;
                 const auto body = nlohmann::json::parse(req.body);
                 if (body.at("model") == "bad-request") {
                   res.status = 400;
                   res.set_content(R"({"error":"bad quote"})", "application/json");
                   return;
                 }
                 if (body.at("model") == "forbidden") {
                   res.status = 403;
                   res.set_content(R"({"error":"forbidden"})", "application/json");
                   return;
                 }
                 if (body.at("model") == "invalid-json") {
                   res.set_content("{not-json", "application/json");
                   return;
                 }
                 if (body.at("model") == "malformed") {
                   res.set_content(R"({"quote":"unknown"})", "application/json");
                   return;
                 }
                 res.set_header("X-Balance-Remaining", "2.250000");
                 res.set_content(nlohmann::json{{"quote", 1.25},
                                                {"seen_body", body},
                                                {"seen_authorization",
                                                 req.get_header_value("Authorization")},
                                                {"seen_siwx",
                                                 req.get_header_value("SIGN-IN-WITH-X")}}
                                     .dump(),
                                 "application/json; charset=utf-8");
               });

    m_svr.Post("/api/v1/video/queue",
               [this, video_error](const httplib::Request& req,
                                   httplib::Response& res) {
                 ++m_video_hits;
                 const auto body = nlohmann::json::parse(req.body);
                 const auto control = body.at("prompt").get<std::string>();
                 if (body.at("model") == "invalid-json") {
                   res.set_content("{not-json", "application/json");
                   return;
                 }
                 if (control == "bad-request") return video_error(res, 400, "bad video request");
                 if (control == "unauthorized") return video_error(res, 401, "unauthorized");
                 if (control == "payment") {
                   res.set_header("PAYMENT-REQUIRED", "video-payment-requirements");
                   return video_error(res, 402, "payment required");
                 }
                 if (control == "forbidden") return video_error(res, 403, "forbidden");
                 if (control == "conflict") return video_error(res, 409, "conflict");
                 if (control == "too-large") return video_error(res, 413, "too large");
                 if (control == "invalid") return video_error(res, 422, "invalid video request");
                 if (control == "server-error") return video_error(res, 500, "server error");
                 if (body.at("model") == "malformed") {
                   res.set_content(R"({"model":"malformed","queue_id":7})",
                                   "application/json");
                   return;
                 }
                 auto response = nlohmann::json{{"model", body.at("model")},
                                                 {"queue_id", "video-queue-fixture"},
                                                 {"seen_body", body},
                                                 {"seen_authorization",
                                                  req.get_header_value("Authorization")},
                                                 {"seen_siwx",
                                                  req.get_header_value("SIGN-IN-WITH-X")}};
                 if (control == "download")
                   response["download_url"] = "https://example.test/video.mp4";
                 res.set_header("PAYMENT-RESPONSE", "video-payment-receipt");
                 res.set_content(response.dump(), "application/json");
               });

    m_svr.Post("/api/v1/video/retrieve",
               [this, video_error](const httplib::Request& req,
                                   httplib::Response& res) {
                 ++m_video_hits;
                 const auto body = nlohmann::json::parse(req.body);
                 const auto queue_id = body.at("queue_id").get<std::string>();
                 if (queue_id == "stall") {
                   ++m_video_stall_hits;
                   m_gate.wait(kStallCap);
                 }
                 if (queue_id == "missing") return video_error(res, 404, "not found");
                 if (queue_id == "payment") {
                   res.set_header("PAYMENT-REQUIRED", "video-retrieve-payment");
                   return video_error(res, 402, "payment required");
                 }
                 if (queue_id == "invalid") return video_error(res, 422, "invalid queue");
                 if (queue_id == "capacity") return video_error(res, 503, "capacity");
                 if (queue_id == "wrong-media") {
                   res.set_content("wrong", "audio/mpeg");
                   return;
                 }
                 if (queue_id == "invalid-json") {
                   res.set_content("{not-json", "application/json");
                   return;
                 }
                 if (queue_id == "malformed") {
                   res.set_content(
                       R"({"status":"PROCESSING","average_execution_time":"soon","execution_duration":1})",
                       "application/json");
                   return;
                 }
                 res.set_header("X-Balance-Remaining", "2.000000");
                 if (queue_id == "media") {
                   const std::array bytes{char{'V'}, char{0}, char{'I'}, char{'D'}};
                   res.set_content(bytes.data(), bytes.size(), "Video/MP4; fixture=true");
                   return;
                 }
                 res.set_content(nlohmann::json{{"status", "PROCESSING"},
                                                {"average_execution_time", 30000},
                                                {"execution_duration", 6500},
                                                {"seen_body", body}}
                                     .dump(),
                                 "application/json");
               });

    m_svr.Post("/api/v1/video/complete",
               [this, video_error](const httplib::Request& req,
                                   httplib::Response& res) {
                 ++m_video_hits;
                 const auto body = nlohmann::json::parse(req.body);
                 const auto queue_id = body.at("queue_id").get<std::string>();
                 if (queue_id == "bad-request") return video_error(res, 400, "bad cleanup");
                 if (queue_id == "unauthorized") return video_error(res, 401, "unauthorized");
                 if (queue_id == "payment") {
                   res.set_header("PAYMENT-REQUIRED", "video-cleanup-payment");
                   return video_error(res, 402, "payment required");
                 }
                 if (queue_id == "server-error") return video_error(res, 500, "server error");
                 if (queue_id == "invalid-json") {
                   res.set_content("{not-json", "application/json");
                   return;
                 }
                 if (queue_id == "malformed") {
                   res.set_content(R"({"success":"yes"})", "application/json");
                   return;
                 }
                 res.set_content(nlohmann::json{{"success", queue_id != "retry"},
                                                {"seen_body", body}}
                                     .dump(),
                                 "application/json");
               });

    m_svr.Post("/api/v1/video/transcriptions",
               [this, video_error](const httplib::Request& req,
                                   httplib::Response& res) {
                 ++m_video_hits;
                 const auto body = nlohmann::json::parse(req.body);
                 const auto control = body.at("url").get<std::string>();
                 if (control == "invalid-json") {
                   res.set_content("{not-json", "application/json");
                   return;
                 }
                 if (control == "bad-request") return video_error(res, 400, "bad URL");
                 if (control == "unauthorized") return video_error(res, 401, "unauthorized");
                 if (control == "payment") {
                   res.set_header("PAYMENT-REQUIRED", "video-transcription-payment");
                   return video_error(res, 402, "payment required");
                 }
                 if (control == "forbidden") return video_error(res, 403, "forbidden");
                 if (control == "rate-limited") return video_error(res, 429, "rate limited");
                 if (control == "server-error") return video_error(res, 500, "server error");
                 if (control == "wrong-media") {
                   res.set_content("wrong", "audio/mpeg");
                   return;
                 }
                 if (control == "malformed") {
                   res.set_content(R"({"transcript":7})", "application/json");
                   return;
                 }
                 res.set_header("X-Balance-Remaining", "1.750000");
                 if (body.value("response_format", std::string{}) == "text") {
                   res.set_content("fixture video transcript", "text/plain; charset=utf-8");
                   return;
                 }
                 res.set_content(nlohmann::json{{"transcript", "fixture video transcript"},
                                                {"lang", "en"},
                                                {"seen_body", body},
                                                {"seen_siwx",
                                                 req.get_header_value("SIGN-IN-WITH-X")}}
                                     .dump(),
                                 "application/json");
               });

    const auto augment_error = [](httplib::Response &res, int status,
                                  std::string_view message) {
      res.status = status;
      res.set_header("X-Protocol-Trace", "augment-error");
      res.set_content(nlohmann::json{{"error", message}}.dump(), "text/plain");
    };

    m_svr.Post("/api/v1/augment/text-parser", [this, form_value, augment_error](
                                                  const httplib::Request &req,
                                                  httplib::Response &res) {
      ++m_augment_hits;
      const auto control = form_value(req, "response_format");
      if (control == "stall") {
        ++m_augment_stall_hits;
        m_gate.wait(kStallCap);
      }
      if (control.starts_with("status-")) {
        const int status = std::stoi(control.substr(7));
        if (status == 402)
          res.set_header("PAYMENT-REQUIRED", "augment-payment-requirements");
        return augment_error(res, status, "document parse failed");
      }
      if (control == "wrong-media") {
        res.set_content("wrong", "application/octet-stream");
        return;
      }
      if (control == "invalid-json") {
        res.set_content("{not-json", "application/json");
        return;
      }
      if (control == "malformed") {
        res.set_content(R"({"text":"fixture","tokens":"many"})",
                        "application/json");
        return;
      }

      const auto file = req.files.find("file");
      res.set_header("X-Balance-Remaining", "1.500000");
      res.set_header("PAYMENT-RESPONSE", "augment-payment-receipt");
      if (control == "text") {
        res.set_content(std::string{"fixture\0text", 12},
                        "text/plain; charset=utf-8");
        return;
      }
      res.set_content(
          nlohmann::json{
              {"text", "fixture document"},
              {"tokens", 2.5},
              {"seen_file_size",
               file == req.files.end() ? 0U : file->second.content.size()},
              {"seen_filename",
               file == req.files.end() ? "" : file->second.filename},
              {"seen_media_type",
               file == req.files.end() ? "" : file->second.content_type},
              {"seen_authorization", req.get_header_value("Authorization")},
              {"seen_siwx", req.get_header_value("SIGN-IN-WITH-X")}}
              .dump(),
          "application/json; charset=utf-8");
    });

    m_svr.Post("/api/v1/augment/scrape", [this, augment_error](
                                             const httplib::Request &req,
                                             httplib::Response &res) {
      ++m_augment_hits;
      const auto body = nlohmann::json::parse(req.body);
      const auto control = body.at("url").get<std::string>();
      if (control.starts_with("status-")) {
        const int status = std::stoi(control.substr(7));
        if (status == 402)
          res.set_header("PAYMENT-REQUIRED", "augment-payment-requirements");
        return augment_error(res, status, "scrape failed");
      }
      if (control == "wrong-media") {
        res.set_content("wrong", "text/plain");
        return;
      }
      if (control == "invalid-json") {
        res.set_content("{not-json", "application/json");
        return;
      }
      if (control == "malformed") {
        res.set_content(
            R"({"url":"malformed","content":7,"format":"markdown"})",
            "application/json");
        return;
      }
      res.set_header("X-Balance-Remaining", "1.250000");
      res.set_content(
          nlohmann::json{
              {"url", control},
              {"content", "# Fixture"},
              {"format", "markdown"},
              {"seen_body", body},
              {"seen_authorization", req.get_header_value("Authorization")},
              {"seen_siwx", req.get_header_value("SIGN-IN-WITH-X")}}
              .dump(),
          "application/json");
    });

    m_svr.Post("/api/v1/augment/search", [this, augment_error](
                                             const httplib::Request &req,
                                             httplib::Response &res) {
      ++m_augment_hits;
      const auto body = nlohmann::json::parse(req.body);
      const auto control = body.at("query").get<std::string>();
      if (control.starts_with("status-")) {
        const int status = std::stoi(control.substr(7));
        if (status == 402)
          res.set_header("PAYMENT-REQUIRED", "augment-payment-requirements");
        return augment_error(res, status, "search failed");
      }
      if (control == "wrong-media") {
        res.set_content("wrong", "text/plain");
        return;
      }
      if (control == "invalid-json") {
        res.set_content("{not-json", "application/json");
        return;
      }
      if (control == "malformed") {
        res.set_content(R"({"query":"malformed","results":{}})",
                        "application/json");
        return;
      }
      res.set_header("X-Balance-Remaining", "1.000000");
      res.set_content(
          nlohmann::json{
              {"query", control},
              {"results",
               nlohmann::json::array({{{"title", "Fixture"},
                                       {"url", "https://example.test/result"},
                                       {"content", "result content"},
                                       {"date", "2026-08-27"},
                                       {"future", true}}})},
              {"seen_body", body},
              {"seen_authorization", req.get_header_value("Authorization")},
              {"seen_siwx", req.get_header_value("SIGN-IN-WITH-X")}}
              .dump(),
          "application/json");
    });

    m_svr.Get("/api/v1/crypto/rpc/networks",
              [this](const httplib::Request& req, httplib::Response& res) {
                ++m_crypto_rpc_hits;
                const auto control = req.get_header_value("Idempotency-Key");
                if (control == "wrong-media") {
                  res.set_content("not json", "text/plain");
                  return;
                }
                if (control == "invalid-json") {
                  res.set_content("{not-json", "application/json");
                  return;
                }
                if (control == "malformed") {
                  res.set_content(R"({"networks":"not-an-array"})",
                                  "application/json");
                  return;
                }
                res.set_header("X-Protocol-Trace", "crypto-networks");
                res.set_content(
                    nlohmann::json{
                        {"networks", nlohmann::json::array(
                                         {"ethereum-mainnet", 42,
                                          "solana-mainnet"})},
                        {"seen_authorization",
                         req.get_header_value("Authorization")}}
                        .dump(),
                    "application/json; charset=utf-8");
              });

    m_svr.Post(R"(/api/v1/crypto/rpc/(.*))",
               [this](const httplib::Request& req, httplib::Response& res) {
                 ++m_crypto_rpc_hits;
                 constexpr std::array<int, 5> kErrorStatuses{400, 401, 402, 429,
                                                              500};
                 for (const int status : kErrorStatuses) {
                   if (req.target != "/api/v1/crypto/rpc/status-" +
                                         std::to_string(status))
                     continue;
                   res.status = status;
                   res.set_header("X-Protocol-Trace", "crypto-rpc-error");
                   if (status == 402)
                     res.set_header("PAYMENT-REQUIRED",
                                    "crypto-payment-requirements");
                   res.set_content(R"({"error":"proxy refused"})", "text/plain");
                   return;
                 }
                 if (req.target == "/api/v1/crypto/rpc/stall") {
                   ++m_crypto_rpc_stall_hits;
                   m_gate.wait(kStallCap);
                 }
                 if (req.target == "/api/v1/crypto/rpc/wrong-media") {
                   res.set_content("wrong", "text/plain");
                   return;
                 }
                 if (req.target == "/api/v1/crypto/rpc/invalid-json") {
                   res.set_content("{not-json", "application/json");
                   return;
                 }
                 if (req.target == "/api/v1/crypto/rpc/malformed") {
                   res.set_content(R"({"jsonrpc":"2.0","result":true})",
                                   "application/json");
                   return;
                 }

                 const auto body = nlohmann::json::parse(req.body);
                 const auto response_item = [&](const nlohmann::json& input,
                                                std::size_t index) {
                   auto output = nlohmann::json::object();
                   output["jsonrpc"] = "2.0";
                   output["id"] = input.value("id", nlohmann::json(nullptr));
                   if (index == 1U) {
                     output["error"] =
                         {{"code", -32602}, {"message", "invalid params"}};
                   } else {
                     output["result"] = nlohmann::json{
                         {"seen_target", req.target},
                         {"seen_body", input},
                         {"seen_raw_body", req.body},
                         {"seen_authorization",
                          req.get_header_value("Authorization")},
                         {"seen_siwx", req.get_header_value("SIGN-IN-WITH-X")},
                         {"seen_idempotency",
                          req.get_header_value("Idempotency-Key")}};
                   }
                   return output;
                 };

                 nlohmann::json output;
                 if (body.is_array()) {
                   output = nlohmann::json::array();
                   for (std::size_t i = 0; i < body.size(); ++i)
                     output.push_back(response_item(body.at(i), i));
                 } else if (req.target == "/api/v1/crypto/rpc/rpc-error") {
                   output = nlohmann::json{{"jsonrpc", "2.0"},
                                           {"id", body.value(
                                                      "id", nlohmann::json(nullptr))},
                                           {"error",
                                            {{"code", -32601},
                                             {"message", "method not found"}}}};
                 } else {
                   output = response_item(body, 0U);
                 }
                 res.set_header("X-Balance-Remaining", "4.230000");
                 res.set_header("X-Venice-RPC-Credits", "20");
                 res.set_header("X-Venice-RPC-Cost-USD", "0.00001400");
                 res.set_header("X-Request-ID", "fixture-request-id");
                 res.set_header("Idempotent-Replayed", "true");
                 res.set_content(output.dump(), "application/json");
               });

    const auto x402_error = [](httplib::Response& res, int status) {
      res.status = status;
      res.set_header("X-Protocol-Trace", "x402-error");
      res.set_content(R"({"error":"x402 refused"})", "text/plain");
    };

    m_svr.Get(R"(/api/v1/x402/balance/(.*))",
              [this, x402_error](const httplib::Request& req,
                                 httplib::Response& res) {
                ++m_x402_hits;
                capture_x402(req);
                constexpr std::array<int, 5> kStatuses{400, 401, 403, 429, 500};
                for (const int status : kStatuses) {
                  if (req.target.find("/status-" + std::to_string(status)) !=
                      std::string::npos)
                    return x402_error(res, status);
                }
                if (req.target.find("/stall") != std::string::npos) {
                  ++m_x402_stall_hits;
                  m_gate.wait(kStallCap);
                }
                if (req.target.find("/wrong-media") != std::string::npos) {
                  res.set_content("wrong", "text/plain");
                  return;
                }
                if (req.target.find("/invalid-json") != std::string::npos) {
                  res.set_content("{", "application/json");
                  return;
                }
                if (req.target.find("/malformed") != std::string::npos) {
                  res.set_content(R"({"success":true,"data":{}})",
                                  "application/json");
                  return;
                }
                res.set_header("X-Balance-Remaining", "12.500000");
                res.set_content(
                    nlohmann::json{
                        {"success", true},
                        {"data",
                         {{"walletAddress", "0xnormalized"},
                          {"balanceUsd", 12.5},
                          {"canConsume", true},
                          {"minimumTopUpUsd", 5},
                          {"suggestedTopUpUsd", 10},
                          {"diemBalanceUsd", 2.25}}},
                        {"seen_target", req.target}}
                        .dump(),
                    "application/json; charset=utf-8");
              });

    m_svr.Get(R"(/api/v1/x402/transactions/(.*))",
              [this, x402_error](const httplib::Request& req,
                                 httplib::Response& res) {
                ++m_x402_hits;
                capture_x402(req);
                constexpr std::array<int, 5> kStatuses{400, 401, 403, 429, 500};
                for (const int status : kStatuses) {
                  if (req.target.find("/status-" + std::to_string(status)) !=
                      std::string::npos)
                    return x402_error(res, status);
                }
                if (req.target.find("/wrong-media") != std::string::npos) {
                  res.set_content("wrong", "text/plain");
                  return;
                }
                if (req.target.find("/invalid-json") != std::string::npos) {
                  res.set_content("{", "application/json");
                  return;
                }
                if (req.target.find("/malformed") != std::string::npos) {
                  res.set_content(R"({"success":true,"data":{}})",
                                  "application/json");
                  return;
                }
                res.set_content(
                    nlohmann::json{
                        {"success", true},
                        {"data",
                         {{"walletAddress", "0xnormalized"},
                          {"currentBalance", 12.35},
                          {"transactions",
                           nlohmann::json::array(
                               {{{"id", "ledger-1"},
                                 {"amount", -0.15},
                                 {"balanceAfter", 12.35},
                                 {"type", "CHARGE"},
                                 {"createdAt", "later"},
                                 {"requestId", "request-1"},
                                 {"modelId", "model-1"}}})},
                          {"pagination",
                           {{"limit", 2},
                            {"offset", 1},
                            {"hasMore", false}}}}},
                        {"seen_target", req.target}}
                        .dump(),
                    "application/json");
              });

    m_svr.Post("/api/v1/x402/top-up",
               [this, x402_error](const httplib::Request& req,
                                  httplib::Response& res) {
                 ++m_x402_hits;
                 note_header_injection(req);
                 capture_x402(req);
                 const std::string control =
                     req.get_header_value("Idempotency-Key");
                 for (const int status : {400, 429, 500}) {
                   if (control == "status-" + std::to_string(status))
                     return x402_error(res, status);
                 }
                 if (control == "stall") {
                   ++m_x402_stall_hits;
                   m_gate.wait(kStallCap);
                 }
                 if (control == "wrong-media-402") {
                   res.status = 402;
                   res.set_content("wrong", "text/plain");
                   return;
                 }
                 if (control == "invalid-json-402") {
                   res.status = 402;
                   res.set_content("{", "application/json");
                   return;
                 }
                 if (control == "malformed-402") {
                   res.status = 402;
                   res.set_content(R"({"x402Version":2})", "application/json");
                   return;
                 }
                 if (control == "wrong-media-200") {
                   res.set_content("wrong", "text/plain");
                   return;
                 }
                 if (control == "invalid-json-200") {
                   res.set_content("{", "application/json");
                   return;
                 }
                 if (control == "malformed-200") {
                   res.set_content(R"({"success":true,"data":{}})",
                                   "application/json");
                   return;
                 }

                 if (req.get_header_value("PAYMENT-SIGNATURE").empty()) {
                   res.status = 402;
                   res.set_header("PAYMENT-REQUIRED", "opaque-requirements");
                   res.set_content(
                       nlohmann::json{
                           {"x402Version", 2},
                           {"accepts",
                            {{{"scheme", "exact"},
                              {"network", "eip155:8453"},
                              {"amount", "5000000"},
                              {"asset", "0xasset"},
                              {"payTo", "0xreceiver"},
                              {"maxTimeoutSeconds", 300},
                              {"extra", {{"version", "2"}}}}}},
                           {"fixture", "public-discovery"}}
                           .dump(),
                       "application/json; charset=utf-8");
                   return;
                 }

                 res.set_header("PAYMENT-RESPONSE", "opaque-settlement");
                 res.set_content(
                     nlohmann::json{
                         {"success", true},
                         {"data",
                          {{"walletAddress", "0xnormalized"},
                           {"amountCredited", 10},
                           {"newBalance", 22.5},
                           {"paymentId", "payment-1"}}},
                         {"fixture", "paid-top-up"}}
                         .dump(),
                     "application/json");
               });

    m_svr.Get("/api/v1/billing/balance",
              [this](const httplib::Request& req, httplib::Response& res) {
                ++m_billing_hits;
                res.set_header("X-Test-Response", "billing-balance");
                res.set_content(
                    nlohmann::json{{"canConsume", false},
                                   {"consumptionCurrency", "DIEM"},
                                   {"balances", {{"diem", 0}, {"usd", nullptr}}},
                                   {"diemEpochAllocation", 100},
                                   {"target", req.target},
                                   {"authorization", req.get_header_value("Authorization")}}
                        .dump(),
                    "application/json");
              });

    m_svr.Get("/api/v1/billing/usage-analytics",
              [this](const httplib::Request& req, httplib::Response& res) {
                ++m_billing_hits;
                if (req.target.find("status=504") != std::string::npos) {
                  res.status = 504;
                  res.set_header("X-Protocol-Trace", "billing-timeout");
                  res.set_content(R"({"error":"reduce the lookback"})", "text/plain");
                  return;
                }
                if (req.target.find("wrong-shape") != std::string::npos) {
                  res.set_content("[]", "application/json");
                  return;
                }
                res.set_header("X-Test-Response", "billing-analytics");
                res.set_content(
                    nlohmann::json{{"lookback", "7d"},
                                   {"byDate", nlohmann::json::array()},
                                   {"byModel", nlohmann::json::array()},
                                   {"byModelDaily", nlohmann::json::array()},
                                   {"topModels", nlohmann::json::array()},
                                   {"byKey", nlohmann::json::array()},
                                   {"byKeyDaily", nlohmann::json::array()},
                                   {"topKeyNames", nlohmann::json::array()},
                                   {"target", req.target},
                                   {"authorization", req.get_header_value("Authorization")}}
                        .dump(),
                    "application/json");
              });

    m_svr.Get("/api/v1/billing/usage-history",
              [this](const httplib::Request& req, httplib::Response& res) {
                ++m_billing_hits;
                const auto target = std::string_view{req.target};
                if (target.find("status=400") != std::string_view::npos ||
                    target.find("status=401") != std::string_view::npos ||
                    target.find("status=500") != std::string_view::npos) {
                  const int status = target.find("status=400") != std::string_view::npos
                                         ? 400
                                     : target.find("status=401") != std::string_view::npos ? 401
                                                                                           : 500;
                  res.status = status;
                  res.set_header("X-Protocol-Trace", "billing-history-error");
                  res.set_content(R"({"error":"history failed"})", "text/plain");
                  return;
                }
                if (target.find("wrong-media") != std::string_view::npos) {
                  res.set_header("X-Protocol-Trace", "billing-history-media");
                  res.set_content("not a supported success", "text/plain");
                  return;
                }
                if (target.find("wrong-shape") != std::string_view::npos) {
                  res.set_content(R"({"data":[]})", "application/json");
                  return;
                }

                const bool force_csv = target.find("force=csv") != std::string_view::npos;
                const bool force_json = target.find("force=json") != std::string_view::npos;
                const bool csv = force_csv ||
                                 (!force_json && req.get_header_value("Accept") == "text/csv");
                res.set_header("X-Test-Response", "billing-history");
                if (csv) {
                  res.set_header("X-Next-Cursor", "next_csv_page");
                  res.set_header(
                      "Content-Disposition",
                      "attachment; filename=billing-usage-history-20260816T120000000Z.csv");
                  const std::string body =
                      "amount,currency,notes\r\n-0.25,DIEM,synthetic\r\n";
                  res.set_content(body, "Text/CSV; charset=utf-8");
                  return;
                }

                res.set_content(
                    nlohmann::json{
                        {"data",
                         nlohmann::json::array(
                             {{{"amount", -0.25},
                               {"currency", "DIEM"},
                               {"inferenceDetails", nullptr},
                               {"notes", req.target},
                               {"pricePerUnitUsd", 1.5},
                               {"sku", "fixture"},
                               {"timestamp", "2026-08-16T12:00:00Z"},
                               {"units", 1}}})},
                        {"nextCursor", nullptr},
                        {"accept", req.get_header_value("Accept")},
                        {"authorization", req.get_header_value("Authorization")}}
                        .dump(),
                    "application/json");
              });

    // Registered *before* the catch-all below, and that ordering is the whole
    // reason this route works: httplib matches handlers in registration order,
    // and `/api/v1/characters/(.*)` matches "alan-watts/reviews" perfectly
    // happily. Swap the two and every case here reaches the detail handler.
    //
    // The pattern is `(.*)` rather than `([^/]*)` for a subtler reason worth
    // recording: httplib percent-*decodes* the target into req.path before
    // matching, so a slug this client encoded as `a%2Fb` arrives here already
    // decoded to `a/b` and a segment-shaped pattern would miss it. What went on
    // the wire is req.target, which is what the assertions read.
    m_svr.Get(R"(/api/v1/characters/(.*)/reviews)",
              [this](const httplib::Request& req, httplib::Response& res) {
                ++m_review_hits;
                if (req.target.starts_with("/api/v1/characters/missing/reviews")) {
                  res.status = 404;
                  res.set_header("X-Protocol-Trace", "reviews-not-found");
                  res.set_content(R"({"error":"character not found"})", "application/json");
                  return;
                }
                if (req.target.starts_with("/api/v1/characters/wrong-shape/reviews")) {
                  res.set_content(R"({"object":"list"})", "application/json");
                  return;
                }
                res.set_content(
                    nlohmann::json{
                        {"data", nlohmann::json::array({{{"id", "r1"},
                                                         {"target", req.target},
                                                         {"username", "product_user_42"},
                                                         {"rating", 5}}})},
                        {"object", "list"},
                        {"pagination",
                         {{"page", 1}, {"pageSize", 20}, {"total", 87}, {"totalPages", 5}}},
                        {"summary", {{"averageRating", 4.7}, {"totalReviews", 87}}},
                        {"authorization", req.get_header_value("Authorization")}}
                        .dump(),
                    "application/json");
              });

    m_svr.Get(R"(/api/v1/characters/(.*))",
              [this](const httplib::Request& req, httplib::Response& res) {
                ++m_character_hits;
                if (req.target == "/api/v1/characters/missing") {
                  res.status = 404;
                  res.set_header("X-Protocol-Trace", "character-not-found");
                  res.set_content(R"({"error":"character not found"})", "application/json");
                  return;
                }
                if (req.target == "/api/v1/characters/wrong-shape") {
                  res.set_content("[]", "application/json");
                  return;
                }
                // The envelope the real endpoint sends, measured 2026-08-11
                // (VC-37, #57). It was a bare object here until then, which is
                // why the deliberate-break matrix found this fixture blind to
                // the unwrap: the shape it spoke was one the server does not.
                res.set_content(
                    nlohmann::json{
                        {"data",
                         {{"slug", "fixture"},
                          {"target", req.target},
                          {"authorization", req.get_header_value("Authorization")}}},
                        {"object", "character"}}
                        .dump(),
                    "application/json");
              });

    m_svr.Post("/api/v1/chat/completions",
               [this](const httplib::Request& req, httplib::Response& res) {
                 ++m_chat_hits;
                 note_header_injection(req);
                 const auto bearer = req.get_header_value("Authorization");
                 const auto siwx = req.get_header_value("SIGN-IN-WITH-X");
                 if (bearer.empty() && siwx.empty()) {
                   res.status = 401;
                   res.set_content(R"({"error":"missing test authentication"})",
                                   "application/json");
                   return;
                 }
                 if (siwx == "needs-payment") {
                   res.status = 402;
                   res.set_header("payment-required", "base64-payment-requirements");
                   res.set_header("X-Protocol-Trace", "retained-on-error");
                   res.set_content(
                       R"({"code":"PAYMENT_REQUIRED","message":"top up this wallet"})",
                       "application/json");
                   return;
                 }
                 if (req.get_header_value("Content-Type") != "application/json") {
                   res.status = 415;
                   res.set_content(R"({"error":"wrong content type"})", "application/json");
                   return;
                 }
                 const auto body = nlohmann::json::parse(req.body);
                 res.set_header("x-balance-remaining", "4.230000");
                 res.set_header("X-Protocol-Trace", "retained-on-success");
                 if (!body.at("stream").get<bool>()) {
                   const nlohmann::json response{
                       {"id", "buffered-chat"},
                       {"seen_authorization", bearer},
                       {"seen_siwx", siwx},
                       {"choices", nlohmann::json::array({{{"message",
                                                            {{"role", "assistant"},
                                                             {"content", "ok"}}}}})}};
                   res.set_content(response.dump(), "application/json");
                   return;
                 }
                 // Per-request, not per-server: two tests drive this endpoint,
                 // and a member counter would leave the second one starting
                 // mid-stream. The shared_ptr is what gives the provider — which
                 // outlives this lambda — somewhere to keep it.
                 auto sent = std::make_shared<int>(0);
                 const bool complete_without_stall = siwx == "complete-stream";
                 res.set_chunked_content_provider(
                     "text/event-stream",
                     [this, sent, complete_without_stall](size_t /*offset*/,
                                                          httplib::DataSink& sink) {
                       if (*sent < kFrames) {
                         const std::string frame = std::string{"data: {\"choices\":[{\"delta\":"} +
                                                   "{\"content\":\"" + kDelta[*sent] + "\"}}]}\n\n";
                         ++*sent;
                         return sink.write(frame.data(), frame.size());
                       }
                       if (complete_without_stall) {
                         sink.done();
                         return true;
                       }
                       m_gate.wait(kStallCap);
                       sink.done();
                       return true;
                     });
               });

    m_svr.Post("/api/v1/responses",
               [this](const httplib::Request& req, httplib::Response& res) {
                 ++m_responses_hits;
                 const auto bearer = req.get_header_value("Authorization");
                 const auto siwx = req.get_header_value("SIGN-IN-WITH-X");
                 if (bearer.empty() && siwx.empty()) {
                   res.status = 401;
                   res.set_content(R"({"error":"missing test authentication"})",
                                   "application/json");
                   return;
                 }
                 const auto request = nlohmann::json::parse(req.body);
                 const auto model = request.at("model").get<std::string>();
                 res.set_header("x-balance-remaining", "3.210000");
                 res.set_header("X-Protocol-Trace", "responses-fixture");
                 if (model.starts_with("status-")) {
                   res.status = std::stoi(model.substr(7));
                   res.set_content(R"({"error":"responses refused"})", "text/plain");
                   return;
                 }
                 if (model == "wrong-media") {
                   res.set_content("not json", "text/plain");
                   return;
                 }
                 if (model == "invalid-json") {
                   res.set_content("{", "application/json");
                   return;
                 }
                 if (model == "malformed") {
                   res.set_content(R"({"id":"resp_1"})", "application/json");
                   return;
                 }
                 res.set_content(
                     nlohmann::json{{"id", "resp_1"},
                                    {"object", "response"},
                                    {"created_at", 10},
                                    {"model", model},
                                    {"status", "completed"},
                                    {"output",
                                     nlohmann::json::array(
                                         {{{"type", "message"},
                                           {"id", "msg_1"},
                                           {"status", "completed"},
                                           {"role", "assistant"},
                                           {"content",
                                            nlohmann::json::array(
                                                {{{"type", "output_text"},
                                                  {"text", "ok"},
                                                  {"annotations", nlohmann::json::array()}}})}}})},
                                    {"seen_stream", request.at("stream")},
                                    {"seen_authorization", bearer},
                                    {"seen_siwx", siwx}}
                         .dump(),
                     "application/json");
               });

    m_port = m_svr.bind_to_any_port("127.0.0.1");
    m_thread = std::thread{[this] { m_svr.listen_after_bind(); }};
    m_svr.wait_until_ready();
  }

  ~TestServer() {
    m_gate.open();  // release every stalled handler before asking listen() to return
    m_svr.stop();
    m_thread.join();
  }

  TestServer(const TestServer&) = delete;
  auto operator=(const TestServer&) -> TestServer& = delete;
  TestServer(TestServer&&) = delete;
  auto operator=(TestServer&&) -> TestServer& = delete;

  [[nodiscard]] auto base_url() const -> std::string {
    return "http://127.0.0.1:" + std::to_string(m_port) + "/api/v1";
  }

  [[nodiscard]] auto stall_hits() const -> int { return m_stall_hits.load(); }
  [[nodiscard]] auto models_hits() const -> int { return m_models_hits.load(); }
  [[nodiscard]] auto chat_hits() const -> int { return m_chat_hits.load(); }
  [[nodiscard]] auto responses_hits() const -> int { return m_responses_hits.load(); }
  [[nodiscard]] auto character_hits() const -> int { return m_character_hits.load(); }
  [[nodiscard]] auto review_hits() const -> int { return m_review_hits.load(); }
  [[nodiscard]] auto billing_hits() const -> int { return m_billing_hits.load(); }
  [[nodiscard]] auto api_key_hits() const -> int { return m_api_key_hits.load(); }
  [[nodiscard]] auto web3_api_key_hits() const -> int {
    return m_web3_api_key_hits.load();
  }
  [[nodiscard]] auto traits_hits() const -> int { return m_traits_hits.load(); }
  [[nodiscard]] auto compat_hits() const -> int { return m_compat_hits.load(); }
  [[nodiscard]] auto embeddings_hits() const -> int { return m_embeddings_hits.load(); }
  [[nodiscard]] auto native_image_hits() const -> int { return m_native_image_hits.load(); }
  [[nodiscard]] auto openai_image_hits() const -> int { return m_openai_image_hits.load(); }
  [[nodiscard]] auto image_styles_hits() const -> int { return m_image_styles_hits.load(); }
  [[nodiscard]] auto image_stall_hits() const -> int { return m_image_stall_hits.load(); }
  [[nodiscard]] auto image_transform_hits() const -> int {
    return m_image_transform_hits.load();
  }
  [[nodiscard]] auto image_transform_stall_hits() const -> int {
    return m_image_transform_stall_hits.load();
  }
  [[nodiscard]] auto audio_hits() const -> int { return m_audio_hits.load(); }
  [[nodiscard]] auto video_hits() const -> int { return m_video_hits.load(); }
  [[nodiscard]] auto video_stall_hits() const -> int {
    return m_video_stall_hits.load();
  }
  [[nodiscard]] auto augment_hits() const -> int {
    return m_augment_hits.load();
  }
  [[nodiscard]] auto augment_stall_hits() const -> int {
    return m_augment_stall_hits.load();
  }
  [[nodiscard]] auto crypto_rpc_hits() const -> int {
    return m_crypto_rpc_hits.load();
  }
  [[nodiscard]] auto crypto_rpc_stall_hits() const -> int {
    return m_crypto_rpc_stall_hits.load();
  }
  [[nodiscard]] auto x402_hits() const -> int { return m_x402_hits.load(); }
  [[nodiscard]] auto x402_stall_hits() const -> int {
    return m_x402_stall_hits.load();
  }
  [[nodiscard]] auto header_injection_hits() const -> int {
    return m_header_injection_hits.load();
  }
  [[nodiscard]] auto last_transform() const -> CapturedTransform {
    const std::lock_guard<std::mutex> lock{m_transform_mu};
    return m_last_transform;
  }
  [[nodiscard]] auto last_web3() const -> CapturedWeb3Request {
    const std::lock_guard<std::mutex> lock{m_web3_mu};
    return m_last_web3;
  }
  [[nodiscard]] auto last_x402() const -> CapturedX402Request {
    const std::lock_guard<std::mutex> lock{m_x402_mu};
    return m_last_x402;
  }
  [[nodiscard]] auto multipart_stall_hits() const -> int { return m_multipart_stall_hits.load(); }

  static constexpr int kFrames = 2;
  static constexpr const char* kDelta[kFrames] = {"hel", "lo"};

 private:
  void note_header_injection(const httplib::Request& req) {
    if (req.has_header("X-Injected")) ++m_header_injection_hits;
  }

  void capture_web3(const httplib::Request& req) {
    const std::lock_guard<std::mutex> lock{m_web3_mu};
    m_last_web3 = CapturedWeb3Request{
        .method = req.method,
        .path = req.target,
        .content_type = req.get_header_value("Content-Type"),
        .authorization = req.get_header_value("Authorization"),
        .siwx = req.get_header_value("SIGN-IN-WITH-X"),
        .payment = req.get_header_value("PAYMENT-SIGNATURE"),
        .body = req.body,
    };
  }

  void capture_x402(const httplib::Request& req) {
    const std::lock_guard<std::mutex> lock{m_x402_mu};
    m_last_x402 = CapturedX402Request{
        .method = req.method,
        .path = req.target,
        .content_type = req.get_header_value("Content-Type"),
        .authorization = req.get_header_value("Authorization"),
        .siwx = req.get_header_value("SIGN-IN-WITH-X"),
        .payment = req.get_header_value("PAYMENT-SIGNATURE"),
        .body = req.body,
    };
  }

  static constexpr auto kStallCap = 10s;

  httplib::Server m_svr;
  std::thread m_thread;
  Gate m_gate;
  int m_port = 0;
  std::atomic<int> m_stall_hits{0};
  std::atomic<int> m_models_hits{0};
  std::atomic<int> m_chat_hits{0};
  std::atomic<int> m_responses_hits{0};
  std::atomic<int> m_character_hits{0};
  std::atomic<int> m_review_hits{0};
  std::atomic<int> m_billing_hits{0};
  std::atomic<int> m_api_key_hits{0};
  std::atomic<int> m_web3_api_key_hits{0};
  std::atomic<int> m_traits_hits{0};
  std::atomic<int> m_compat_hits{0};
  std::atomic<int> m_embeddings_hits{0};
  std::atomic<int> m_native_image_hits{0};
  std::atomic<int> m_openai_image_hits{0};
  std::atomic<int> m_image_styles_hits{0};
  std::atomic<int> m_image_stall_hits{0};
  std::atomic<int> m_image_transform_hits{0};
  std::atomic<int> m_image_transform_stall_hits{0};
  std::atomic<int> m_audio_hits{0};
  std::atomic<int> m_video_hits{0};
  std::atomic<int> m_video_stall_hits{0};
  std::atomic<int> m_augment_hits{0};
  std::atomic<int> m_augment_stall_hits{0};
  std::atomic<int> m_crypto_rpc_hits{0};
  std::atomic<int> m_crypto_rpc_stall_hits{0};
  std::atomic<int> m_x402_hits{0};
  std::atomic<int> m_x402_stall_hits{0};
  std::atomic<int> m_header_injection_hits{0};
  std::atomic<int> m_multipart_stall_hits{0};
  mutable std::mutex m_transform_mu;
  CapturedTransform m_last_transform{};
  Web3ChallengeMode m_web3_challenge_mode;
  mutable std::mutex m_web3_mu;
  CapturedWeb3Request m_last_web3{};
  mutable std::mutex m_x402_mu;
  CapturedX402Request m_last_x402{};
};

// What the catalogue fixture echoed under `key`, or a marker naming what was
// missing. find() returns nullptr for an absent key, so a REQUIRE that
// dereferences it directly *crashes* the run instead of failing it — and the
// key going absent is exactly the regression these cases exist to catch, so it
// is the one failure mode that must stay readable.
template <typename Result>
auto echoed(const Result& res, std::string_view key) -> std::string {
  const std::string* value = res.find(key);
  return value != nullptr ? *value : "(absent: " + std::string{key} + ")";
}

auto minimal_chat() -> ChatRequest {
  ChatRequest r;
  r.model = "test-model";
  r.messages = {Message::user("hi")};
  return r;
}

auto minimal_response() -> ResponsesRequest {
  ResponsesRequest request;
  request.model = "response-test";
  request.input = venice::responses_input::text("hello");
  return request;
}

auto minimal_embedding() -> EmbeddingRequest {
  EmbeddingRequest request;
  request.model = "text-embedding-test";
  request.input = venice::embedding_input::text("hello");
  return request;
}

auto minimal_native_image() -> ImageGenerationRequest {
  ImageGenerationRequest request;
  request.model = "image-test";
  request.prompt = "paint it";
  return request;
}

auto minimal_openai_image() -> OpenAIImageGenerationRequest {
  OpenAIImageGenerationRequest request;
  request.prompt = "paint it";
  return request;
}

auto minimal_upscale() -> ImageUpscaleRequest {
  ImageUpscaleRequest request;
  request.image = venice::image_input::base64("AAEC");
  return request;
}

auto minimal_image_edit() -> ImageEditRequest {
  ImageEditRequest request;
  request.image = venice::image_input::base64("AAEC");
  request.prompt = "paint it";
  return request;
}

auto minimal_multi_edit() -> MultiImageEditRequest {
  MultiImageEditRequest request;
  request.images = {venice::image_input::base64("BASE"),
                    venice::image_input::url("https://example.test/layer.png")};
  request.prompt = "combine them";
  return request;
}

auto minimal_background_removal() -> ImageBackgroundRemovalRequest {
  ImageBackgroundRemovalRequest request;
  request.image = venice::image_input::url("https://example.test/source.png");
  return request;
}

auto parts_named(const CapturedTransform& capture, std::string_view name)
    -> std::vector<CapturedTransformPart> {
  std::vector<CapturedTransformPart> result;
  for (const auto& part : capture.parts)
    if (part.name == name) result.push_back(part);
  return result;
}

// Elapsed wall time around a call, for the one-sided bounds described above.
template <typename F>
auto timed(F&& fn) -> std::chrono::milliseconds {
  const auto start = std::chrono::steady_clock::now();
  std::forward<F>(fn)();
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                               start);
}

// The bound every cancellation case is measured against. Three orders of
// magnitude below the 300s default it is standing in for.
constexpr auto kPromptly = 5000ms;

// ── authentication/payment foundation: failure matrix first (VC-23) ─────

TEST_CASE("endpoint authentication policies reject impossible modes before the socket",
          "[transport][auth][failure]") {
  const TestServer server;
  const Client public_client{Authentication::public_access(), server.base_url()};

  const auto public_chat = public_client.chat(minimal_chat());
  REQUIRE_FALSE(public_chat.has_value());
  REQUIRE(public_chat.error().kind == ErrorKind::InvalidArg);
  REQUIRE(server.chat_hits() == 0);

  const auto public_embeddings = public_client.embeddings(minimal_embedding());
  REQUIRE_FALSE(public_embeddings.has_value());
  REQUIRE(public_embeddings.error().kind == ErrorKind::InvalidArg);
  REQUIRE(server.embeddings_hits() == 0);

  const auto public_native_image = public_client.generate_image(minimal_native_image());
  REQUIRE_FALSE(public_native_image.has_value());
  REQUIRE(public_native_image.error().kind == ErrorKind::InvalidArg);
  REQUIRE(server.native_image_hits() == 0);

  const auto public_openai_image =
      public_client.generate_image_openai(minimal_openai_image());
  REQUIRE_FALSE(public_openai_image.has_value());
  REQUIRE(public_openai_image.error().kind == ErrorKind::InvalidArg);
  REQUIRE(server.openai_image_hits() == 0);

  const auto public_characters = public_client.characters();
  REQUIRE_FALSE(public_characters.has_value());
  REQUIRE(public_characters.error().kind == ErrorKind::InvalidArg);

  const auto public_balance = public_client.balance();
  REQUIRE_FALSE(public_balance.has_value());
  REQUIRE(public_balance.error().kind == ErrorKind::InvalidArg);
  REQUIRE(server.stall_hits() == 0);

  const Client siwx_client{Authentication::sign_in_with_x("signed-wallet"), server.base_url()};
  const auto siwx_models = siwx_client.models();
  REQUIRE_FALSE(siwx_models.has_value());
  REQUIRE(siwx_models.error().kind == ErrorKind::InvalidArg);
  REQUIRE(server.models_hits() == 0);

  const Client payment_client{Authentication::x402_payment("payment-payload"),
                              server.base_url()};
  const auto payment_chat = payment_client.chat(minimal_chat());
  REQUIRE_FALSE(payment_chat.has_value());
  REQUIRE(payment_chat.error().kind == ErrorKind::InvalidArg);
  REQUIRE(payment_chat.error().message.find("payment-payload") == std::string::npos);
  REQUIRE(payment_chat.error().body.empty());
  REQUIRE(payment_chat.error().metadata.headers.empty());
  REQUIRE(server.chat_hits() == 0);

  const auto payment_embeddings = payment_client.embeddings(minimal_embedding());
  REQUIRE_FALSE(payment_embeddings.has_value());
  REQUIRE(payment_embeddings.error().kind == ErrorKind::InvalidArg);
  REQUIRE(server.embeddings_hits() == 0);

  const Client empty_bearer{"", server.base_url()};
  const auto empty_models = empty_bearer.models();
  REQUIRE_FALSE(empty_models.has_value());
  REQUIRE(empty_models.error().kind == ErrorKind::InvalidArg);
  REQUIRE(server.models_hits() == 0);
}

TEST_CASE("caller-controlled header values cannot inject another field",
          "[transport][auth][failure]") {
  const TestServer server;
  const std::string injected = "synthetic-secret\r\nX-Injected: reached-wire";

  const auto require_rejected = [&](const auto& result) {
    REQUIRE(server.header_injection_hits() == 0);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind == ErrorKind::InvalidArg);
    REQUIRE(result.error().status == 0);
    REQUIRE(result.error().message.find("synthetic-secret") == std::string::npos);
    REQUIRE(result.error().body.empty());
    REQUIRE(result.error().metadata.headers.empty());
  };

  SECTION("client-default Bearer") {
    const Client client{injected, server.base_url()};
    const auto result = client.models();
    require_rejected(result);
    REQUIRE(server.models_hits() == 0);
  }

  SECTION("per-call Bearer override") {
    const Client client{"safe-default", server.base_url()};
    const auto result = client.models(
        {}, {.authentication = Authentication::bearer(injected)});
    require_rejected(result);
    REQUIRE(server.models_hits() == 0);
  }

  SECTION("SIWX proof") {
    const Client client{Authentication::sign_in_with_x(injected),
                        server.base_url()};
    const auto result = client.chat(minimal_chat());
    require_rejected(result);
    REQUIRE(server.chat_hits() == 0);
  }

  SECTION("x402 payment signature") {
    const Client client{Authentication::x402_payment(injected),
                        server.base_url()};
    const auto result = client.x402_top_up();
    require_rejected(result);
    REQUIRE(server.x402_hits() == 0);
  }

  SECTION("idempotency key") {
    const Client client{Authentication::public_access(), server.base_url()};
    const auto result = client.models({}, {.idempotency_key = injected});
    require_rejected(result);
    REQUIRE(server.models_hits() == 0);
  }
}

TEST_CASE("forbidden HTTP field controls fail before every caller-owned header path",
          "[transport][auth][failure]") {
  const TestServer server;
  std::vector<unsigned int> forbidden;
  for (unsigned int byte = 0; byte < 0x20U; ++byte)
    if (byte != static_cast<unsigned int>('\t')) forbidden.push_back(byte);
  forbidden.push_back(0x7FU);

  const auto require_invalid = [](const auto& result) {
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind == ErrorKind::InvalidArg);
    REQUIRE(result.error().status == 0);
    REQUIRE(result.error().message.find("synthetic-secret") == std::string::npos);
    REQUIRE(result.error().body.empty());
    REQUIRE(result.error().metadata.headers.empty());
  };

  for (const unsigned int byte : forbidden) {
    CAPTURE(byte);
    std::string value = "synthetic-secret";
    value.push_back(static_cast<char>(byte));
    value += "tail";

    const Client bearer{value, server.base_url()};
    require_invalid(bearer.models());

    const Client override_client{"safe-default", server.base_url()};
    require_invalid(override_client.models(
        {}, {.authentication = Authentication::bearer(value)}));

    const Client siwx{Authentication::sign_in_with_x(value), server.base_url()};
    require_invalid(siwx.chat(minimal_chat()));

    const Client payment{Authentication::x402_payment(value), server.base_url()};
    require_invalid(payment.x402_top_up());

    const Client public_client{Authentication::public_access(), server.base_url()};
    require_invalid(public_client.models({}, {.idempotency_key = value}));
  }

  REQUIRE(server.models_hits() == 0);
  REQUIRE(server.chat_hits() == 0);
  REQUIRE(server.x402_hits() == 0);
  REQUIRE(server.header_injection_hits() == 0);
}

TEST_CASE("HTTP field validation preserves the open value syntax",
          "[transport][auth]") {
  std::string allowed(1, '\t');
  for (unsigned int byte = 0x20U; byte <= 0xFFU; ++byte)
    if (byte != 0x7FU) allowed.push_back(static_cast<char>(byte));

  REQUIRE(venice::detail::validate_http_field_value({}, "test field").has_value());
  REQUIRE(venice::detail::validate_http_field_value(allowed, "test field").has_value());

  const auto bearer = venice::detail::authentication_headers(
      Authentication::bearer(allowed));
  REQUIRE(bearer.has_value());
  REQUIRE(bearer->find("Authorization") != bearer->end());
  REQUIRE(bearer->find("Authorization")->second == "Bearer " + allowed);
}

TEST_CASE("all four authentication modes traverse the buffered fixture independently",
          "[transport][auth]") {
  const TestServer server;
  struct Case {
    Authentication authentication;
    std::string authorization;
    std::string siwx;
    std::string payment;
  };
  const std::vector<Case> cases{
      {Authentication::public_access(), "", "", ""},
      {Authentication::bearer("bearer-value"), "Bearer bearer-value", "", ""},
      {Authentication::sign_in_with_x("siwx-value"), "", "siwx-value", ""},
      {Authentication::x402_payment("payment-value"), "", "", "payment-value"},
  };

  for (const auto& test : cases) {
    const auto headers = venice::detail::authentication_headers(test.authentication);
    REQUIRE(headers.has_value());
    const auto response = venice::detail::send_buffered(
        server.base_url(),
        {.method = venice::detail::HttpMethod::Get,
         .endpoint = "/transport/echo",
         .headers = *headers});
    REQUIRE(response.has_value());
    const auto body = venice::detail::decode_json(*response);
    REQUIRE(body.has_value());
    REQUIRE((*body)["authorization"] == test.authorization);
    REQUIRE((*body)["siwx"] == test.siwx);
    REQUIRE((*body)["payment"] == test.payment);
  }
}

TEST_CASE("embeddings posts exact JSON with Bearer auth and keeps response metadata",
          "[transport][embeddings]") {
  const TestServer server;
  const Client client{"default-token", server.base_url()};
  auto request = minimal_embedding();
  request.input = venice::embedding_input::texts({"one", "two"});
  request.dimensions = 7;
  request.encoding_format = "float";
  request.user = "fixture-user";

  const auto response = client.embeddings(request);
  REQUIRE(response.has_value());
  REQUIRE(server.embeddings_hits() == 1);
  REQUIRE(response->raw["seen_authorization"] == "Bearer default-token");
  REQUIRE(response->raw["seen_siwx"] == "");
  REQUIRE(response->raw["seen_body"] == request.to_json_body());
  REQUIRE(response->raw["seen_body"]["input"] == nlohmann::json::array({"one", "two"}));
  REQUIRE(response->metadata.x_balance_remaining == "4.230000");
  REQUIRE(response->metadata.header("x-protocol-trace") == "embedding-success");
  REQUIRE(std::holds_alternative<std::vector<double>>(response->data.front().value));
}

TEST_CASE("embeddings supports SIWX and keeps base64 opaque", "[transport][embeddings][auth]") {
  const TestServer server;
  const Client client{"default-token", server.base_url()};
  auto request = minimal_embedding();
  request.encoding_format = "base64";

  const auto response = client.embeddings(
      request, {.authentication = Authentication::sign_in_with_x("signed-wallet")});
  REQUIRE(response.has_value());
  REQUIRE(response->raw["seen_authorization"] == "");
  REQUIRE(response->raw["seen_siwx"] == "signed-wallet");
  const auto* encoded = std::get_if<std::string>(&response->data.front().value);
  REQUIRE(encoded != nullptr);
  REQUIRE(*encoded == "AQIDBA==");
}

TEST_CASE("embeddings preserves endpoint errors and rejects malformed success data",
          "[transport][embeddings][failure]") {
  const TestServer server;
  const Client client{"default-token", server.base_url()};

  auto request = minimal_embedding();
  request.encoding_format = "unsupported-media";
  const auto unsupported = client.embeddings(request);
  REQUIRE_FALSE(unsupported.has_value());
  REQUIRE(unsupported.error().kind == ErrorKind::Http);
  REQUIRE(unsupported.error().status == 415);
  REQUIRE(unsupported.error().body == R"({"error":"unsupported media"})");

  request.encoding_format = "rate-limit";
  const auto limited = client.embeddings(request);
  REQUIRE_FALSE(limited.has_value());
  REQUIRE(limited.error().kind == ErrorKind::RateLimited);
  REQUIRE(limited.error().status == 429);

  request.encoding_format = "malformed";
  const auto malformed = client.embeddings(request);
  REQUIRE_FALSE(malformed.has_value());
  REQUIRE(malformed.error().kind == ErrorKind::Parse);
  REQUIRE(malformed.error().status == 200);
  REQUIRE(malformed.error().message.starts_with("embeddings parse: embeddings:"));
  REQUIRE(malformed.error().metadata.x_balance_remaining == "4.230000");

  const Client wallet{Authentication::sign_in_with_x("needs-payment"), server.base_url()};
  request.encoding_format = "float";
  const auto payment = wallet.embeddings(request);
  REQUIRE_FALSE(payment.has_value());
  REQUIRE(payment.error().kind == ErrorKind::PaymentRequired);
  REQUIRE(payment.error().status == 402);
  REQUIRE(payment.error().metadata.payment_required == "embedding-payment-requirements");
  REQUIRE(server.embeddings_hits() == 4);
}

TEST_CASE("native image generation posts exact JSON and keeps a typed JSON response",
          "[transport][images]") {
  const TestServer server;
  const Client client{"default-token", server.base_url()};
  auto request = minimal_native_image();
  request.cfg_scale = 6.5;
  request.format = "webp";
  request.style_references = std::vector<venice::ImageStyleReference>{
      {.image = "data:image/png;base64,AAEC", .strength = 0.75}};
  const nlohmann::json expected_body{
      {"model", "image-test"},
      {"prompt", "paint it"},
      {"cfg_scale", 6.5},
      {"format", "webp"},
      {"style_references",
       nlohmann::json::array({{{"image", "data:image/png;base64,AAEC"},
                               {"strength", 0.75}}})}};

  const auto result = client.generate_image(request);
  REQUIRE(result.has_value());
  REQUIRE(server.native_image_hits() == 1);
  const auto* response =
      std::get_if<venice::NativeImageGenerationResponse>(&*result);
  REQUIRE(response != nullptr);
  REQUIRE(response->id == "img_fixture");
  REQUIRE(response->images == std::vector<std::string>{"AAEC"});
  REQUIRE(response->request == expected_body);
  REQUIRE(response->raw["seen_authorization"] == "Bearer default-token");
  REQUIRE(response->raw["seen_siwx"] == "");
  REQUIRE(response->raw["seen_content_type"] == "application/json");
  REQUIRE(response->metadata.x_balance_remaining == "7.500000");
  REQUIRE(response->metadata.payment_response == "image-payment-receipt");
  REQUIRE(response->metadata.header("x-protocol-trace") == "native-image-success");
  REQUIRE(response->metadata.header("x-venice-is-blurred") == "false");
}

TEST_CASE("native image generation routes actual image media and preserves NUL bytes",
          "[transport][images][binary][auth]") {
  const TestServer server;
  const Client client{"default-token", server.base_url()};
  auto request = minimal_native_image();
  request.return_binary = true;
  request.format = "png";

  const auto result = client.generate_image(
      request, {.authentication = Authentication::sign_in_with_x("signed-wallet")});
  REQUIRE(result.has_value());
  const auto* media = std::get_if<venice::GeneratedImageMedia>(&*result);
  REQUIRE(media != nullptr);
  REQUIRE(media->media_type == "image/png");
  REQUIRE(media->bytes ==
          std::string{{'P', 'N', char{0}, static_cast<char>(0xFF), 'G'}});
  REQUIRE(media->metadata.x_balance_remaining == "7.500000");
  REQUIRE(media->metadata.payment_response == "image-payment-receipt");
  REQUIRE(media->metadata.header("x-protocol-trace") == "native-image-success");
}

TEST_CASE("native image generation trusts actual media over the request hint",
          "[transport][images][binary]") {
  const TestServer server;
  const Client client{"default-token", server.base_url()};

  auto request = minimal_native_image();
  request.return_binary = true;
  for (const auto& [format, expected_type] :
       std::array{std::pair{"jpeg", "image/jpeg"},
                  std::pair{"webp", "image/webp"}}) {
    request.format = format;
    const auto result = client.generate_image(request);
    REQUIRE(result.has_value());
    const auto* media = std::get_if<venice::GeneratedImageMedia>(&*result);
    REQUIRE(media != nullptr);
    REQUIRE(media->media_type == expected_type);
  }

  request.prompt = "media-despite-json";
  request.return_binary = false;
  const auto media_result = client.generate_image(request);
  REQUIRE(media_result.has_value());
  REQUIRE(std::holds_alternative<venice::GeneratedImageMedia>(*media_result));

  request.prompt = "json-despite-media";
  request.return_binary = true;
  const auto json_result = client.generate_image(request);
  REQUIRE(json_result.has_value());
  REQUIRE(std::holds_alternative<venice::NativeImageGenerationResponse>(*json_result));
}

TEST_CASE("native image generation classifies statuses before success media",
          "[transport][images][failure]") {
  const TestServer server;
  const Client client{"default-token", server.base_url()};

  struct Case {
    const char* prompt;
    ErrorKind kind;
    int status;
  };
  const std::array cases{
      Case{"bad-request", ErrorKind::Http, 400},
      Case{"unauthorized", ErrorKind::Auth, 401},
      Case{"unsupported-media", ErrorKind::Http, 415},
      Case{"rate-limit", ErrorKind::RateLimited, 429},
      Case{"capacity", ErrorKind::Http, 503},
  };
  for (const auto& test : cases) {
    auto request = minimal_native_image();
    request.prompt = test.prompt;
    const auto result = client.generate_image(request);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind == test.kind);
    REQUIRE(result.error().status == test.status);
    REQUIRE(result.error().metadata.header("x-protocol-trace") == "image-error");
  }

  auto request = minimal_native_image();
  request.prompt = "wrong-media";
  const auto wrong_media = client.generate_image(request);
  REQUIRE_FALSE(wrong_media.has_value());
  REQUIRE(wrong_media.error().kind == ErrorKind::Parse);
  REQUIRE(wrong_media.error().status == 200);
  REQUIRE(wrong_media.error().body == "not image data");
  REQUIRE(wrong_media.error().metadata.x_balance_remaining == "7.500000");

  request.prompt = "malformed";
  const auto malformed = client.generate_image(request);
  REQUIRE_FALSE(malformed.has_value());
  REQUIRE(malformed.error().kind == ErrorKind::Parse);
  REQUIRE(malformed.error().status == 200);
  REQUIRE(malformed.error().message.starts_with(
      "image generation parse: image generation timing:"));

  const Client wallet{Authentication::sign_in_with_x("image-needs-payment"),
                      server.base_url()};
  request.prompt = "paint it";
  const auto payment = wallet.generate_image(request);
  REQUIRE_FALSE(payment.has_value());
  REQUIRE(payment.error().kind == ErrorKind::PaymentRequired);
  REQUIRE(payment.error().status == 402);
  REQUIRE(payment.error().metadata.payment_required == "image-payment-requirements");
}

TEST_CASE("OpenAI image generation uses its own path body and response shape",
          "[transport][images][openai]") {
  const TestServer server;
  const Client client{"default-token", server.base_url()};
  auto request = minimal_openai_image();
  request.model = "image-test";
  request.output_format = "png";
  request.response_format = "b64_json";
  const nlohmann::json expected_body{{"prompt", "paint it"},
                                     {"model", "image-test"},
                                     {"output_format", "png"},
                                     {"response_format", "b64_json"}};

  const auto response = client.generate_image_openai(
      request, {.authentication = Authentication::sign_in_with_x("signed-wallet")});
  REQUIRE(response.has_value());
  REQUIRE(server.openai_image_hits() == 1);
  REQUIRE(response->data.size() == 2);
  REQUIRE(response->data[0].b64_json == "AAEC");
  REQUIRE(response->data[1].url == "data:image/png;base64,AwQF");
  REQUIRE(response->raw["seen_body"] == expected_body);
  REQUIRE(response->raw["seen_authorization"] == "");
  REQUIRE(response->raw["seen_siwx"] == "signed-wallet");
  REQUIRE(response->raw["seen_content_type"] == "application/json");
  REQUIRE(response->metadata.x_balance_remaining == "6.250000");
  REQUIRE(response->metadata.header("x-protocol-trace") == "openai-image-success");

  request.prompt = "malformed";
  const auto malformed = client.generate_image_openai(request);
  REQUIRE_FALSE(malformed.has_value());
  REQUIRE(malformed.error().kind == ErrorKind::Parse);
  REQUIRE(malformed.error().status == 200);
}

TEST_CASE("image styles are public without an empty Authorization header",
          "[transport][images][styles]") {
  const TestServer server;
  const Client public_client{Authentication::public_access(), server.base_url()};
  const auto public_styles = public_client.image_styles();
  REQUIRE(public_styles.has_value());
  REQUIRE(public_styles->returned == 3);
  REQUIRE(public_styles->entries ==
          std::vector<std::string>{"Anime", "authorization:"});
  REQUIRE(public_styles->metadata.header("x-protocol-trace") ==
          "image-styles-success");

  const Client bearer_client{"default-token", server.base_url()};
  const auto bearer_styles = bearer_client.image_styles(
      {.authentication = Authentication::bearer("override-token")});
  REQUIRE(bearer_styles.has_value());
  REQUIRE(bearer_styles->entries ==
          std::vector<std::string>{"Anime", "authorization:present:Bearer override-token"});

  const auto malformed = bearer_client.image_styles(
      {.authentication = Authentication::bearer("malformed-styles")});
  REQUIRE_FALSE(malformed.has_value());
  REQUIRE(malformed.error().kind == ErrorKind::Parse);
  REQUIRE(malformed.error().status == 200);

  const auto wrong_auth = public_client.image_styles(
      {.authentication = Authentication::sign_in_with_x("signed-wallet")});
  REQUIRE_FALSE(wrong_auth.has_value());
  REQUIRE(wrong_auth.error().kind == ErrorKind::InvalidArg);
  REQUIRE(server.image_styles_hits() == 3);
}

TEST_CASE("cancellation interrupts native image generation",
          "[transport][images][cancel][failure]") {
  const TestServer server;
  const Client client{"default-token", server.base_url()};
  auto request = minimal_native_image();
  request.prompt = "stall";
  venice::CancelToken token;
  std::thread canceller{[&] {
    while (server.image_stall_hits() == 0) std::this_thread::sleep_for(5ms);
    token.cancel();
  }};

  std::expected<venice::ImageGenerationResult, venice::Error> result;
  const auto elapsed = timed([&] { result = client.generate_image(request, {.cancel = &token}); });
  canceller.join();
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().kind == ErrorKind::Cancelled);
  REQUIRE(elapsed < kPromptly);
  REQUIRE(server.image_stall_hits() == 1);
}

TEST_CASE("image transformations classify statuses before success media",
          "[transport][images][transform][failure]") {
  const TestServer server;
  const Client client{"default-token", server.base_url()};

  struct Case {
    const char* prompt;
    ErrorKind kind;
    int status;
  };
  const std::array cases{
      Case{"bad-request", ErrorKind::Http, 400},
      Case{"unauthorized", ErrorKind::Auth, 401},
      Case{"unsupported-media", ErrorKind::Http, 415},
      Case{"rate-limit", ErrorKind::RateLimited, 429},
      Case{"server-error", ErrorKind::Http, 500},
      Case{"capacity", ErrorKind::Http, 503},
  };
  for (const auto& test : cases) {
    auto request = minimal_image_edit();
    request.prompt = test.prompt;
    const auto result = client.edit_image(request);
    REQUIRE_FALSE(result);
    REQUIRE(result.error().kind == test.kind);
    REQUIRE(result.error().status == test.status);
    REQUIRE(result.error().metadata.header("x-protocol-trace") ==
            "transform-error");
  }

  auto request = minimal_image_edit();
  request.prompt = "wrong-media";
  const auto wrong_media = client.edit_image(request);
  REQUIRE_FALSE(wrong_media);
  REQUIRE(wrong_media.error().kind == ErrorKind::Parse);
  REQUIRE(wrong_media.error().status == 200);
  REQUIRE(wrong_media.error().body == "not image data");
  REQUIRE(wrong_media.error().metadata.x_balance_remaining == "5.500000");

  const Client wallet{Authentication::sign_in_with_x("transform-needs-payment"),
                      server.base_url()};
  request.prompt = "paint it";
  const auto payment = wallet.edit_image(request);
  REQUIRE_FALSE(payment);
  REQUIRE(payment.error().kind == ErrorKind::PaymentRequired);
  REQUIRE(payment.error().status == 402);
  REQUIRE(payment.error().metadata.payment_required ==
          "transform-payment-requirements");
}

TEST_CASE("image transformations choose exact JSON endpoints and actual media",
          "[transport][images][transform][json]") {
  const TestServer server;
  const Client client{"default-token", server.base_url()};
  const std::string expected_bytes{{'I', 'M', char{0},
                                    static_cast<char>(0xFF), 'G'}};

  auto upscale = minimal_upscale();
  upscale.creativity = 0.0125;
  upscale.scale = 3.5;  // the server, not this client, owns its accepted set
  const auto upscaled = client.upscale_image(upscale);
  REQUIRE(upscaled);
  REQUIRE(upscaled->bytes == expected_bytes);
  REQUIRE(upscaled->media_type == "image/png");
  REQUIRE(upscaled->metadata.payment_response == "transform-payment-receipt");
  auto capture = server.last_transform();
  REQUIRE(capture.path == "/api/v1/image/upscale");
  REQUIRE(capture.content_type == "application/json");
  REQUIRE(capture.authorization == "Bearer default-token");
  REQUIRE(nlohmann::json::parse(capture.body) == upscale.to_json_body());

  auto edit = minimal_image_edit();
  edit.image = venice::image_input::url("https://example.test/source.png");
  edit.model = "edit-model";
  edit.output_format = "webp";
  const auto edited = client.edit_image(
      edit, {.authentication = Authentication::sign_in_with_x("signed-wallet")});
  REQUIRE(edited);
  REQUIRE(edited->media_type == "image/webp");
  capture = server.last_transform();
  REQUIRE(capture.path == "/api/v1/image/edit");
  REQUIRE(capture.siwx == "signed-wallet");
  REQUIRE(nlohmann::json::parse(capture.body) == edit.to_json_body());
  REQUIRE(nlohmann::json::parse(capture.body).contains("model"));
  REQUIRE_FALSE(nlohmann::json::parse(capture.body).contains("modelId"));

  auto multi = minimal_multi_edit();
  multi.model = "multi-model";
  multi.output_format = "jpeg";
  const auto combined = client.multi_edit_image(multi);
  REQUIRE(combined);
  REQUIRE(combined->media_type == "image/jpeg");
  capture = server.last_transform();
  REQUIRE(capture.path == "/api/v1/image/multi-edit");
  const auto multi_body = nlohmann::json::parse(capture.body);
  REQUIRE(multi_body == multi.to_json_body());
  REQUIRE(multi_body["images"] == nlohmann::json::array(
                                      {"BASE", "https://example.test/layer.png"}));
  REQUIRE(multi_body["modelId"] == "multi-model");
  REQUIRE_FALSE(multi_body.contains("model"));

  auto background = minimal_background_removal();
  const auto removed = client.remove_image_background(background);
  REQUIRE(removed);
  REQUIRE(removed->media_type == "image/png");
  capture = server.last_transform();
  REQUIRE(capture.path == "/api/v1/image/background-remove");
  REQUIRE(nlohmann::json::parse(capture.body) == background.to_json_body());
  REQUIRE(nlohmann::json::parse(capture.body).contains("image_url"));
  REQUIRE(server.image_transform_hits() == 4);
}

TEST_CASE("image multipart forms keep owned bytes names types and repeated order",
          "[transport][images][transform][multipart]") {
  const TestServer server;
  const Client client{"default-token", server.base_url()};
  const std::string first{{'P', 'N', char{0}, 'G'}};
  const std::string second{{'W', 'E', static_cast<char>(0xFF), 'B', 'P'}};

  ImageUpscaleRequest upscale;
  upscale.image = venice::image_input::file(first, "source.png", "image/png");
  upscale.creativity = 0.01;
  upscale.scale = 2.0;
  REQUIRE(client.upscale_image(upscale));
  auto capture = server.last_transform();
  REQUIRE(capture.path == "/api/v1/image/upscale");
  REQUIRE(capture.content_type.starts_with("multipart/form-data; boundary="));
  auto images = parts_named(capture, "image");
  REQUIRE(images.size() == 1);
  REQUIRE(images[0].content == first);
  REQUIRE(images[0].filename == "source.png");
  REQUIRE(images[0].content_type == "image/png");
  REQUIRE(parts_named(capture, "creativity")[0].content == "0.01");
  REQUIRE(parts_named(capture, "scale")[0].content == "2.0");

  ImageEditRequest edit;
  edit.image = venice::image_input::file(first, "source.png", "image/png");
  edit.prompt = "paint it";
  edit.model = "edit-model";
  edit.output_format = "webp";
  const auto edited = client.edit_image(edit);
  REQUIRE(edited);
  REQUIRE(edited->media_type == "image/webp");
  capture = server.last_transform();
  REQUIRE(capture.path == "/api/v1/image/edit");
  REQUIRE(parts_named(capture, "prompt")[0].content == "paint it");
  REQUIRE(parts_named(capture, "model")[0].content == "edit-model");

  MultiImageEditRequest multi;
  multi.images = {venice::image_input::file(first, "first.png", "image/png"),
                  venice::image_input::file(second, "second.webp", "image/webp")};
  multi.prompt = "combine them";
  multi.model = "multi-model";
  REQUIRE(client.multi_edit_image(multi));
  capture = server.last_transform();
  REQUIRE(capture.path == "/api/v1/image/multi-edit");
  images = parts_named(capture, "images");
  REQUIRE(images.size() == 2);
  REQUIRE(images[0].content == first);
  REQUIRE(images[0].filename == "first.png");
  REQUIRE(images[1].content == second);
  REQUIRE(images[1].filename == "second.webp");
  REQUIRE(parts_named(capture, "modelId")[0].content == "multi-model");
  REQUIRE(parts_named(capture, "model").empty());

  ImageBackgroundRemovalRequest background;
  background.image =
      venice::image_input::file(second, "subject.webp", "image/webp");
  REQUIRE(client.remove_image_background(background));
  capture = server.last_transform();
  REQUIRE(capture.path == "/api/v1/image/background-remove");
  images = parts_named(capture, "image");
  REQUIRE(images.size() == 1);
  REQUIRE(images[0].content == second);
  REQUIRE(images[0].filename == "subject.webp");
  REQUIRE(images[0].content_type == "image/webp");
}

TEST_CASE("cancellation interrupts multipart image transformation upload",
          "[transport][images][transform][cancel][failure]") {
  const TestServer server;
  const Client client{"default-token", server.base_url()};
  ImageEditRequest request;
  request.image = venice::image_input::file("PNG", "source.png", "image/png");
  request.prompt = "stall";
  venice::CancelToken token;
  std::thread canceller{[&] {
    while (server.image_transform_stall_hits() == 0)
      std::this_thread::sleep_for(5ms);
    token.cancel();
  }};

  std::expected<venice::GeneratedImageMedia, venice::Error> result;
  const auto elapsed = timed(
      [&] { result = client.edit_image(request, {.cancel = &token}); });
  canceller.join();
  REQUIRE_FALSE(result);
  REQUIRE(result.error().kind == ErrorKind::Cancelled);
  REQUIRE(elapsed < kPromptly);
  REQUIRE(server.image_transform_stall_hits() == 1);
}

TEST_CASE("character detail rejects an empty slug before auth or transport",
          "[transport][character][auth][failure]") {
  const TestServer server;
  const Client public_client{Authentication::public_access(), server.base_url()};

  const auto empty = public_client.character("");
  REQUIRE_FALSE(empty.has_value());
  REQUIRE(empty.error().kind == ErrorKind::InvalidArg);
  REQUIRE(empty.error().status == 0);
  REQUIRE(empty.error().message == "character slug must not be empty");
  REQUIRE(empty.error().body.empty());
  REQUIRE(server.character_hits() == 0);

  const auto wrong_auth = public_client.character("fixture");
  REQUIRE_FALSE(wrong_auth.has_value());
  REQUIRE(wrong_auth.error().kind == ErrorKind::InvalidArg);
  REQUIRE(server.character_hits() == 0);
}

TEST_CASE("character detail keeps a slug in one encoded path segment",
          "[transport][character]") {
  const TestServer server;
  const Client client{"default-token", server.base_url()};

  const auto response = client.character(
      "a/b?c#d% e", {.authentication = Authentication::bearer("override-token")});
  REQUIRE(response.has_value());
  REQUIRE(response->slug == "fixture");
  REQUIRE(response->raw["target"] ==
          "/api/v1/characters/a%2Fb%3Fc%23d%25%20e");
  REQUIRE(response->raw["authorization"] == "Bearer override-token");
  REQUIRE(server.character_hits() == 1);
}

TEST_CASE("character detail preserves 404 and rejects a malformed success body",
          "[transport][character][failure]") {
  const TestServer server;
  const Client client{"default-token", server.base_url()};

  const auto missing = client.character("missing");
  REQUIRE_FALSE(missing.has_value());
  REQUIRE(missing.error().kind == ErrorKind::Http);
  REQUIRE(missing.error().status == 404);
  REQUIRE(missing.error().body == R"({"error":"character not found"})");
  REQUIRE(missing.error().metadata.header("x-protocol-trace") == "character-not-found");

  const auto malformed = client.character("wrong-shape");
  REQUIRE_FALSE(malformed.has_value());
  REQUIRE(malformed.error().kind == ErrorKind::Parse);
  REQUIRE(malformed.error().status == 200);
  REQUIRE(malformed.error().message.starts_with("character parse: character:"));
  REQUIRE(malformed.error().body == "[]");
  REQUIRE(server.character_hits() == 2);
}

// ── character reviews (VC-36, #56) ────────────────────────────────────────
//
// The parse is offline in test/08characters/. What only a socket can show is
// the target that actually went on the wire, and this endpoint has a hazard the
// detail fetch does not: the path continues after the slug.

TEST_CASE("character reviews reject an empty slug before auth or transport",
          "[transport][reviews][auth][failure]") {
  const TestServer server;
  const Client public_client{Authentication::public_access(), server.base_url()};

  const auto empty = public_client.character_reviews("");
  REQUIRE_FALSE(empty.has_value());
  REQUIRE(empty.error().kind == ErrorKind::InvalidArg);
  REQUIRE(empty.error().status == 0);
  REQUIRE(empty.error().message == "character slug must not be empty");
  REQUIRE(empty.error().body.empty());
  REQUIRE(server.review_hits() == 0);

  // Bearer-only, like the listing and the detail fetch. Public, SIWX and a
  // pre-built payment are all refused before a socket.
  for (const auto& authentication :
       {Authentication::public_access(), Authentication::sign_in_with_x("siwx-token"),
        Authentication::x402_payment("payment-payload")}) {
    const auto wrong_auth =
        public_client.character_reviews("alan-watts", {}, {.authentication = authentication});
    REQUIRE_FALSE(wrong_auth.has_value());
    REQUIRE(wrong_auth.error().kind == ErrorKind::InvalidArg);
  }
  REQUIRE(server.review_hits() == 0);
}

TEST_CASE("character reviews keep the slug in one path segment", "[transport][reviews]") {
  const TestServer server;
  const Client client{"default-token", server.base_url()};

  // The reason this matters more here than on the detail fetch: an unencoded
  // slash would not select another character, it would address
  // /characters/a/b/reviews — a route this operation does not own.
  const auto response = client.character_reviews(
      "a/b?c#d% e", {}, {.authentication = Authentication::bearer("override-token")});
  REQUIRE(response.has_value());
  REQUIRE(response->entries.size() == 1);
  REQUIRE(response->entries.front().raw["target"] ==
          "/api/v1/characters/a%2Fb%3Fc%23d%25%20e/reviews");
  REQUIRE(response->raw["authorization"] == "Bearer override-token");
  REQUIRE(server.review_hits() == 1);
  REQUIRE(server.character_hits() == 0);  // and never the detail route
}

TEST_CASE("a default reviews query puts no query string on the wire",
          "[transport][reviews]") {
  const TestServer server;
  const Client client{"default-token", server.base_url()};

  const auto bare = client.character_reviews("alan-watts");
  REQUIRE(bare.has_value());
  REQUIRE(bare->entries.front().raw["target"] == "/api/v1/characters/alan-watts/reviews");

  const auto paged =
      client.character_reviews("alan-watts", {.page = 2, .page_size = 100});
  REQUIRE(paged.has_value());
  REQUIRE(paged->entries.front().raw["target"] ==
          "/api/v1/characters/alan-watts/reviews?page=2&pageSize=100");

  REQUIRE(server.review_hits() == 2);
}

TEST_CASE("character reviews preserve 404 and reject a malformed success body",
          "[transport][reviews][failure]") {
  const TestServer server;
  const Client client{"default-token", server.base_url()};

  const auto missing = client.character_reviews("missing");
  REQUIRE_FALSE(missing.has_value());
  REQUIRE(missing.error().kind == ErrorKind::Http);
  REQUIRE(missing.error().status == 404);
  REQUIRE(missing.error().body == R"({"error":"character not found"})");
  REQUIRE(missing.error().metadata.header("x-protocol-trace") == "reviews-not-found");

  // An envelope with no `data` list at all: 200, well-formed JSON, and nothing
  // to degrade to.
  const auto malformed = client.character_reviews("wrong-shape");
  REQUIRE_FALSE(malformed.has_value());
  REQUIRE(malformed.error().kind == ErrorKind::Parse);
  REQUIRE(malformed.error().status == 200);
  REQUIRE(malformed.error().message.starts_with(
      "character reviews parse: character reviews:"));
  REQUIRE(malformed.error().body == R"({"object":"list"})");
  REQUIRE(server.review_hits() == 2);
}

TEST_CASE("a reviews page arrives with its pagination and summary", "[transport][reviews]") {
  const TestServer server;
  const Client client{"default-token", server.base_url()};

  const auto page = client.character_reviews("alan-watts", {.page = 1});
  REQUIRE(page.has_value());
  REQUIRE(page->returned == 1);
  REQUIRE(page->entries.front().id == "r1");
  REQUIRE(page->entries.front().username == "product_user_42");
  REQUIRE(page->entries.front().rating == 5.0);
  REQUIRE(page->pagination.has_value());
  REQUIRE(page->pagination->total_pages == 5);
  REQUIRE(page->summary.has_value());
  REQUIRE(page->summary->average_rating == 4.7);
}

// ── billing (VC-42, #68) ──────────────────────────────────────────────────
//
// Parser/guard cases live in test/13billing/. What is only visible from here is
// the exact target and Accept header that went on the wire, actual-media routing,
// response metadata, cancellation, and non-2xx classification before media.

TEST_CASE("billing calls reject non-Bearer authentication before the socket",
          "[transport][billing][auth][failure]") {
  const TestServer server;
  for (const auto& authentication :
       {Authentication::public_access(), Authentication::sign_in_with_x("signed-wallet"),
        Authentication::x402_payment("payment-payload")}) {
    const Client client{authentication, server.base_url()};
    REQUIRE_FALSE(client.billing_balance());
    REQUIRE_FALSE(client.billing_usage_analytics());
    REQUIRE_FALSE(client.billing_usage_history());
  }
  REQUIRE(server.billing_hits() == 0);
}

TEST_CASE("billing calls preserve exact targets, Bearer overrides and metadata",
          "[transport][billing]") {
  const TestServer server;
  const Client client{"default-token", server.base_url()};

  const auto balance = client.billing_balance(
      {.authentication = Authentication::bearer("override-token")});
  REQUIRE(balance);
  REQUIRE(balance->raw["target"] == "/api/v1/billing/balance");
  REQUIRE(balance->raw["authorization"] == "Bearer override-token");
  REQUIRE(balance->metadata.header("x-test-response") == "billing-balance");

  const auto analytics = client.billing_usage_analytics(
      {.lookback = "7 d", .extra = {{"future", "a&b"}}});
  REQUIRE(analytics);
  REQUIRE(analytics->raw["target"] ==
          "/api/v1/billing/usage-analytics?lookback=7%20d&future=a%26b");
  REQUIRE(analytics->raw["authorization"] == "Bearer default-token");
  REQUIRE(analytics->metadata.header("x-test-response") == "billing-analytics");

  const auto history = client.billing_usage_history({.query = {
                                                          .currency = "DIEM",
                                                          .end_timestamp = "later",
                                                          .page_size = 10,
                                                          .start_timestamp = "earlier",
                                                      }});
  REQUIRE(history);
  const auto* page = std::get_if<venice::BillingUsageHistoryPage>(&*history);
  REQUIRE(page != nullptr);
  REQUIRE(page->raw["accept"] == "application/json");
  REQUIRE(page->raw["authorization"] == "Bearer default-token");
  REQUIRE(page->entries.front().notes ==
          "/api/v1/billing/usage-history?currency=DIEM&endTimestamp=later&pageSize=10&startTimestamp=earlier");
  REQUIRE(page->metadata.header("x-test-response") == "billing-history");
  REQUIRE(server.billing_hits() == 3);
}

TEST_CASE("billing history follows actual response media and preserves CSV bytes",
          "[transport][billing][media]") {
  const TestServer server;
  const Client client{"default-token", server.base_url()};

  const auto forced_csv = client.billing_usage_history(
      {.query = {.extra = {{"force", "csv"}}},
       .format = venice::BillingUsageHistoryFormat::Json});
  REQUIRE(forced_csv);
  const auto* csv = std::get_if<venice::BillingUsageHistoryCsv>(&*forced_csv);
  REQUIRE(csv != nullptr);
  REQUIRE(csv->text == "amount,currency,notes\r\n-0.25,DIEM,synthetic\r\n");
  REQUIRE(csv->media_type == "text/csv");
  REQUIRE(csv->next_cursor == "next_csv_page");
  REQUIRE(csv->content_disposition ==
          "attachment; filename=billing-usage-history-20260816T120000000Z.csv");
  REQUIRE(csv->metadata.header("x-test-response") == "billing-history");

  const auto forced_json = client.billing_usage_history(
      {.query = {.extra = {{"force", "json"}}},
       .format = venice::BillingUsageHistoryFormat::Csv});
  REQUIRE(forced_json);
  const auto* page = std::get_if<venice::BillingUsageHistoryPage>(&*forced_json);
  REQUIRE(page != nullptr);
  REQUIRE(page->raw["accept"] == "text/csv");
}

TEST_CASE("billing errors are classified before success media and shape",
          "[transport][billing][failure]") {
  const TestServer server;
  const Client client{"default-token", server.base_url()};

  for (const auto& [status, kind] :
       std::vector<std::pair<int, ErrorKind>>{{400, ErrorKind::Http},
                                               {401, ErrorKind::Auth},
                                               {500, ErrorKind::Http}}) {
    const auto result = client.billing_usage_history(
        {.query = {.extra = {{"status", std::to_string(status)}}},
         .format = venice::BillingUsageHistoryFormat::Csv});
    REQUIRE_FALSE(result);
    REQUIRE(result.error().status == status);
    REQUIRE(result.error().kind == kind);
    REQUIRE(result.error().body == R"({"error":"history failed"})");
    REQUIRE(result.error().metadata.header("x-protocol-trace") ==
            "billing-history-error");
  }

  const auto timeout = client.billing_usage_analytics({.extra = {{"status", "504"}}});
  REQUIRE_FALSE(timeout);
  REQUIRE(timeout.error().kind == ErrorKind::Http);
  REQUIRE(timeout.error().status == 504);
  REQUIRE(timeout.error().metadata.header("x-protocol-trace") == "billing-timeout");

  const auto wrong_media = client.billing_usage_history(
      {.query = {.extra = {{"wrong-media", "yes"}}}});
  REQUIRE_FALSE(wrong_media);
  REQUIRE(wrong_media.error().kind == ErrorKind::Parse);
  REQUIRE(wrong_media.error().status == 200);
  REQUIRE(wrong_media.error().metadata.header("x-protocol-trace") ==
          "billing-history-media");

  const auto wrong_history = client.billing_usage_history(
      {.query = {.extra = {{"wrong-shape", "yes"}}}});
  REQUIRE_FALSE(wrong_history);
  REQUIRE(wrong_history.error().kind == ErrorKind::Parse);
  REQUIRE(wrong_history.error().body == R"({"data":[]})");

  const auto wrong_analytics = client.billing_usage_analytics(
      {.extra = {{"wrong-shape", "yes"}}});
  REQUIRE_FALSE(wrong_analytics);
  REQUIRE(wrong_analytics.error().kind == ErrorKind::Parse);
  REQUIRE(wrong_analytics.error().body == "[]");
}

TEST_CASE("pre-cancelled billing calls send no request", "[transport][billing][cancel]") {
  const TestServer server;
  const Client client{"default-token", server.base_url()};
  venice::CancelToken token;
  token.cancel();

  const auto balance = client.billing_balance({.cancel = &token});
  REQUIRE_FALSE(balance);
  REQUIRE(balance.error().kind == ErrorKind::Cancelled);
  REQUIRE(server.billing_hits() == 0);
}

// ── API-key lifecycle and rate limits (VC-43, #70) ─────────────────────

TEST_CASE("API-key calls reject non-Bearer authentication before the socket",
          "[transport][api-keys][auth][failure]") {
  const TestServer server;
  for (const auto& authentication :
       {Authentication::public_access(), Authentication::sign_in_with_x("signed-wallet"),
        Authentication::x402_payment("payment-payload")}) {
    const Client client{authentication, server.base_url()};
    REQUIRE_FALSE(client.api_keys());
    REQUIRE_FALSE(client.api_key("fixture-id"));
    REQUIRE_FALSE(client.create_api_key({.api_key_type = "INFERENCE",
                                         .description = "fixture"}));
    REQUIRE_FALSE(client.update_api_key({.id = "fixture-id"}));
    REQUIRE_FALSE(client.delete_api_key("fixture-id"));
    REQUIRE_FALSE(client.api_key_rate_limits());
    REQUIRE_FALSE(client.api_key_rate_limit_logs());
  }
  REQUIRE(server.api_key_hits() == 0);
}

TEST_CASE("API-key calls preserve exact methods targets bodies and metadata",
          "[transport][api-keys]") {
  const TestServer server;
  const Client client{"api-key-fixture", server.base_url()};

  const auto list = client.api_keys();
  REQUIRE(list);
  REQUIRE(list->entries.front().raw["target"] == "/api/v1/api_keys");
  REQUIRE(list->entries.front().raw["method"] == "GET");
  REQUIRE(list->entries.front().raw["authorization"] == "Bearer api-key-fixture");
  REQUIRE(list->metadata.header("x-test-response") == "api-key-list");

  const auto detail = client.api_key("a/b ?");
  REQUIRE(detail);
  REQUIRE(detail->raw["target"] == "/api/v1/api_keys/a%2Fb%20%3F");
  REQUIRE(detail->metadata.header("x-test-response") == "api-key-detail");

  const auto created = client.create_api_key({
      .api_key_type = "INFERENCE",
      .description = "fixture",
      .consumption_limit = venice::ApiKeyConsumptionLimitRequest{.usd = 0.0},
      .limit_period = "EPOCH",
      .extra = {{"future", true}},
  });
  REQUIRE(created);
  REQUIRE(created->id == "fixture-created-id");
  REQUIRE(created->api_key == "SYNTHETIC_SECRET_RETURNED_ONCE");
  REQUIRE(created->raw["data"]["apiKey"] == "[REDACTED]");
  REQUIRE(created->raw.dump().find("SYNTHETIC_SECRET_RETURNED_ONCE") ==
          std::string::npos);
  REQUIRE(created->raw["seenBody"]["apiKeyType"] == "INFERENCE");
  REQUIRE(created->raw["seenBody"]["consumptionLimit"]["usd"] == 0.0);
  REQUIRE(created->raw["seenBody"]["future"] == true);
  REQUIRE(created->raw["authorization"] == "Bearer api-key-fixture");
  REQUIRE(created->metadata.header("x-test-response") == "api-key-create");

  const auto updated = client.update_api_key({
      .id = "fixture-id",
      .description = "updated",
      .expires_at = "",
  });
  REQUIRE(updated);
  REQUIRE(updated->key.raw["method"] == "PATCH");
  REQUIRE(updated->raw["seenBody"]["id"] == "fixture-id");
  REQUIRE(updated->raw["seenBody"]["description"] == "updated");
  REQUIRE(updated->raw["seenBody"]["expiresAt"] == "");
  REQUIRE(updated->metadata.header("x-test-response") == "api-key-update");

  const auto deleted = client.delete_api_key("a/b ?");
  REQUIRE(deleted);
  REQUIRE(deleted->success);
  REQUIRE(deleted->raw["target"] == "/api/v1/api_keys?id=a%2Fb%20%3F");
  REQUIRE(deleted->raw["authorization"] == "Bearer api-key-fixture");
  REQUIRE(deleted->metadata.header("x-test-response") == "api-key-delete");

  const auto limits = client.api_key_rate_limits();
  REQUIRE(limits);
  REQUIRE(limits->balances->usd == 0.0);
  REQUIRE(limits->raw["data"]["target"] == "/api/v1/api_keys/rate_limits");
  REQUIRE(limits->metadata.header("x-test-response") == "api-key-rate-limits");

  const auto logs = client.api_key_rate_limit_logs();
  REQUIRE(logs);
  REQUIRE(logs->returned == 1);
  REQUIRE(logs->entries.front().raw["target"] ==
          "/api/v1/api_keys/rate_limits/log");
  REQUIRE(logs->metadata.header("x-test-response") == "api-key-rate-limit-logs");
  REQUIRE(server.api_key_hits() == 7);
}

TEST_CASE("API-key errors classify before success media and shape",
          "[transport][api-keys][failure]") {
  const TestServer server;
  const Client client{"api-key-fixture", server.base_url()};

  struct ErrorCase {
    int status;
    ErrorKind kind;
  };
  for (const auto [status, kind] :
       {ErrorCase{400, ErrorKind::Http}, ErrorCase{401, ErrorKind::Auth},
        ErrorCase{402, ErrorKind::PaymentRequired},
        ErrorCase{429, ErrorKind::RateLimited}, ErrorCase{500, ErrorKind::Http}}) {
    const auto denied = client.api_key("status-" + std::to_string(status));
    REQUIRE_FALSE(denied);
    REQUIRE(denied.error().kind == kind);
    REQUIRE(denied.error().status == status);
    REQUIRE(denied.error().body == R"({"error":"denied"})");
    REQUIRE(denied.error().metadata.header("x-protocol-trace") ==
            "api-key-auth-error");
  }

  const auto wrong_media = client.api_key("wrong-media");
  REQUIRE_FALSE(wrong_media);
  REQUIRE(wrong_media.error().kind == ErrorKind::Parse);
  REQUIRE(wrong_media.error().status == 200);
  REQUIRE(wrong_media.error().metadata.header("x-protocol-trace") ==
          "api-key-wrong-media");

  const auto wrong_shape = client.api_key("wrong-shape");
  REQUIRE_FALSE(wrong_shape);
  REQUIRE(wrong_shape.error().kind == ErrorKind::Parse);
  REQUIRE(wrong_shape.error().body == "[]");
}

TEST_CASE("API-key creation errors redact returned key material",
          "[transport][api-keys][security][failure]") {
  const TestServer server;
  const Client client{"api-key-fixture", server.base_url()};

  const auto malformed = client.create_api_key({
      .api_key_type = "INFERENCE",
      .description = "fixture",
      .extra = {{"forceMalformed", true}},
  });
  REQUIRE_FALSE(malformed);
  REQUIRE(malformed.error().kind == ErrorKind::Parse);
  REQUIRE(malformed.error().body.find("SYNTHETIC_SECRET_RETURNED_ONCE") ==
          std::string::npos);
  REQUIRE(malformed.error().body.find("[REDACTED]") != std::string::npos);

  const auto rejected = client.create_api_key({
      .api_key_type = "INFERENCE",
      .description = "fixture",
      .extra = {{"forceHttpError", true}},
  });
  REQUIRE_FALSE(rejected);
  REQUIRE(rejected.error().kind == ErrorKind::Http);
  REQUIRE(rejected.error().body.find("SYNTHETIC_SECRET_RETURNED_ONCE") ==
          std::string::npos);
  REQUIRE(rejected.error().body.find("[REDACTED]") != std::string::npos);
}

TEST_CASE("balance remains the raw compatibility view of typed rate limits",
          "[transport][api-keys][compat]") {
  const TestServer server;
  const Client client{"api-key-fixture", server.base_url()};
  const auto balance = client.balance();
  REQUIRE(balance);
  REQUIRE((*balance)["data"]["accessPermitted"] == true);
  REQUIRE((*balance)["data"]["balances"]["USD"] == 0.0);
  REQUIRE((*balance)["futureEnvelope"] == true);
  REQUIRE(server.api_key_hits() == 1);
}

TEST_CASE("pre-cancelled API-key calls send no request",
          "[transport][api-keys][cancel]") {
  const TestServer server;
  const Client client{"api-key-fixture", server.base_url()};
  venice::CancelToken token;
  token.cancel();
  const auto result = client.api_keys({.cancel = &token});
  REQUIRE_FALSE(result);
  REQUIRE(result.error().kind == ErrorKind::Cancelled);
  REQUIRE(server.api_key_hits() == 0);
}

// ── public Web3 API-key proof flow (VC-44, #72) ────────────────────────

TEST_CASE("Web3 API-key calls reject credential-bearing transport before the socket",
          "[transport][api-keys][web3][auth][failure]") {
  const TestServer server;
  const venice::Web3ApiKeyCreateRequest request{
      .api_key_type = "INFERENCE",
      .address = "synthetic-address",
      .signature = "SYNTHETIC_SIGNATURE_SECRET",
      .token = "SYNTHETIC_CHALLENGE_SECRET",
  };
  for (const auto& authentication :
       {Authentication::bearer("bearer-secret"),
        Authentication::sign_in_with_x("signed-wallet"),
        Authentication::x402_payment("payment-payload")}) {
    const Client client{authentication, server.base_url()};
    const auto challenge = client.web3_api_key_challenge();
    REQUIRE_FALSE(challenge);
    REQUIRE(challenge.error().kind == ErrorKind::InvalidArg);
    REQUIRE(challenge.error().message == "endpoint requires public authentication");
    REQUIRE(challenge.error().body.empty());
    const auto created = client.create_web3_api_key(request);
    REQUIRE_FALSE(created);
    REQUIRE(created.error().kind == ErrorKind::InvalidArg);
    REQUIRE(created.error().message == "endpoint requires public authentication");
    REQUIRE(created.error().body.empty());
  }
  REQUIRE(server.web3_api_key_hits() == 0);
}

TEST_CASE("Web3 API-key calls send exact public wire contracts and retain metadata",
          "[transport][api-keys][web3][security]") {
  const TestServer server;
  const Client client{Authentication::public_access(), server.base_url()};

  const auto challenge = client.web3_api_key_challenge();
  REQUIRE(challenge);
  REQUIRE(challenge->success);
  REQUIRE(challenge->token == "SYNTHETIC_CHALLENGE_SECRET");
  REQUIRE(challenge->raw["data"]["token"] == "[REDACTED]");
  REQUIRE(challenge->raw["futureEnvelope"]["signature"] == "[REDACTED]");
  REQUIRE(challenge->metadata.header("x-test-response") ==
          "web3-api-key-challenge");
  auto captured = server.last_web3();
  REQUIRE(captured.method == "GET");
  REQUIRE(captured.path == "/api/v1/api_keys/generate_web3_key");
  REQUIRE(captured.content_type.empty());
  REQUIRE(captured.authorization.empty());
  REQUIRE(captured.siwx.empty());
  REQUIRE(captured.payment.empty());
  REQUIRE(captured.body.empty());

  const auto created = client.create_web3_api_key({
      .api_key_type = "FUTURE_TYPE",
      .address = "synthetic-address",
      .signature = "SYNTHETIC_SIGNATURE_SECRET",
      .token = "SYNTHETIC_CHALLENGE_SECRET",
      .consumption_limit = venice::ApiKeyConsumptionLimitRequest{.usd = 0.0},
      .limit_period = "FUTURE_PERIOD",
      .description = "synthetic key",
      .expires_at = "",
      .extra = {{"future", true}},
  });
  REQUIRE(created);
  REQUIRE(created->success);
  REQUIRE(created->id == "fixture-web3-key-id");
  REQUIRE(created->api_key == "SYNTHETIC_WEB3_API_KEY_SECRET");
  REQUIRE(created->raw["data"]["apiKey"] == "[REDACTED]");
  REQUIRE(created->raw["seenBody"]["token"] == "[REDACTED]");
  REQUIRE(created->raw["seenBody"]["signature"] == "[REDACTED]");
  REQUIRE(created->raw.dump().find("SYNTHETIC_CHALLENGE_SECRET") ==
          std::string::npos);
  REQUIRE(created->raw.dump().find("SYNTHETIC_SIGNATURE_SECRET") ==
          std::string::npos);
  REQUIRE(created->raw.dump().find("SYNTHETIC_WEB3_API_KEY_SECRET") ==
          std::string::npos);
  REQUIRE(created->metadata.header("x-test-response") == "web3-api-key-create");

  captured = server.last_web3();
  REQUIRE(captured.method == "POST");
  REQUIRE(captured.path == "/api/v1/api_keys/generate_web3_key");
  REQUIRE(captured.content_type == "application/json");
  REQUIRE(captured.authorization.empty());
  REQUIRE(captured.siwx.empty());
  REQUIRE(captured.payment.empty());
  const auto body = nlohmann::json::parse(captured.body);
  REQUIRE(body["apiKeyType"] == "FUTURE_TYPE");
  REQUIRE(body["address"] == "synthetic-address");
  REQUIRE(body["signature"] == "SYNTHETIC_SIGNATURE_SECRET");
  REQUIRE(body["token"] == "SYNTHETIC_CHALLENGE_SECRET");
  REQUIRE(body["consumptionLimit"]["usd"] == 0.0);
  REQUIRE(body["limitPeriod"] == "FUTURE_PERIOD");
  REQUIRE(body["description"] == "synthetic key");
  REQUIRE(body["expiresAt"] == "");
  REQUIRE(body["future"] == true);
  REQUIRE(server.web3_api_key_hits() == 2);
}

TEST_CASE("a per-call public override strips a Bearer client's credential",
          "[transport][api-keys][web3][auth]") {
  const TestServer server;
  const Client client{"default-bearer-secret", server.base_url()};
  const auto challenge = client.web3_api_key_challenge(
      {.authentication = Authentication::public_access()});
  REQUIRE(challenge);
  const auto captured = server.last_web3();
  REQUIRE(captured.authorization.empty());
  REQUIRE(captured.siwx.empty());
  REQUIRE(captured.payment.empty());
  REQUIRE(server.web3_api_key_hits() == 1);
}

TEST_CASE("Web3 challenge failures redact secrets and preserve classification",
          "[transport][api-keys][web3][security][failure]") {
  {
    const TestServer server{Web3ChallengeMode::HttpError};
    const Client client{Authentication::public_access(), server.base_url()};
    const auto result = client.web3_api_key_challenge();
    REQUIRE_FALSE(result);
    REQUIRE(result.error().kind == ErrorKind::Auth);
    REQUIRE(result.error().status == 401);
    REQUIRE(result.error().metadata.header("x-protocol-trace") ==
            "web3-challenge-http");
    REQUIRE(result.error().body.find("[REDACTED]") != std::string::npos);
    REQUIRE(result.error().body.find("SYNTHETIC_") == std::string::npos);
  }
  {
    const TestServer server{Web3ChallengeMode::WrongMedia};
    const Client client{Authentication::public_access(), server.base_url()};
    const auto result = client.web3_api_key_challenge();
    REQUIRE_FALSE(result);
    REQUIRE(result.error().kind == ErrorKind::Parse);
    REQUIRE(result.error().body ==
            "[REDACTED: non-JSON Web3 API-key response]");
  }
  {
    const TestServer server{Web3ChallengeMode::WrongShape};
    const Client client{Authentication::public_access(), server.base_url()};
    const auto result = client.web3_api_key_challenge();
    REQUIRE_FALSE(result);
    REQUIRE(result.error().kind == ErrorKind::Parse);
    REQUIRE(result.error().body.find("SYNTHETIC_CHALLENGE_SECRET") ==
            std::string::npos);
    REQUIRE(result.error().body.find("[REDACTED]") != std::string::npos);
  }
  {
    const TestServer server{Web3ChallengeMode::MalformedJson};
    const Client client{Authentication::public_access(), server.base_url()};
    const auto result = client.web3_api_key_challenge();
    REQUIRE_FALSE(result);
    REQUIRE(result.error().kind == ErrorKind::Parse);
    REQUIRE(result.error().body ==
            "[REDACTED: non-JSON Web3 API-key response]");
  }
}

TEST_CASE("Web3 creation failures classify before media and redact every proof",
          "[transport][api-keys][web3][security][failure]") {
  const TestServer server;
  const Client client{Authentication::public_access(), server.base_url()};
  const auto call = [&](nlohmann::json extra) {
    return client.create_web3_api_key({
        .api_key_type = "INFERENCE",
        .address = "synthetic-address",
        .signature = "SYNTHETIC_SIGNATURE_SECRET",
        .token = "SYNTHETIC_CHALLENGE_SECRET",
        .extra = std::move(extra),
    });
  };

  struct ErrorCase {
    int status;
    ErrorKind kind;
  };
  for (const auto [status, kind] :
       {ErrorCase{400, ErrorKind::Http}, ErrorCase{401, ErrorKind::Auth},
        ErrorCase{402, ErrorKind::PaymentRequired},
        ErrorCase{429, ErrorKind::RateLimited}, ErrorCase{500, ErrorKind::Http}}) {
    const auto result = call({{"forceStatus", status}});
    REQUIRE_FALSE(result);
    REQUIRE(result.error().kind == kind);
    REQUIRE(result.error().status == status);
    REQUIRE(result.error().metadata.header("x-protocol-trace") ==
            "web3-create-http");
    REQUIRE(result.error().body.find("[REDACTED]") != std::string::npos);
    REQUIRE(result.error().body.find("SYNTHETIC_") == std::string::npos);
  }

  const auto wrong_media = call({{"forceWrongMedia", true}});
  REQUIRE_FALSE(wrong_media);
  REQUIRE(wrong_media.error().kind == ErrorKind::Parse);
  REQUIRE(wrong_media.error().body ==
          "[REDACTED: non-JSON Web3 API-key response]");

  const auto malformed = call({{"forceMalformedJson", true}});
  REQUIRE_FALSE(malformed);
  REQUIRE(malformed.error().kind == ErrorKind::Parse);
  REQUIRE(malformed.error().body ==
          "[REDACTED: non-JSON Web3 API-key response]");

  const auto wrong_shape = call({{"forceWrongShape", true}});
  REQUIRE_FALSE(wrong_shape);
  REQUIRE(wrong_shape.error().kind == ErrorKind::Parse);
  REQUIRE(wrong_shape.error().body.find("SYNTHETIC_") == std::string::npos);
  REQUIRE(wrong_shape.error().body.find("[REDACTED]") != std::string::npos);
}

TEST_CASE("pre-cancelled Web3 API-key calls send no request",
          "[transport][api-keys][web3][cancel]") {
  const TestServer server;
  const Client client{Authentication::public_access(), server.base_url()};
  venice::CancelToken token;
  token.cancel();
  const venice::RequestOptions opts{.cancel = &token};
  const auto challenge = client.web3_api_key_challenge(opts);
  REQUIRE_FALSE(challenge);
  REQUIRE(challenge.error().kind == ErrorKind::Cancelled);
  REQUIRE(challenge.error().body.empty());
  const auto created = client.create_web3_api_key(
      {.api_key_type = "INFERENCE",
       .address = "synthetic-address",
       .signature = "SYNTHETIC_SIGNATURE_SECRET",
       .token = "SYNTHETIC_CHALLENGE_SECRET"},
      opts);
  REQUIRE_FALSE(created);
  REQUIRE(created.error().kind == ErrorKind::Cancelled);
  REQUIRE(created.error().body.empty());
  REQUIRE(server.web3_api_key_hits() == 0);
}

// ── the catalogue sub-paths (VC-38, #59) ──────────────────────────────────
//
// Failure matrix first, as everywhere. What is only visible from here — and not
// from test/09catalogue/ — is the target that actually went on the wire, the
// header the client chose to send, and how a non-2xx status is classified.

TEST_CASE("the catalogue calls reject impossible auth modes before the socket",
          "[transport][auth][catalogue][failure]") {
  const TestServer server;

  // Both impossible modes against BOTH operations, rather than one each. The
  // asymmetric version of this case passed while covering half of it: if
  // model_compatibility_mapping's policy were copy-pasted to
  // BearerOrSignInWithX, a SIWX client would have put a SIGN-IN-WITH-X header on
  // the wire and nothing here would have noticed, because compat was only ever
  // probed with x402.
  const Client siwx_client{Authentication::sign_in_with_x("signed-wallet"), server.base_url()};
  const auto siwx_traits = siwx_client.model_traits();
  REQUIRE_FALSE(siwx_traits.has_value());
  REQUIRE(siwx_traits.error().kind == ErrorKind::InvalidArg);

  const auto siwx_compat = siwx_client.model_compatibility_mapping();
  REQUIRE_FALSE(siwx_compat.has_value());
  REQUIRE(siwx_compat.error().kind == ErrorKind::InvalidArg);

  const Client payment_client{Authentication::x402_payment("payment-payload"),
                              server.base_url()};
  const auto payment_traits = payment_client.model_traits();
  REQUIRE_FALSE(payment_traits.has_value());
  REQUIRE(payment_traits.error().kind == ErrorKind::InvalidArg);

  const auto payment_compat = payment_client.model_compatibility_mapping();
  REQUIRE_FALSE(payment_compat.has_value());
  REQUIRE(payment_compat.error().kind == ErrorKind::InvalidArg);
  // The credential must not leak into the diagnostic of a call that never left.
  REQUIRE(payment_compat.error().message.find("payment-payload") == std::string::npos);

  // An empty Bearer is not the same thing as public access, and the distinction
  // matters more on these two endpoints than anywhere else: they would have
  // answered 200 to a request carrying no credential at all, so a client that
  // silently degraded an empty token to public would look like it worked.
  const Client empty_bearer{"", server.base_url()};
  const auto empty_traits = empty_bearer.model_traits();
  REQUIRE_FALSE(empty_traits.has_value());
  REQUIRE(empty_traits.error().kind == ErrorKind::InvalidArg);

  const auto empty_compat = empty_bearer.model_compatibility_mapping();
  REQUIRE_FALSE(empty_compat.has_value());
  REQUIRE(empty_compat.error().kind == ErrorKind::InvalidArg);

  // Nothing reached the peer in any of the six.
  REQUIRE(server.traits_hits() == 0);
  REQUIRE(server.compat_hits() == 0);
}

TEST_CASE("a refused type filter reaches the caller with its status and body intact",
          "[transport][catalogue][failure]") {
  const TestServer server;
  const Client client{"default-token", server.base_url()};

  // The decision this pins: `type` is passed through, not validated here. The
  // client sends "all", the server refuses it, and the refusal arrives whole.
  const auto res = client.model_compatibility_mapping("all");
  REQUIRE_FALSE(res.has_value());
  REQUIRE(res.error().kind == ErrorKind::Http);
  REQUIRE(res.error().status == 400);
  REQUIRE(server.compat_hits() == 1);

  // The part a local guard could never have produced: the server names the set
  // it will accept. That string is the whole reason not to guess it here.
  REQUIRE(res.error().body.find("Invalid enum value") != std::string::npos);
  REQUIRE(res.error().body.find("received 'all'") != std::string::npos);
  REQUIRE(res.error().metadata.header("x-protocol-trace") == "compat-enum-refused");

  // And the asymmetry itself: the same value on the sibling operation is fine.
  // Byte-identical `parameters` blocks in the OpenAPI document, two different
  // answers on the wire — measured 2026-08-11.
  const auto traits = client.model_traits("all");
  REQUIRE(traits.has_value());
  REQUIRE(server.traits_hits() == 1);
}

TEST_CASE("a 200 with no data object is a parse failure, not an empty map",
          "[transport][catalogue][failure]") {
  const TestServer server;
  const Client client{"default-token", server.base_url()};

  // The transport twin of test/09catalogue/'s headline case. A client that
  // copied the list-endpoint `data ? *data : j` fallback would return a
  // successful two-entry map here and this is where that would surface as a
  // caller acting on {object -> "list", type -> "text"}.
  const auto res = client.model_traits("no-data");
  REQUIRE_FALSE(res.has_value());
  REQUIRE(res.error().kind == ErrorKind::Parse);
  REQUIRE(res.error().status == 200);  // the transport was fine; the body was not
  REQUIRE(res.error().message.starts_with("model traits parse: model traits:"));
  REQUIRE(res.error().body == R"({"object":"list","type":"text"})");
}

TEST_CASE("the catalogue calls put the exact encoded target on the wire",
          "[transport][catalogue]") {
  const TestServer server;
  const Client client{"default-token", server.base_url()};

  // No filter: no query string at all. This is the empty-skip contract, and it
  // is what every caller that passes nothing depends on.
  const auto bare = client.model_traits();
  REQUIRE(bare.has_value());
  REQUIRE(echoed(*bare, "target") == "/api/v1/models/traits");

  const auto filtered = client.model_traits("image");
  REQUIRE(filtered.has_value());
  REQUIRE(echoed(*filtered, "target") == "/api/v1/models/traits?type=image");

  // A value needing encoding. The pair separators must survive as data rather
  // than becoming structure — otherwise a filter could address a different query.
  const auto encoded = client.model_traits("a b&type=admin");
  REQUIRE(encoded.has_value());
  REQUIRE(echoed(*encoded, "target") == "/api/v1/models/traits?type=a%20b%26type%3Dadmin");

  const auto compat = client.model_compatibility_mapping("text");
  REQUIRE(compat.has_value());
  REQUIRE(echoed(*compat, "target") == "/api/v1/models/compatibility_mapping?type=text");

  // The paths do not collide with /models. Asserted through the hit counters
  // because a route mix-up is exactly the failure that still returns a valid
  // body — /models would parse into an empty map rather than an error.
  REQUIRE(server.models_hits() == 0);
  REQUIRE(server.traits_hits() == 3);
  REQUIRE(server.compat_hits() == 1);
}

TEST_CASE("the catalogue calls send no credential when the client is public",
          "[transport][auth][catalogue]") {
  const TestServer server;
  const Client public_client{Authentication::public_access(), server.base_url()};

  // These are the endpoints where public is the *expected* mode rather than a
  // tolerated one, so the assertion has to be that the header is **absent**, not
  // that its value is empty. Those are different failures and only one of them
  // is correct: a client that sent `Authorization:` with nothing after it would
  // still be answered 200 by the real server, so nothing downstream would ever
  // reveal the bug.
  //
  // httplib's get_header_value returns "" for both, which is why the fixture
  // echoes a "present:" prefix instead of the bare value — without it this case
  // reads as if it distinguishes them while asserting only the weaker half.
  const auto traits = public_client.model_traits();
  REQUIRE(traits.has_value());
  REQUIRE(echoed(*traits, "authorization").empty());  // absent, not "present:"

  const auto compat = public_client.model_compatibility_mapping();
  REQUIRE(compat.has_value());
  REQUIRE(echoed(*compat, "authorization").empty());

  // Both directions of the per-call override, on both operations.
  const auto bearer_override =
      public_client.model_traits({}, {.authentication = Authentication::bearer("override-token")});
  REQUIRE(bearer_override.has_value());
  REQUIRE(echoed(*bearer_override, "authorization") == "present:Bearer override-token");

  const Client bearer_client{"default-token", server.base_url()};
  const auto keyed = bearer_client.model_compatibility_mapping();
  REQUIRE(keyed.has_value());
  REQUIRE(echoed(*keyed, "authorization") == "present:Bearer default-token");

  const auto public_override = bearer_client.model_compatibility_mapping(
      {}, {.authentication = Authentication::public_access()});
  REQUIRE(public_override.has_value());
  REQUIRE(echoed(*public_override, "authorization").empty());
}

TEST_CASE("public and Bearer model calls honor per-call authentication overrides",
          "[transport][auth]") {
  const TestServer server;
  const Client public_client{Authentication::public_access(), server.base_url()};

  const auto public_models = public_client.models();
  REQUIRE(public_models.has_value());
  REQUIRE(public_models->front().raw["authorization"] == "");
  REQUIRE(public_models->front().raw["siwx"] == "");
  REQUIRE(public_models->front().raw["payment"] == "");

  const auto bearer_override = public_client.models(
      {}, {.authentication = Authentication::bearer("override-token")});
  REQUIRE(bearer_override.has_value());
  REQUIRE(bearer_override->front().raw["authorization"] == "Bearer override-token");

  const Client bearer_client{"default-token", server.base_url()};
  const auto public_override = bearer_client.models(
      {}, {.authentication = Authentication::public_access()});
  REQUIRE(public_override.has_value());
  REQUIRE(public_override->front().raw["authorization"] == "");
}

TEST_CASE("SIWX buffered and streamed successes expose response metadata",
          "[transport][auth][metadata]") {
  const TestServer server;
  const Client bearer_client{"default-token", server.base_url()};

  const auto buffered = bearer_client.chat(
      minimal_chat(), {.authentication = Authentication::sign_in_with_x("signed-wallet")});
  REQUIRE(buffered.has_value());
  REQUIRE(buffered->raw["seen_authorization"] == "");
  REQUIRE(buffered->raw["seen_siwx"] == "signed-wallet");
  REQUIRE(buffered->metadata.x_balance_remaining == "4.230000");
  REQUIRE(buffered->metadata.header("x-protocol-trace") == "retained-on-success");

  venice::StreamAccumulator completed_acc;
  const auto completed = bearer_client.chat_stream(
      minimal_chat(), completed_acc,
      {.authentication = Authentication::sign_in_with_x("complete-stream")});
  REQUIRE(completed.has_value());
  REQUIRE(completed->content == "hello");
  REQUIRE(completed->metadata.x_balance_remaining == "4.230000");
  REQUIRE(completed->metadata.header("X-Protocol-Trace") == "retained-on-success");

  venice::StreamAccumulator early_acc;
  const auto early = bearer_client.chat_stream(
      minimal_chat(), early_acc, [](const venice::StreamDelta&) { return false; },
      {.authentication = Authentication::sign_in_with_x("signed-wallet")});
  REQUIRE(early.has_value());
  REQUIRE(early->content == "hel");
  REQUIRE(early->metadata.x_balance_remaining == "4.230000");
}

TEST_CASE("402 is payment-required and preserves body and headers on both chat paths",
          "[transport][auth][payment][failure]") {
  const TestServer server;
  const std::string secret = "needs-payment";
  const Client client{Authentication::sign_in_with_x(secret), server.base_url()};

  const auto assert_payment_error = [&](const venice::Error& error) {
    REQUIRE(error.kind == ErrorKind::PaymentRequired);
    REQUIRE(error.status == 402);
    REQUIRE(error.message == "HTTP 402");
    REQUIRE(error.body == R"({"code":"PAYMENT_REQUIRED","message":"top up this wallet"})");
    REQUIRE(error.metadata.payment_required == "base64-payment-requirements");
    REQUIRE(error.metadata.header("x-protocol-trace") == "retained-on-error");
    REQUIRE(error.message.find(secret) == std::string::npos);
    REQUIRE(error.body.find(secret) == std::string::npos);
  };

  const auto buffered = client.chat(minimal_chat());
  REQUIRE_FALSE(buffered.has_value());
  assert_payment_error(buffered.error());

  venice::StreamAccumulator acc;
  const auto streamed = client.chat_stream(minimal_chat(), acc);
  REQUIRE_FALSE(streamed.has_value());
  assert_payment_error(streamed.error());
  REQUIRE(acc.empty());
}

// ── buffered substrate: failure matrix first (VC-22) ──────────────────────────

TEST_CASE("redirect responses are returned without contacting another origin",
          "[transport][redirect][buffered][failure]") {
  const RedirectFixture fixture;
  const std::array statuses{301, 302, 303, 307, 308};

  for (const int status : statuses) {
    const auto response = venice::detail::send_buffered(
        fixture.origin_base_url(),
        {.method = venice::detail::HttpMethod::Post,
         .endpoint = "/transport/redirect?status=" + std::to_string(status),
         .headers = {{"Authorization", "Bearer redirect-secret"},
                     {"SIGN-IN-WITH-X", "redirect-siwx"},
                     {"PAYMENT-SIGNATURE", "redirect-payment"},
                     {"Idempotency-Key", "redirect-idempotency"}},
         .body = venice::detail::ByteBody{"paid request body", "application/json"}});

    REQUIRE(response.has_value());
    REQUIRE(response->status == status);
    REQUIRE(response->body == "redirect blocked");
    REQUIRE(response->headers.find("Location") != response->headers.end());
    REQUIRE(fixture.destination_hits() == 0);

    constexpr std::array<std::string_view, 1> kJson{"application/json"};
    const auto classified = venice::detail::require_media_type(*response, kJson);
    REQUIRE_FALSE(classified.has_value());
    REQUIRE(classified.error().kind == ErrorKind::Http);
    REQUIRE(classified.error().status == status);
    REQUIRE(classified.error().body == "redirect blocked");
    REQUIRE(classified.error().metadata.header("x-protocol-trace") ==
            "origin-redirect");
  }

  const Client client{"redirect-secret", fixture.origin_base_url()};
  const auto models = client.models();
  REQUIRE_FALSE(models.has_value());
  REQUIRE(models.error().kind == ErrorKind::Http);
  REQUIRE(models.error().status == 307);
  REQUIRE(models.error().body == "redirect blocked");
  REQUIRE(models.error().metadata.header("location").has_value());
  REQUIRE(models.error().metadata.header("x-protocol-trace") ==
          "origin-redirect");
  REQUIRE(fixture.destination_hits() == 0);
}

TEST_CASE("redirects cannot replay multipart bodies or remain enabled on one origin",
          "[transport][redirect][multipart][failure]") {
  const RedirectFixture fixture;

  const auto multipart = venice::detail::send_buffered(
      fixture.origin_base_url(),
      {.method = venice::detail::HttpMethod::Post,
       .endpoint = "/transport/redirect?status=307",
       .headers = {{"Authorization", "Bearer redirect-secret"}},
       .body = venice::detail::MultipartBody{{
           {.name = "file",
            .bytes = std::string{"owned\0upload", 12},
            .filename = "private.bin",
            .content_type = "application/octet-stream"},
       }}});
  REQUIRE(multipart.has_value());
  REQUIRE(multipart->status == 307);
  REQUIRE(multipart->body == "redirect blocked");
  REQUIRE(fixture.destination_hits() == 0);

  const auto same_origin = venice::detail::send_buffered(
      fixture.origin_base_url(),
      {.method = venice::detail::HttpMethod::Get,
       .endpoint = "/transport/redirect?status=302&same-origin=1",
       .headers = {{"Authorization", "Bearer redirect-secret"}}});
  REQUIRE(same_origin.has_value());
  REQUIRE(same_origin->status == 302);
  REQUIRE(same_origin->body == "redirect blocked");
  REQUIRE(fixture.same_origin_hits() == 0);
}

TEST_CASE("streaming APIs classify origin redirects without following them",
          "[transport][redirect][stream][failure]") {
  const RedirectFixture fixture;
  const Client client{"redirect-secret", fixture.origin_base_url()};

  venice::StreamAccumulator acc;
  const auto chat = client.chat_stream(
      minimal_chat(), acc, {}, {.idempotency_key = "redirect-idempotency"});
  REQUIRE_FALSE(chat.has_value());
  REQUIRE(chat.error().kind == ErrorKind::Http);
  REQUIRE(chat.error().status == 307);
  REQUIRE(chat.error().body == "redirect blocked");
  REQUIRE(chat.error().metadata.header("location").has_value());
  REQUIRE(chat.error().metadata.header("x-protocol-trace") ==
          "origin-redirect");
  REQUIRE(acc.empty());

  const auto speech = client.generate_speech_stream(
      venice::SpeechRequest{.input = "private speech input"}, {});
  REQUIRE_FALSE(speech.has_value());
  REQUIRE(speech.error().kind == ErrorKind::Http);
  REQUIRE(speech.error().status == 308);
  REQUIRE(speech.error().body == "redirect blocked");
  REQUIRE(speech.error().metadata.header("location").has_value());
  REQUIRE(speech.error().metadata.header("x-protocol-trace") ==
          "origin-redirect");

  REQUIRE(fixture.destination_hits() == 0);
}

TEST_CASE("buffered JSON decoding rejects missing and wrong success content types",
          "[transport][buffered][failure]") {
  const TestServer server;

  for (const std::string endpoint : {"/transport/missing-type", "/transport/wrong-type"}) {
    const auto response = venice::detail::send_buffered(
        server.base_url(), {.method = venice::detail::HttpMethod::Get, .endpoint = endpoint});
    REQUIRE(response.has_value());

    const auto decoded = venice::detail::decode_json(*response);
    REQUIRE_FALSE(decoded.has_value());
    REQUIRE(decoded.error().kind == ErrorKind::Parse);
    REQUIRE(decoded.error().status == 200);
    REQUIRE(decoded.error().body == R"({"ok":true})");
    REQUIRE(decoded.error().metadata.header("x-protocol-trace") == "parse-metadata");
  }
}

TEST_CASE("buffered JSON decoding rejects malformed JSON after accepting its media type",
          "[transport][buffered][failure]") {
  const TestServer server;
  const auto response = venice::detail::send_buffered(
      server.base_url(),
      {.method = venice::detail::HttpMethod::Get, .endpoint = "/transport/malformed-json"});
  REQUIRE(response.has_value());

  const auto decoded = venice::detail::decode_json(*response);
  REQUIRE_FALSE(decoded.has_value());
  REQUIRE(decoded.error().kind == ErrorKind::Parse);
  REQUIRE(decoded.error().status == 200);
  REQUIRE(decoded.error().body == "{");
  REQUIRE(decoded.error().metadata.header("x-protocol-trace") == "parse-metadata");
}

TEST_CASE("a non-success status wins over an unexpected content type",
          "[transport][buffered][failure]") {
  const TestServer server;
  const auto response = venice::detail::send_buffered(
      server.base_url(),
      {.method = venice::detail::HttpMethod::Get, .endpoint = "/transport/http-error"});
  REQUIRE(response.has_value());

  const auto decoded = venice::detail::decode_json(*response);
  REQUIRE_FALSE(decoded.has_value());
  REQUIRE(decoded.error().kind == ErrorKind::Http);
  REQUIRE(decoded.error().status == 418);
  REQUIRE(decoded.error().body == "not JSON and that does not matter");
  REQUIRE(decoded.error().metadata.header("x-protocol-trace") == "http-metadata");
}

TEST_CASE("multipart is rejected on methods the audited contract never uses",
          "[transport][buffered][multipart][failure]") {
  const TestServer server;
  const auto response = venice::detail::send_buffered(
      server.base_url(),
      {.method = venice::detail::HttpMethod::Patch,
       .endpoint = "/transport/multipart",
       .body = venice::detail::MultipartBody{}});

  REQUIRE_FALSE(response.has_value());
  REQUIRE(response.error().kind == ErrorKind::InvalidArg);
}

TEST_CASE("every buffered HTTP method shares headers path body and response capture",
          "[transport][buffered]") {
  const TestServer server;

  struct Case {
    venice::detail::HttpMethod method;
    std::string_view name;
    std::string bytes;
  };
  const std::array cases{
      Case{venice::detail::HttpMethod::Get, "GET", {}},
      Case{venice::detail::HttpMethod::Post, "POST", std::string{"post\0bytes", 10}},
      Case{venice::detail::HttpMethod::Patch, "PATCH", "patch bytes"},
      Case{venice::detail::HttpMethod::Delete, "DELETE", "delete bytes"},
  };

  for (const auto& item : cases) {
    venice::detail::BufferedRequest request{
        .method = item.method,
        .endpoint = "/transport/echo?mode=exact%20path",
        .headers = {{"X-Test-Request", "caller-value"}},
    };
    if (item.method != venice::detail::HttpMethod::Get)
      request.body = venice::detail::ByteBody{item.bytes, "application/octet-stream"};

    const auto response = venice::detail::send_buffered(server.base_url(), request);
    REQUIRE(response.has_value());
    REQUIRE(response->status == 200);
    REQUIRE(response->content_type == "application/json");
    REQUIRE(response->headers.find("X-Test-Response") != response->headers.end());

    const auto decoded = venice::detail::decode_json(*response);
    REQUIRE(decoded.has_value());
    REQUIRE((*decoded)["method"] == item.name);
    REQUIRE((*decoded)["target"] == "/api/v1/transport/echo?mode=exact%20path");
    REQUIRE((*decoded)["x_test"] == "caller-value");
    REQUIRE((*decoded)["body"].get<std::string>() == item.bytes);
    if (item.method == venice::detail::HttpMethod::Get)
      REQUIRE((*decoded)["content_type"] == "");
    else
      REQUIRE((*decoded)["content_type"] == "application/octet-stream");
  }
}

TEST_CASE("a structured JSON suffix and media-type parameters are accepted",
          "[transport][buffered]") {
  const TestServer server;
  const auto response = venice::detail::send_buffered(
      server.base_url(),
      {.method = venice::detail::HttpMethod::Get, .endpoint = "/transport/vendor-json"});
  REQUIRE(response.has_value());
  REQUIRE(response->content_type == "application/problem+json");

  const auto decoded = venice::detail::decode_json(*response);
  REQUIRE(decoded.has_value());
  REQUIRE((*decoded)["kind"] == "problem");
}

TEST_CASE("buffered text CSV and binary responses retain metadata and exact bytes",
          "[transport][buffered]") {
  const TestServer server;

  const auto text = venice::detail::send_buffered(
      server.base_url(), {.method = venice::detail::HttpMethod::Get, .endpoint = "/transport/text"});
  REQUIRE(text.has_value());
  REQUIRE(text->content_type == "text/plain");
  REQUIRE(text->body == "plain response");
  const auto text_header = text->headers.find("X-Test-Response");
  REQUIRE(text_header != text->headers.end());
  REQUIRE(text_header->second == "text");
  const std::array text_types{std::string_view{"text/plain"}};
  REQUIRE(venice::detail::require_media_type(*text, text_types).has_value());

  const auto csv = venice::detail::send_buffered(
      server.base_url(), {.method = venice::detail::HttpMethod::Get, .endpoint = "/transport/csv"});
  REQUIRE(csv.has_value());
  REQUIRE(csv->content_type == "text/csv");
  REQUIRE(csv->body == "a,b\n1,2\n");
  const std::array csv_types{std::string_view{"text/csv"}};
  REQUIRE(venice::detail::require_media_type(*csv, csv_types).has_value());

  const auto binary = venice::detail::send_buffered(
      server.base_url(),
      {.method = venice::detail::HttpMethod::Get, .endpoint = "/transport/binary"});
  REQUIRE(binary.has_value());
  REQUIRE(binary->content_type == "application/octet-stream");
  REQUIRE(binary->body == std::string{"\0\1\xFFx", 4});
  const auto binary_header = binary->headers.find("X-Test-Response");
  REQUIRE(binary_header != binary->headers.end());
  REQUIRE(binary_header->second == "binary");
  const std::array binary_types{std::string_view{"application/octet-stream"}};
  REQUIRE(venice::detail::require_media_type(*binary, binary_types).has_value());
}

TEST_CASE("multipart preserves repeated names filenames media types and NUL bytes",
          "[transport][buffered][multipart]") {
  const TestServer server;
  const std::string nul_bytes{"left\0right", 10};
  const auto response = venice::detail::send_buffered(
      server.base_url(),
      {.method = venice::detail::HttpMethod::Post,
       .endpoint = "/transport/multipart",
       .headers = {{"X-Test-Request", "multipart"}},
       .body = venice::detail::MultipartBody{{
           {.name = "images", .bytes = nul_bytes, .filename = "one.bin", .content_type = "application/octet-stream"},
           {.name = "images", .bytes = "second", .filename = "two.txt", .content_type = "text/plain"},
           {.name = "prompt", .bytes = "edit this", .filename = {}, .content_type = "text/plain"},
       }}});
  REQUIRE(response.has_value());
  const auto seen_header = response->headers.find("X-Multipart-Seen");
  REQUIRE(seen_header != response->headers.end());
  REQUIRE(seen_header->second == "yes");

  const auto decoded = venice::detail::decode_json(*response);
  REQUIRE(decoded.has_value());
  const auto& parts = decoded->at("parts");
  REQUIRE(parts.size() == 3);
  REQUIRE(parts[0]["name"] == "images");
  REQUIRE(parts[0]["filename"] == "one.bin");
  REQUIRE(parts[0]["content_type"] == "application/octet-stream");
  REQUIRE(parts[0]["content"].get<std::string>() == nul_bytes);
  REQUIRE(parts[1]["name"] == "images");
  REQUIRE(parts[1]["filename"] == "two.txt");
  REQUIRE(parts[1]["content"] == "second");
  REQUIRE(parts[2]["name"] == "prompt");
  REQUIRE(parts[2]["filename"] == "");
  REQUIRE(parts[2]["content"] == "edit this");
}

TEST_CASE("cancellation interrupts the multipart transport path",
          "[transport][buffered][multipart][cancel][failure]") {
  const TestServer server;
  venice::CancelToken token;
  std::thread canceller{[&] {
    while (server.multipart_stall_hits() == 0) std::this_thread::sleep_for(5ms);
    token.cancel();
  }};

  std::expected<venice::detail::BufferedResponse, venice::Error> response;
  const auto elapsed = timed([&] {
    response = venice::detail::send_buffered(
        server.base_url(),
        {.method = venice::detail::HttpMethod::Post,
         .endpoint = "/transport/multipart-stall",
         .body = venice::detail::MultipartBody{{
             {.name = "file", .bytes = std::string(4096, 'x'), .filename = "x.bin", .content_type = "application/octet-stream"},
         }}},
        {.cancel = &token});
  });
  canceller.join();

  REQUIRE_FALSE(response.has_value());
  REQUIRE(response.error().kind == ErrorKind::Cancelled);
  REQUIRE(elapsed < kPromptly);
  REQUIRE(server.multipart_stall_hits() == 1);
}

}  // namespace

// ── §0 a cancelled token refuses the call outright ────────────────────────
//
// A token that has already fired must not produce a request at all. Asserted on
// the server's own hit counter rather than on the clock: "returned fast" would
// also be true of a request that was sent and then aborted, and those are
// different contracts. This is the pre-send check in Client::get_json.

TEST_CASE("a token cancelled before the call sends nothing", "[transport][cancel][failure]") {
  const TestServer server;
  const Client client{"not-a-real-key", server.base_url()};

  venice::CancelToken token;
  token.cancel();

  const auto res = client.balance({.cancel = &token});

  REQUIRE_FALSE(res.has_value());
  REQUIRE(res.error().kind == ErrorKind::Cancelled);
  REQUIRE(server.stall_hits() == 0);
}

// ── §1 the connect timeout is the caller's to set ─────────────────────────
//
// The other half of the timeout story, and the half that cannot use the local
// server: a peer on loopback accepts instantly, so there is no connect to time
// out. 192.0.2.1 is RFC 5737 TEST-NET-1 — reserved for documentation and
// guaranteed never to be routed — which makes it the one address that reliably
// goes nowhere without depending on anybody's firewall.
//
// The assertion is deliberately two-sided-tolerant. A network that blackholes
// the packet gives a connect timeout; one that answers ICMP unreachable gives
// an immediate refusal. Both are ErrorKind::Network and both are fast, so the
// case holds either way rather than encoding an assumption about the runner's
// network. What it must never do is take the 30s default: that is the bug.
//
// Measured locally before it was written: 301ms elapsed for a 300ms setting,
// "transport: Connection timed out".

TEST_CASE("a per-request connect timeout bounds an unreachable peer",
          "[transport][timeout][failure]") {
  const Client client{"not-a-real-key", "http://192.0.2.1:9/api/v1"};

  std::expected<nlohmann::json, venice::Error> res;
  const auto elapsed = timed([&] { res = client.balance({.connect_timeout = 300ms}); });

  REQUIRE_FALSE(res.has_value());
  REQUIRE(res.error().kind == ErrorKind::Network);
  REQUIRE(elapsed < kPromptly);
}

// ── §2 the read timeout is the caller's to set ────────────────────────────
//
// The server accepts and then says nothing. With the library's default this
// call would block for 300s; with an override it must come back on schedule and
// as a Network error, because a timeout *is* a transport failure — nobody
// cancelled anything.

TEST_CASE("a per-request read timeout bounds a stalled response", "[transport][timeout][failure]") {
  const TestServer server;
  const Client client{"not-a-real-key", server.base_url()};

  std::expected<nlohmann::json, venice::Error> res;
  const auto elapsed = timed([&] { res = client.balance({.read_timeout = 300ms}); });

  REQUIRE_FALSE(res.has_value());
  REQUIRE(res.error().kind == ErrorKind::Network);
  REQUIRE(elapsed < kPromptly);
  REQUIRE(server.stall_hits() == 1);  // it really did connect and get stalled on
}

// ── §3 cancel before the first body byte ──────────────────────────────────
//
// The case #7's follow-up comment says a stop-token mapped onto on_token cannot
// satisfy: the request is in flight, the server has sent nothing, and there is
// therefore no callback to say no from. Only shutting the socket down gets out
// of this, and no read timeout is set — the default 300s stands, so returning
// at all is the assertion.

TEST_CASE("cancel reaches a request that has received nothing", "[transport][cancel][failure]") {
  const TestServer server;
  const Client client{"not-a-real-key", server.base_url()};

  venice::CancelToken token;
  std::thread canceller{[&] {
    std::this_thread::sleep_for(150ms);
    token.cancel();
  }};

  std::expected<nlohmann::json, venice::Error> res;
  const auto elapsed = timed([&] { res = client.balance({.cancel = &token}); });
  canceller.join();

  REQUIRE_FALSE(res.has_value());
  REQUIRE(res.error().kind == ErrorKind::Cancelled);
  REQUIRE(elapsed < kPromptly);
  REQUIRE(server.stall_hits() == 1);
}

// ── §4 cancel during a stalled stream ─────────────────────────────────────
//
// Two frames arrive, then the server goes quiet. on_token is never called
// again, so the pre-VC-06 library had no way out but the read timeout. The
// deltas already delivered are asserted too: cancelling must not require
// throwing away what the callback was legitimately handed.

TEST_CASE("cancel reaches a stream stalled between frames", "[transport][cancel][stream][failure]") {
  const TestServer server;
  const Client client{"not-a-real-key", server.base_url()};

  std::vector<std::string> deltas;  // touched only by the calling thread
  std::atomic<int> seen{0};

  venice::CancelToken token;
  std::thread canceller{[&] {
    while (seen.load() < TestServer::kFrames) std::this_thread::sleep_for(5ms);
    token.cancel();
  }};

  std::expected<venice::ChatResponse, venice::Error> res;
  const auto elapsed = timed([&] {
    res = client.chat_stream(
        minimal_chat(),
        [&](std::string_view d) {
          deltas.emplace_back(d);
          seen.fetch_add(1);
          return true;  // never an early stop — the token is the only exit
        },
        {.cancel = &token});
  });
  canceller.join();

  REQUIRE_FALSE(res.has_value());
  REQUIRE(res.error().kind == ErrorKind::Cancelled);
  REQUIRE(elapsed < kPromptly);
  REQUIRE(deltas == std::vector<std::string>{"hel", "lo"});
}

// ── §5 the two ways to stop a stream are not the same thing ───────────────
//
// on_token returning false is the Phase 0 early stop and still returns the
// partial response as *success*. That is the behaviour every existing caller
// has, and adding a cancellation kind must not quietly reinterpret it. Pinned
// here rather than trusted: fold Cancelled onto this path and this case is the
// one that goes red.

TEST_CASE("on_token returning false is still partial success, not Cancelled",
          "[transport][stream][failure]") {
  const TestServer server;
  const Client client{"not-a-real-key", server.base_url()};

  const auto res = client.chat_stream(minimal_chat(), [](std::string_view) { return false; });

  REQUIRE(res.has_value());
  REQUIRE(res->content == "hel");
}

// ── §6 an unfired token changes nothing ───────────────────────────────────
//
// The guard installs a watcher thread on every call that passes a token, and
// that thread must be invisible to a call that is never cancelled — no stall on
// teardown, no spurious Cancelled, no lost body. Runs the same request twice to
// catch a guard that leaks state or a socket between calls.

TEST_CASE("a live but unfired token leaves a normal call alone", "[transport][cancel]") {
  const TestServer server;
  const Client client{"not-a-real-key", server.base_url()};

  venice::CancelToken token;

  for (int i = 0; i < 2; ++i) {
    const auto res = client.models("", {.cancel = &token});
    REQUIRE(res.has_value());
    REQUIRE(res->size() == 1);
    REQUIRE(res->front().id == "test-model");
  }
  REQUIRE(server.models_hits() == 2);
}

// ── §7 the caller's signal state survives a cancellation ──────────────────
//
// Cancelling shuts a socket down under a live request, and over TLS the
// teardown that follows makes OpenSSL write a close_notify to a peer that is
// gone — EPIPE, therefore SIGPIPE, therefore a dead process at the default
// disposition. detail::SigPipeBlock blocks it for the request and drains it
// afterwards.
//
// ⚠ This case does NOT prove the fix, and pretending otherwise would be worse
// than not having it. Two reasons it cannot: the loopback peer speaks plain
// HTTP, which has no close_notify to write, and constructing TestServer at all
// runs httplib::Server's constructor, which does signal(SIGPIPE, SIG_IGN)
// process-wide. Every test in this file therefore runs with the signal already
// defused. The fix is evidenced by a live TLS run recorded in
// venice/options.hpp — 43ms and Cancelled where the same binary previously
// exited 141.
//
// What *is* checkable here is the other half of the contract, and it is a real
// invariant: whatever we do to the mask, we undo. A leaked block would silently
// swallow the application's own SIGPIPE from then on, and a skipped drain would
// hand them ours the moment they next unblocked. Neither is hypothetical —
// both are what the naive spelling of this class does.

#ifndef _WIN32
TEST_CASE("a cancelled call leaves the signal mask and disposition as it found them",
          "[transport][cancel]") {
  const TestServer server;
  const Client client{"not-a-real-key", server.base_url()};

  // Absolute, not relative to the state on entry. The first spelling of this
  // case compared the mask before against the mask after and stayed green with
  // the restore deliberately deleted: Catch2 had already run a cancelling case
  // ahead of it, that case leaked the block, and "before" was therefore just as
  // wrong as "after". Asserting the mask is *clean* at both ends is what makes
  // the case independent of test order — and turns a leak anywhere in this file
  // red here.
  sigset_t mask_before{};
  pthread_sigmask(SIG_BLOCK, nullptr, &mask_before);
  REQUIRE(sigismember(&mask_before, SIGPIPE) == 0);

  struct sigaction action_before {};
  sigaction(SIGPIPE, nullptr, &action_before);

  venice::CancelToken token;
  std::thread canceller{[&] {
    std::this_thread::sleep_for(50ms);
    token.cancel();
  }};
  const auto res = client.balance({.cancel = &token});
  canceller.join();

  REQUIRE_FALSE(res.has_value());
  REQUIRE(res.error().kind == ErrorKind::Cancelled);

  sigset_t mask_after{};
  pthread_sigmask(SIG_BLOCK, nullptr, &mask_after);
  REQUIRE(sigismember(&mask_after, SIGPIPE) == 0);

  // Nothing left pending for the caller to collect later.
  sigset_t pending{};
  sigpending(&pending);
  REQUIRE(sigismember(&pending, SIGPIPE) == 0);

  // And the disposition itself is untouched — a library changing that behind
  // the application's back is the fix we deliberately did not take.
  struct sigaction action_after {};
  sigaction(SIGPIPE, nullptr, &action_after);
  REQUIRE(action_after.sa_handler == action_before.sa_handler);
}
#endif

// ── §7b a cancelled stream loses the response, not the data (VC-05) ───────
//
// The payoff of making StreamAccumulator the caller's object rather than a
// local inside chat_stream, and the one thing that needs a real socket: the
// frames have to actually arrive, mid-flight, before the cancel lands.
//
// VC-06's contract is unchanged and still right — a cancelled call returns
// ErrorKind::Cancelled and no response, because a caller who has stopped
// waiting should not have to tell "the text so far" from "the answer". What
// changes is that abandoning the call no longer *destroys* what arrived. The
// return value says "you abandoned this"; the accumulator still holds every
// token and every verbatim chunk.
//
// The server writes two content frames and then stalls, so the cancel fires
// during the quiet gap — exactly the shape on_token alone cannot escape, which
// is why #7 existed.

TEST_CASE("a cancelled stream leaves the accumulator holding what arrived",
          "[transport][cancel][stream]") {
  const TestServer server;
  const Client client{"not-a-real-key", server.base_url()};

  venice::CancelToken token;
  venice::StreamAccumulator acc;

  std::expected<venice::ChatResponse, venice::Error> res;
  const auto elapsed = timed([&] {
    res = client.chat_stream(minimal_chat(), acc,
                             [&](const venice::StreamDelta&) {
                               // Cancel once both frames are in. The delta is
                               // already accumulated by the time this runs, so
                               // the frame that triggered the cancel is kept.
                               if (acc.message().text() == "hello") token.cancel();
                               return true;
                             },
                             {.cancel = &token});
  });

  // The call reports the abandonment, and reports it as Cancelled rather than
  // Network — the socket was shut down, so it *arrives* as a transport failure.
  REQUIRE_FALSE(res.has_value());
  REQUIRE(res.error().kind == ErrorKind::Cancelled);
  REQUIRE(elapsed < kPromptly);

  // ...and none of that cost the caller the two frames that did arrive.
  REQUIRE(acc.message().text() == "hello");
  REQUIRE(acc.chunks().size() == TestServer::kFrames);
  REQUIRE_FALSE(acc.empty());
}

// The same object, on the path that is not a cancel: a stream that completes
// normally must put the same thing in the accumulator as it returns.
TEST_CASE("the accumulator and the returned response agree", "[transport][stream]") {
  const TestServer server;
  const Client client{"not-a-real-key", server.base_url()};

  venice::CancelToken token;
  venice::StreamAccumulator acc;

  // The endpoint stalls after its two frames, so stop at the second one rather
  // than waiting the handler out. An on_delta false is a deliberate early stop:
  // partial success, response returned.
  const auto res = client.chat_stream(minimal_chat(), acc,
                                      [&](const venice::StreamDelta&) {
                                        return acc.message().text() != "hello";
                                      },
                                      {.cancel = &token});

  REQUIRE(res.has_value());
  REQUIRE(res->content == "hello");
  REQUIRE(res->content == acc.message().text());
  REQUIRE(nlohmann::json(*res->message) == nlohmann::json(acc.message()));
}

// ── §8 happy path: defaults still work ────────────────────────────────────
//
// RequestOptions is defaulted on every entry point, so the pre-VC-06 call
// spelling has to keep compiling and keep behaving. This case is as much a
// compile-time assertion as a runtime one.

TEST_CASE("default options behave as before", "[transport]") {
  const TestServer server;
  const Client client{"not-a-real-key", server.base_url()};

  const auto res = client.models();

  REQUIRE(res.has_value());
  REQUIRE(res->size() == 1);
  REQUIRE(res->front().id == "test-model");
}

TEST_CASE("buffered chat still sends bearer JSON and parses the typed reply", "[transport]") {
  const TestServer server;
  const Client client{"not-a-real-key", server.base_url()};

  const auto res = client.chat(minimal_chat());

  REQUIRE(res.has_value());
  REQUIRE(res->id == "buffered-chat");
  REQUIRE(res->content == "ok");
}

// ── §9 Audio: media, multipart and explicit async lifecycle (VC-28) ─────

TEST_CASE("malformed Audio success bodies become Parse values at the client boundary",
          "[transport][audio][failure][parse]") {
  const TestServer server;
  const Client client{"audio-key", server.base_url()};
  const venice::AudioFile file{.bytes = std::string{"RIFF\0WAVE", 9},
                               .filename = "sample.wav",
                               .media_type = "audio/wav"};

  const auto transcription = client.transcribe_audio(
      {.file = file, .model = "malformed", .response_format = "json"});
  REQUIRE_FALSE(transcription);
  REQUIRE(transcription.error().is(ErrorKind::Parse));
  REQUIRE(transcription.error().status == 200);

  const auto voice = client.clone_voice({.file = file, .model = "malformed"});
  REQUIRE_FALSE(voice);
  REQUIRE(voice.error().is(ErrorKind::Parse));

  const auto quote = client.quote_audio({.model = "malformed"});
  REQUIRE_FALSE(quote);
  REQUIRE(quote.error().is(ErrorKind::Parse));

  const auto queue = client.queue_audio({.model = "malformed", .prompt = "fixture"});
  REQUIRE_FALSE(queue);
  REQUIRE(queue.error().is(ErrorKind::Parse));

  const auto retrieve = client.retrieve_audio(
      {.model = "music-fixture", .queue_id = "malformed"});
  REQUIRE_FALSE(retrieve);
  REQUIRE(retrieve.error().is(ErrorKind::Parse));

  const auto cleanup = client.cleanup_audio(
      {.model = "music-fixture", .queue_id = "malformed"});
  REQUIRE_FALSE(cleanup);
  REQUIRE(cleanup.error().is(ErrorKind::Parse));
}

TEST_CASE("speech classifies HTTP before media and preserves opaque bytes",
          "[transport][audio][speech]") {
  const TestServer server;
  const Client client{"audio-key", server.base_url()};

  venice::SpeechRequest request{.input = "hello"};
  const auto media = client.generate_speech(request);
  REQUIRE(media.has_value());
  REQUIRE(media->bytes == std::string{"A\0UD", 4});
  REQUIRE(media->media_type == "audio/mpeg");
  REQUIRE(media->metadata.x_balance_remaining == "3.250000");
  REQUIRE(media->metadata.payment_response == "audio-payment-receipt");

  for (const auto& [format, media_type] :
       std::array<std::pair<const char*, const char*>, 5>{{
           {"aac", "audio/aac"},
           {"flac", "audio/flac"},
           {"opus", "audio/opus"},
           {"pcm", "audio/pcm"},
           {"wav", "audio/wav"},
       }}) {
    request.response_format = format;
    const auto alternate = client.generate_speech(request);
    REQUIRE(alternate.has_value());
    REQUIRE(alternate->bytes == std::string{"A\0UD", 4});
    REQUIRE(alternate->media_type == media_type);
  }
  request.response_format.reset();

  request.input = "wrong-media";
  const auto wrong = client.generate_speech(request);
  REQUIRE_FALSE(wrong);
  REQUIRE(wrong.error().is(ErrorKind::Parse));
  REQUIRE(wrong.error().status == 200);
  REQUIRE(wrong.error().body == "not audio");

  request.input = "bad-request";
  const auto bad = client.generate_speech(request);
  REQUIRE_FALSE(bad);
  REQUIRE(bad.error().is(ErrorKind::Http));
  REQUIRE(bad.error().status == 400);
  REQUIRE(bad.error().body == R"({"error":"bad speech request"})");

  const Client wallet{Authentication::sign_in_with_x("audio-needs-payment"),
                      server.base_url()};
  request.input = "hello";
  const auto payment = wallet.generate_speech(request);
  REQUIRE_FALSE(payment);
  REQUIRE(payment.error().is(ErrorKind::PaymentRequired));
  REQUIRE(payment.error().metadata.payment_required ==
          "audio-payment-requirements");
}

TEST_CASE("speech streaming distinguishes completion early stop cancellation and callback failure",
          "[transport][audio][speech][stream][cancel]") {
  const TestServer server;
  const Client client{"audio-key", server.base_url()};
  venice::SpeechRequest request{.input = "hello"};

  request.input = "bad-request";
  const auto bad = client.generate_speech_stream(request, {});
  REQUIRE_FALSE(bad);
  REQUIRE(bad.error().is(ErrorKind::Http));
  REQUIRE(bad.error().status == 400);
  REQUIRE(bad.error().body == R"({"error":"bad speech request"})");

  request.input = "wrong-media";
  const auto wrong = client.generate_speech_stream(request, {});
  REQUIRE_FALSE(wrong);
  REQUIRE(wrong.error().is(ErrorKind::Parse));
  REQUIRE(wrong.error().status == 200);
  REQUIRE(wrong.error().body == "not audio");

  request.input = "hello";

  std::string bytes;
  const auto complete = client.generate_speech_stream(
      request, [&](std::string_view chunk) {
        bytes.append(chunk);
        return true;
      });
  REQUIRE(complete.has_value());
  REQUIRE(complete->completed);
  REQUIRE(complete->media_type == "audio/mpeg");
  REQUIRE(complete->metadata.x_balance_remaining == "3.250000");
  REQUIRE(complete->metadata.payment_response == "audio-payment-receipt");
  REQUIRE(bytes == std::string{"S\0TR", 4});

  bytes.clear();
  const auto stopped = client.generate_speech_stream(
      request, [&](std::string_view chunk) {
        bytes.append(chunk);
        return false;
      });
  REQUIRE(stopped.has_value());
  REQUIRE_FALSE(stopped->completed);
  REQUIRE(bytes == std::string{"S\0", 2});

  venice::CancelToken token;
  request.input = "stall";
  const auto cancelled = client.generate_speech_stream(
      request,
      [&](std::string_view) {
        token.cancel();
        return true;
      },
      {.cancel = &token});
  REQUIRE_FALSE(cancelled);
  REQUIRE(cancelled.error().is(ErrorKind::Cancelled));

  request.input = "hello";
  const auto callback_failure = client.generate_speech_stream(
      request, [](std::string_view) -> bool { throw std::runtime_error{"fixture"}; });
  REQUIRE_FALSE(callback_failure);
  REQUIRE(callback_failure.error().is(ErrorKind::InvalidArg));
  REQUIRE(callback_failure.error().message == "speech callback: fixture");

  const auto empty_callback_failure = client.generate_speech_stream(
      request, [](std::string_view) -> bool { throw std::runtime_error{""}; });
  REQUIRE_FALSE(empty_callback_failure);
  REQUIRE(empty_callback_failure.error().is(ErrorKind::InvalidArg));
  REQUIRE(empty_callback_failure.error().message == "speech callback: ");
}

TEST_CASE("audio multipart preserves NUL bytes filename type and form fields",
          "[transport][audio][multipart]") {
  const TestServer server;
  const Client client{"audio-key", server.base_url()};
  const venice::AudioFile file{.bytes = std::string{"RIFF\0WAVE", 9},
                               .filename = "sample.wav",
                               .media_type = "audio/wav"};

  venice::AudioTranscriptionRequest request{
      .file = file,
      .model = "asr-fixture",
      .response_format = "json",
      .timestamps = false,
      .language = "en",
  };
  const auto json = client.transcribe_audio(request);
  REQUIRE(json.has_value());
  const auto* parsed = std::get_if<venice::JsonAudioTranscription>(&*json);
  REQUIRE(parsed != nullptr);
  REQUIRE(parsed->text == "fixture transcript");
  REQUIRE(parsed->raw["seen_file_size"] == 9);
  REQUIRE(parsed->raw["seen_filename"] == "sample.wav");
  REQUIRE(parsed->raw["seen_media_type"] == "audio/wav");
  REQUIRE(parsed->raw["seen_timestamps"] == "false");
  REQUIRE(parsed->raw["seen_language"] == "en");
  REQUIRE(parsed->metadata.x_balance_remaining == "3.000000");

  request.response_format = "text";
  const auto text = client.transcribe_audio(request);
  REQUIRE(text.has_value());
  const auto* plain = std::get_if<venice::TextAudioTranscription>(&*text);
  REQUIRE(plain != nullptr);
  REQUIRE(plain->text == "fixture transcript");
  REQUIRE(plain->media_type == "text/plain");

  for (const auto& [model, status] :
       std::array<std::pair<const char*, int>, 3>{{{"payload-too-large", 413},
                                                   {"unsupported-media", 415},
                                                   {"validation-error", 422}}}) {
    request.model = model;
    const auto result = client.transcribe_audio(request);
    REQUIRE_FALSE(result);
    REQUIRE(result.error().status == status);
  }
  request.model = "wrong-media";
  const auto wrong = client.transcribe_audio(request);
  REQUIRE_FALSE(wrong);
  REQUIRE(wrong.error().is(ErrorKind::Parse));

  venice::AudioVoiceCloneRequest clone{.file = file, .model = "tts-fixture"};
  const auto voice = client.clone_voice(clone);
  REQUIRE(voice.has_value());
  REQUIRE(voice->id == "vv_fixture");
  REQUIRE(voice->model == "tts-fixture");
  REQUIRE(voice->raw["seen_file_size"] == 9);
  REQUIRE(voice->raw["seen_filename"] == "sample.wav");
  REQUIRE(voice->raw["seen_media_type"] == "audio/wav");
}

TEST_CASE("audio async calls keep quote auth and status-media-cleanup states distinct",
          "[transport][audio][async]") {
  const TestServer server;
  const Client bearer{"audio-key", server.base_url()};
  const Client wallet{Authentication::sign_in_with_x("signed-audio-wallet"),
                      server.base_url()};

  const auto quote = bearer.quote_audio({.model = "music-fixture", .character_count = 100});
  REQUIRE(quote.has_value());
  REQUIRE(quote->quote == 0.75);
  REQUIRE(quote->raw["seen_authorization"] == "Bearer audio-key");
  REQUIRE(quote->raw["seen_siwx"] == "");
  const auto quote_wrong_auth = wallet.quote_audio({.model = "music-fixture"});
  REQUIRE_FALSE(quote_wrong_auth);
  REQUIRE(quote_wrong_auth.error().is(ErrorKind::InvalidArg));

  const auto queued = wallet.queue_audio({.model = "music-fixture",
                                          .prompt = "fixture prompt",
                                          .force_instrumental = false});
  REQUIRE(queued.has_value());
  REQUIRE(queued->queue_id == "queue-fixture");
  REQUIRE(queued->raw["seen_siwx"] == "signed-audio-wallet");
  REQUIRE(queued->raw["seen_body"]["force_instrumental"] == false);

  const auto processing = bearer.retrieve_audio(
      {.model = "music-fixture", .queue_id = "processing"});
  REQUIRE(processing.has_value());
  const auto* status = std::get_if<venice::AudioProcessing>(&*processing);
  REQUIRE(status != nullptr);
  REQUIRE(status->status == "PROCESSING");
  REQUIRE(status->average_execution_time == 20000.0);

  const auto media = bearer.retrieve_audio(
      {.model = "music-fixture", .queue_id = "media"});
  REQUIRE(media.has_value());
  const auto* bytes = std::get_if<venice::AudioMedia>(&*media);
  REQUIRE(bytes != nullptr);
  REQUIRE(bytes->bytes == std::string{"M\0P3", 4});
  REQUIRE(bytes->media_type == "audio/mpeg");

  const auto missing = bearer.retrieve_audio(
      {.model = "music-fixture", .queue_id = "missing"});
  REQUIRE_FALSE(missing);
  REQUIRE(missing.error().status == 404);
  const auto wrong = bearer.retrieve_audio(
      {.model = "music-fixture", .queue_id = "wrong-media"});
  REQUIRE_FALSE(wrong);
  REQUIRE(wrong.error().is(ErrorKind::Parse));

  const auto retryable = bearer.cleanup_audio(
      {.model = "music-fixture", .queue_id = "retry"});
  REQUIRE(retryable.has_value());
  REQUIRE_FALSE(retryable->success);
  REQUIRE(retryable->raw["seen_body"]["queue_id"] == "retry");
  const auto cleaned = bearer.cleanup_audio(
      {.model = "music-fixture", .queue_id = "done"});
  REQUIRE(cleaned.has_value());
  REQUIRE(cleaned->success);
  REQUIRE(server.audio_hits() >= 8);
}

// ── §10 Video: safe quote and explicit async media lifecycle (VC-29) ────

TEST_CASE("malformed Video success bodies become Parse values at the client boundary",
          "[transport][video][failure][parse]") {
  const TestServer server;
  const Client client{"video-key", server.base_url()};

  const auto quote = client.quote_video({.model = "malformed", .duration = "5"});
  REQUIRE_FALSE(quote);
  REQUIRE(quote.error().is(ErrorKind::Parse));
  REQUIRE(quote.error().status == 200);
  const auto invalid_quote = client.quote_video(
      {.model = "invalid-json", .duration = "5"});
  REQUIRE_FALSE(invalid_quote);
  REQUIRE(invalid_quote.error().is(ErrorKind::Parse));

  const auto queue = client.queue_video(
      {.model = "malformed", .prompt = "fixture", .duration = "5"});
  REQUIRE_FALSE(queue);
  REQUIRE(queue.error().is(ErrorKind::Parse));
  const auto invalid_queue = client.queue_video(
      {.model = "invalid-json", .prompt = "fixture", .duration = "5"});
  REQUIRE_FALSE(invalid_queue);
  REQUIRE(invalid_queue.error().is(ErrorKind::Parse));

  const auto retrieve = client.retrieve_video(
      {.model = "video-fixture", .queue_id = "malformed"});
  REQUIRE_FALSE(retrieve);
  REQUIRE(retrieve.error().is(ErrorKind::Parse));
  const auto invalid_retrieve = client.retrieve_video(
      {.model = "video-fixture", .queue_id = "invalid-json"});
  REQUIRE_FALSE(invalid_retrieve);
  REQUIRE(invalid_retrieve.error().is(ErrorKind::Parse));

  const auto cleanup = client.cleanup_video(
      {.model = "video-fixture", .queue_id = "malformed"});
  REQUIRE_FALSE(cleanup);
  REQUIRE(cleanup.error().is(ErrorKind::Parse));
  const auto invalid_cleanup = client.cleanup_video(
      {.model = "video-fixture", .queue_id = "invalid-json"});
  REQUIRE_FALSE(invalid_cleanup);
  REQUIRE(invalid_cleanup.error().is(ErrorKind::Parse));

  const auto transcription = client.transcribe_video({.url = "malformed"});
  REQUIRE_FALSE(transcription);
  REQUIRE(transcription.error().is(ErrorKind::Parse));
  const auto invalid_transcription = client.transcribe_video(
      {.url = "invalid-json"});
  REQUIRE_FALSE(invalid_transcription);
  REQUIRE(invalid_transcription.error().is(ErrorKind::Parse));
}

TEST_CASE("video quote is Bearer-only and queue preserves caller-authored shapes",
          "[transport][video][auth][queue]") {
  const TestServer server;
  const Client bearer{"video-key", server.base_url()};
  const Client wallet{Authentication::sign_in_with_x("signed-video-wallet"),
                      server.base_url()};

  const auto quote = bearer.quote_video({.model = "video-fixture",
                                         .duration = "5",
                                         .aspect_ratio = "16:9",
                                         .audio = false});
  REQUIRE(quote.has_value());
  REQUIRE(quote->quote == 1.25);
  REQUIRE(quote->raw["seen_authorization"] == "Bearer video-key");
  REQUIRE(quote->raw["seen_siwx"] == "");
  REQUIRE(quote->raw["seen_body"]["audio"] == false);
  REQUIRE(quote->metadata.x_balance_remaining == "2.250000");

  const auto quote_wrong_auth = wallet.quote_video(
      {.model = "video-fixture", .duration = "5"});
  REQUIRE_FALSE(quote_wrong_auth);
  REQUIRE(quote_wrong_auth.error().is(ErrorKind::InvalidArg));

  const auto bad_quote = bearer.quote_video(
      {.model = "bad-request", .duration = "5"});
  REQUIRE_FALSE(bad_quote);
  REQUIRE(bad_quote.error().is(ErrorKind::Http));
  REQUIRE(bad_quote.error().status == 400);
  const auto forbidden_quote = bearer.quote_video(
      {.model = "forbidden", .duration = "5"});
  REQUIRE_FALSE(forbidden_quote);
  REQUIRE(forbidden_quote.error().is(ErrorKind::Auth));
  REQUIRE(forbidden_quote.error().status == 403);

  const auto queued = wallet.queue_video(
      {.model = "video-fixture",
       .prompt = "download",
       .duration = "5",
       .consents = nlohmann::json{{"adult_content", false}},
       .softness = 0.0,
       .h264_output = false,
       .reference_image_urls = std::vector<std::string>{"first", "second"},
       .elements = std::vector<nlohmann::json>{
           venice::video_input::element("front", std::vector<std::string>{"ref"}),
           nlohmann::json{{"future_kind", "opaque"}}},
       .keyframes = std::vector<nlohmann::json>{
           venice::video_input::keyframe("start", 0),
           venice::video_input::keyframe("end", 120)}});
  REQUIRE(queued.has_value());
  REQUIRE(queued->model == "video-fixture");
  REQUIRE(queued->queue_id == "video-queue-fixture");
  REQUIRE(queued->download_url == "https://example.test/video.mp4");
  REQUIRE(queued->raw["seen_siwx"] == "signed-video-wallet");
  REQUIRE(queued->raw["seen_body"]["softness"] == 0.0);
  REQUIRE(queued->raw["seen_body"]["h264_output"] == false);
  REQUIRE(queued->raw["seen_body"]["reference_image_urls"] ==
          nlohmann::json::array({"first", "second"}));
  REQUIRE(queued->raw["seen_body"]["elements"][1]["future_kind"] == "opaque");
  REQUIRE(queued->raw["seen_body"]["keyframes"][1]["frame_index"] == 120);
  REQUIRE(queued->metadata.payment_response == "video-payment-receipt");
}

TEST_CASE("video calls classify status before success media and preserve MP4 bytes",
          "[transport][video][retrieve][failure]") {
  const TestServer server;
  const Client client{"video-key", server.base_url()};

  struct QueueCase {
    const char* prompt;
    ErrorKind kind;
    int status;
  };
  const std::array queue_cases{
      QueueCase{"bad-request", ErrorKind::Http, 400},
      QueueCase{"unauthorized", ErrorKind::Auth, 401},
      QueueCase{"payment", ErrorKind::PaymentRequired, 402},
      QueueCase{"forbidden", ErrorKind::Auth, 403},
      QueueCase{"conflict", ErrorKind::Http, 409},
      QueueCase{"too-large", ErrorKind::Http, 413},
      QueueCase{"invalid", ErrorKind::Http, 422},
      QueueCase{"server-error", ErrorKind::Http, 500},
  };
  for (const auto& test : queue_cases) {
    const auto result = client.queue_video(
        {.model = "video-fixture", .prompt = test.prompt, .duration = "5"});
    REQUIRE_FALSE(result);
    REQUIRE(result.error().kind == test.kind);
    REQUIRE(result.error().status == test.status);
    if (test.status == 402)
      REQUIRE(result.error().metadata.payment_required ==
              "video-payment-requirements");
  }

  const auto processing = client.retrieve_video(
      {.model = "video-fixture",
       .queue_id = "processing",
       .delete_media_on_completion = false});
  REQUIRE(processing.has_value());
  const auto* status = std::get_if<venice::VideoProcessing>(&*processing);
  REQUIRE(status != nullptr);
  REQUIRE(status->status == "PROCESSING");
  REQUIRE(status->average_execution_time == 30000.0);
  REQUIRE(status->raw["seen_body"]["delete_media_on_completion"] == false);
  REQUIRE(status->metadata.x_balance_remaining == "2.000000");

  const auto media = client.retrieve_video(
      {.model = "video-fixture", .queue_id = "media"});
  REQUIRE(media.has_value());
  const auto* bytes = std::get_if<venice::VideoMedia>(&*media);
  REQUIRE(bytes != nullptr);
  REQUIRE(bytes->bytes == std::string{"V\0ID", 4});
  REQUIRE(bytes->media_type == "video/mp4");

  for (const auto& [queue_id, status_code] :
       std::array<std::pair<const char*, int>, 4>{{{"missing", 404},
                                                   {"payment", 402},
                                                   {"invalid", 422},
                                                   {"capacity", 503}}}) {
    const auto result = client.retrieve_video(
        {.model = "video-fixture", .queue_id = queue_id});
    REQUIRE_FALSE(result);
    REQUIRE(result.error().status == status_code);
  }
  const auto wrong = client.retrieve_video(
      {.model = "video-fixture", .queue_id = "wrong-media"});
  REQUIRE_FALSE(wrong);
  REQUIRE(wrong.error().is(ErrorKind::Parse));
  REQUIRE(wrong.error().body == "wrong");
}

TEST_CASE("video transcription and cleanup retain their result unions and retry state",
          "[transport][video][transcription][cleanup]") {
  const TestServer server;
  const Client client{"video-key", server.base_url()};

  const auto json = client.transcribe_video({.url = "https://example.test/video.mp4"});
  REQUIRE(json.has_value());
  const auto* parsed = std::get_if<venice::JsonVideoTranscription>(&*json);
  REQUIRE(parsed != nullptr);
  REQUIRE(parsed->transcript == "fixture video transcript");
  REQUIRE(parsed->language == "en");
  REQUIRE(parsed->metadata.x_balance_remaining == "1.750000");

  const auto text = client.transcribe_video(
      {.url = "https://example.test/video.mp4", .response_format = "text"});
  REQUIRE(text.has_value());
  const auto* plain = std::get_if<venice::TextVideoTranscription>(&*text);
  REQUIRE(plain != nullptr);
  REQUIRE(plain->text == "fixture video transcript");
  REQUIRE(plain->media_type == "text/plain");

  struct TranscriptionCase {
    const char* url;
    ErrorKind kind;
    int status;
  };
  const std::array transcription_cases{
      TranscriptionCase{"bad-request", ErrorKind::Http, 400},
      TranscriptionCase{"unauthorized", ErrorKind::Auth, 401},
      TranscriptionCase{"payment", ErrorKind::PaymentRequired, 402},
      TranscriptionCase{"forbidden", ErrorKind::Auth, 403},
      TranscriptionCase{"rate-limited", ErrorKind::RateLimited, 429},
      TranscriptionCase{"server-error", ErrorKind::Http, 500},
  };
  for (const auto& test : transcription_cases) {
    const auto result = client.transcribe_video({.url = test.url});
    REQUIRE_FALSE(result);
    REQUIRE(result.error().kind == test.kind);
    REQUIRE(result.error().status == test.status);
  }
  const auto wrong = client.transcribe_video({.url = "wrong-media"});
  REQUIRE_FALSE(wrong);
  REQUIRE(wrong.error().is(ErrorKind::Parse));

  const auto retryable = client.cleanup_video(
      {.model = "video-fixture", .queue_id = "retry"});
  REQUIRE(retryable.has_value());
  REQUIRE_FALSE(retryable->success);
  REQUIRE(retryable->raw["seen_body"]["queue_id"] == "retry");
  const auto cleaned = client.cleanup_video(
      {.model = "video-fixture", .queue_id = "done"});
  REQUIRE(cleaned.has_value());
  REQUIRE(cleaned->success);
  struct CleanupCase {
    const char* queue_id;
    ErrorKind kind;
    int status;
  };
  const std::array cleanup_cases{
      CleanupCase{"bad-request", ErrorKind::Http, 400},
      CleanupCase{"unauthorized", ErrorKind::Auth, 401},
      CleanupCase{"payment", ErrorKind::PaymentRequired, 402},
      CleanupCase{"server-error", ErrorKind::Http, 500},
  };
  for (const auto& test : cleanup_cases) {
    const auto result = client.cleanup_video(
        {.model = "video-fixture", .queue_id = test.queue_id});
    REQUIRE_FALSE(result);
    REQUIRE(result.error().kind == test.kind);
    REQUIRE(result.error().status == test.status);
    if (test.status == 402)
      REQUIRE(result.error().metadata.payment_required == "video-cleanup-payment");
  }
}

TEST_CASE("cancellation interrupts a stalled video retrieval",
          "[transport][video][cancel][failure]") {
  const TestServer server;
  const Client client{"video-key", server.base_url()};
  venice::CancelToken token;
  std::thread canceller{[&] {
    while (server.video_stall_hits() == 0) std::this_thread::sleep_for(5ms);
    token.cancel();
  }};

  std::expected<venice::VideoRetrieveResult, venice::Error> result;
  const auto elapsed = timed([&] {
    result = client.retrieve_video(
        {.model = "video-fixture", .queue_id = "stall"}, {.cancel = &token});
  });
  canceller.join();
  REQUIRE_FALSE(result);
  REQUIRE(result.error().is(ErrorKind::Cancelled));
  REQUIRE(elapsed < kPromptly);
  REQUIRE(server.video_stall_hits() == 1);
  REQUIRE(server.video_hits() == 1);
}

TEST_CASE("augment endpoints reject impossible auth modes before the socket",
          "[transport][augment][auth][failure]") {
  const TestServer server;
  const Client public_client{Authentication::public_access(),
                             server.base_url()};
  const Client payment_client{Authentication::x402_payment("signed-payment"),
                              server.base_url()};
  const venice::DocumentParseRequest document{
      .file = {.bytes = "fixture",
               .filename = "fixture.txt",
               .media_type = "text/plain"}};

  REQUIRE_FALSE(public_client.parse_document(document));
  REQUIRE_FALSE(public_client.scrape_web({.url = "https://example.test"}));
  REQUIRE_FALSE(payment_client.search_web({.query = "fixture"}));
  REQUIRE(server.augment_hits() == 0);
}

TEST_CASE("document parsing preserves multipart bytes and selects actual "
          "response media",
          "[transport][augment][document]") {
  const TestServer server;
  const Client bearer{"augment-key", server.base_url()};
  const Client wallet{Authentication::sign_in_with_x("signed-augment-wallet"),
                      server.base_url()};
  const auto bytes = std::string{"doc\0bytes", 9};
  venice::DocumentParseRequest request{.file = {.bytes = bytes,
                                                .filename = "fixture.txt",
                                                .media_type = "text/plain"},
                                       .response_format = "json"};

  const auto json = bearer.parse_document(request);
  REQUIRE(json.has_value());
  const auto *parsed = std::get_if<venice::JsonDocumentParse>(&*json);
  REQUIRE(parsed != nullptr);
  REQUIRE(parsed->text == "fixture document");
  REQUIRE(parsed->tokens == 2.5);
  REQUIRE(parsed->raw["seen_file_size"] == bytes.size());
  REQUIRE(parsed->raw["seen_filename"] == "fixture.txt");
  REQUIRE(parsed->raw["seen_media_type"] == "text/plain");
  REQUIRE(parsed->raw["seen_authorization"] == "Bearer augment-key");
  REQUIRE(parsed->metadata.x_balance_remaining == "1.500000");
  REQUIRE(parsed->metadata.payment_response == "augment-payment-receipt");

  request.response_format = "text";
  const auto text = wallet.parse_document(request);
  REQUIRE(text.has_value());
  const auto *plain = std::get_if<venice::TextDocumentParse>(&*text);
  REQUIRE(plain != nullptr);
  REQUIRE(plain->text == std::string{"fixture\0text", 12});
  REQUIRE(plain->media_type == "text/plain");
  REQUIRE(plain->metadata.x_balance_remaining == "1.500000");
  REQUIRE(server.augment_hits() == 2);
}

TEST_CASE("scrape and search post exact modeled-wins JSON with Bearer or SIWX",
          "[transport][augment][json][auth]") {
  const TestServer server;
  const Client bearer{"augment-key", server.base_url()};
  const Client wallet{Authentication::sign_in_with_x("signed-augment-wallet"),
                      server.base_url()};

  const auto scraped =
      wallet.scrape_web({.url = "https://example.test",
                         .extra = {{"url", "shadow"}, {"future", 1}}});
  REQUIRE(scraped.has_value());
  REQUIRE(scraped->url == "https://example.test");
  REQUIRE(scraped->content == "# Fixture");
  REQUIRE(scraped->format == "markdown");
  REQUIRE(scraped->raw["seen_body"]["future"] == 1);
  REQUIRE(scraped->raw["seen_siwx"] == "signed-augment-wallet");
  REQUIRE(scraped->metadata.x_balance_remaining == "1.250000");

  const auto searched =
      bearer.search_web({.query = "fixture",
                         .limit = 0,
                         .search_provider = "future-provider",
                         .extra = {{"query", "shadow"}, {"future", true}}});
  REQUIRE(searched.has_value());
  REQUIRE(searched->query == "fixture");
  REQUIRE(searched->returned == 1);
  REQUIRE(searched->results.size() == 1);
  REQUIRE(searched->results[0].title == "Fixture");
  REQUIRE(searched->results[0].raw["future"] == true);
  REQUIRE(searched->raw["seen_body"]["limit"] == 0);
  REQUIRE(searched->raw["seen_body"]["search_provider"] == "future-provider");
  REQUIRE(searched->raw["seen_authorization"] == "Bearer augment-key");
  REQUIRE(searched->metadata.x_balance_remaining == "1.000000");
}

TEST_CASE("augment status errors win over media and successful shape failures "
          "are parse errors",
          "[transport][augment][failure]") {
  const TestServer server;
  const Client client{"augment-key", server.base_url()};
  const venice::DocumentFile file{.bytes = "fixture",
                                  .filename = "fixture.txt",
                                  .media_type = "text/plain"};

  struct StatusCase {
    int status;
    ErrorKind kind;
  };
  const std::array status_cases{
      StatusCase{400, ErrorKind::Http},
      StatusCase{401, ErrorKind::Auth},
      StatusCase{402, ErrorKind::PaymentRequired},
      StatusCase{403, ErrorKind::Auth},
      StatusCase{415, ErrorKind::Http},
      StatusCase{429, ErrorKind::RateLimited},
      StatusCase{500, ErrorKind::Http},
  };
  for (const auto &test : status_cases) {
    const auto result = client.parse_document(
        {.file = file,
         .response_format = "status-" + std::to_string(test.status)});
    REQUIRE_FALSE(result);
    REQUIRE(result.error().kind == test.kind);
    REQUIRE(result.error().status == test.status);
    REQUIRE(result.error().body == R"({"error":"document parse failed"})");
    REQUIRE(result.error().metadata.header("x-protocol-trace") ==
            "augment-error");
    if (test.status == 402)
      REQUIRE(result.error().metadata.payment_required ==
              "augment-payment-requirements");
  }

  for (const std::string control :
       {"wrong-media", "invalid-json", "malformed"}) {
    CAPTURE(control);
    const auto result =
        client.parse_document({.file = file, .response_format = control});
    REQUIRE_FALSE(result);
    REQUIRE(result.error().is(ErrorKind::Parse));
    REQUIRE(result.error().status == 200);
  }

  for (const std::string control :
       {"wrong-media", "invalid-json", "malformed"}) {
    const auto scrape = client.scrape_web({.url = control});
    REQUIRE_FALSE(scrape);
    REQUIRE(scrape.error().is(ErrorKind::Parse));
    const auto search = client.search_web({.query = control});
    REQUIRE_FALSE(search);
    REQUIRE(search.error().is(ErrorKind::Parse));
  }
}

TEST_CASE("cancellation interrupts a stalled document parse",
          "[transport][augment][cancel][failure]") {
  const TestServer server;
  const Client client{"augment-key", server.base_url()};
  venice::CancelToken token;
  std::thread canceller{[&] {
    while (server.augment_stall_hits() == 0)
      std::this_thread::sleep_for(5ms);
    token.cancel();
  }};

  std::expected<venice::DocumentParseResult, venice::Error> result;
  const auto elapsed = timed([&] {
    result = client.parse_document({.file = {.bytes = "fixture",
                                             .filename = "fixture.txt",
                                             .media_type = "text/plain"},
                                    .response_format = "stall"},
                                   {.cancel = &token});
  });
  canceller.join();
  REQUIRE_FALSE(result);
  REQUIRE(result.error().is(ErrorKind::Cancelled));
  REQUIRE(elapsed < kPromptly);
  REQUIRE(server.augment_stall_hits() == 1);
  REQUIRE(server.augment_hits() == 1);
}

TEST_CASE("crypto RPC rejects impossible auth and structure before the socket",
          "[transport][crypto-rpc][auth][failure]") {
  const TestServer server;
  const auto request = venice::crypto_rpc_input::request(
      "eth_chainId", nlohmann::json::array(), nlohmann::json(1));
  const Client public_client{Authentication::public_access(), server.base_url()};
  const Client payment_client{Authentication::x402_payment("signed-payment"),
                              server.base_url()};
  const Client bearer{"rpc-key", server.base_url()};

  REQUIRE_FALSE(public_client.crypto_rpc("ethereum-mainnet", request));
  REQUIRE_FALSE(payment_client.crypto_rpc("ethereum-mainnet", request));
  REQUIRE_FALSE(bearer.crypto_rpc("", request));
  REQUIRE_FALSE(bearer.crypto_rpc("ethereum-mainnet", nlohmann::json("bad")));
  REQUIRE(server.crypto_rpc_hits() == 0);
}

TEST_CASE("crypto RPC discovery is public tolerant and metadata-preserving",
          "[transport][crypto-rpc][networks]") {
  const TestServer server;
  const Client public_client{Authentication::public_access(), server.base_url()};
  const Client bearer{"rpc-key", server.base_url()};

  const auto public_networks = public_client.crypto_rpc_networks();
  REQUIRE(public_networks.has_value());
  REQUIRE(public_networks->networks ==
          std::vector<std::string>{"ethereum-mainnet", "solana-mainnet"});
  REQUIRE(public_networks->raw["seen_authorization"] == "");
  REQUIRE(public_networks->metadata.header("x-protocol-trace") ==
          "crypto-networks");

  const auto bearer_networks = bearer.crypto_rpc_networks();
  REQUIRE(bearer_networks.has_value());
  REQUIRE(bearer_networks->raw["seen_authorization"] == "Bearer rpc-key");

  for (const std::string control : {"wrong-media", "invalid-json", "malformed"}) {
    const auto result = public_client.crypto_rpc_networks(
        {.idempotency_key = control});
    REQUIRE_FALSE(result);
    REQUIRE(result.error().is(ErrorKind::Parse));
  }
}

TEST_CASE("crypto RPC sends exact path body auth and idempotency header",
          "[transport][crypto-rpc][request]") {
  const TestServer server;
  const Client bearer{"rpc-key", server.base_url()};
  const Client wallet{Authentication::sign_in_with_x("signed-rpc-wallet"),
                      server.base_url()};
  const auto request = venice::crypto_rpc_input::request(
      "future_method", nlohmann::json{{"future", true}},
      nlohmann::json("request-a"));

  const auto response = bearer.crypto_rpc(
      "future/net?x#%", request, {.idempotency_key = "fixture-key_1"});
  REQUIRE(response.has_value());
  const auto* item =
      std::get_if<venice::CryptoRpcResponseItem>(&response->payload);
  REQUIRE(item != nullptr);
  REQUIRE(item->id == "request-a");
  REQUIRE(item->result.has_value());
  REQUIRE(item->result->at("seen_target") ==
          "/api/v1/crypto/rpc/future%2Fnet%3Fx%23%25");
  REQUIRE(item->result->at("seen_body") == request);
  REQUIRE(item->result->at("seen_raw_body") == request.dump());
  REQUIRE(item->result->at("seen_authorization") == "Bearer rpc-key");
  REQUIRE(item->result->at("seen_siwx") == "");
  REQUIRE(item->result->at("seen_idempotency") == "fixture-key_1");
  REQUIRE_FALSE(request.contains("Idempotency-Key"));
  REQUIRE(response->metadata.x_balance_remaining == "4.230000");
  REQUIRE(response->metadata.header("X-Venice-RPC-Credits") == "20");
  REQUIRE(response->metadata.header("X-Venice-RPC-Cost-USD") ==
          "0.00001400");
  REQUIRE(response->metadata.header("X-Request-ID") == "fixture-request-id");
  REQUIRE(response->metadata.header("Idempotent-Replayed") == "true");

  const auto wallet_response = wallet.crypto_rpc("ethereum-mainnet", request);
  REQUIRE(wallet_response.has_value());
  const auto* wallet_item =
      std::get_if<venice::CryptoRpcResponseItem>(&wallet_response->payload);
  REQUIRE(wallet_item != nullptr);
  REQUIRE(wallet_item->result->at("seen_authorization") == "");
  REQUIRE(wallet_item->result->at("seen_siwx") == "signed-rpc-wallet");
}

TEST_CASE("crypto RPC preserves batch order and HTTP-200 application errors",
          "[transport][crypto-rpc][response]") {
  const TestServer server;
  const Client client{"rpc-key", server.base_url()};
  const auto first = venice::crypto_rpc_input::request(
      "eth_chainId", nlohmann::json::array(), nlohmann::json(1));
  const auto second = venice::crypto_rpc_input::request(
      "future_method", nlohmann::json::array(), nlohmann::json("two"));

  const auto batch = client.crypto_rpc(
      "ethereum-mainnet", venice::crypto_rpc_input::batch({first, second}));
  REQUIRE(batch.has_value());
  const auto* items =
      std::get_if<std::vector<venice::CryptoRpcResponseItem>>(&batch->payload);
  REQUIRE(items != nullptr);
  REQUIRE(items->size() == 2);
  REQUIRE(items->at(0).id == 1);
  REQUIRE(items->at(0).result.has_value());
  REQUIRE(items->at(0).result->at("seen_raw_body") ==
          venice::crypto_rpc_input::batch({first, second}).dump());
  REQUIRE(items->at(1).id == "two");
  REQUIRE(items->at(1).error->at("code") == -32602);

  const auto rpc_error = client.crypto_rpc("rpc-error", first);
  REQUIRE(rpc_error.has_value());
  const auto* error_item =
      std::get_if<venice::CryptoRpcResponseItem>(&rpc_error->payload);
  REQUIRE(error_item != nullptr);
  REQUIRE(error_item->error->at("code") == -32601);
}

TEST_CASE("crypto RPC status errors win over media and shape failures parse",
          "[transport][crypto-rpc][failure]") {
  const TestServer server;
  const Client client{"rpc-key", server.base_url()};
  const auto request = venice::crypto_rpc_input::request(
      "eth_chainId", nlohmann::json::array(), nlohmann::json(1));
  struct StatusCase {
    int status;
    ErrorKind kind;
  };
  const std::array statuses{StatusCase{400, ErrorKind::Http},
                            StatusCase{401, ErrorKind::Auth},
                            StatusCase{402, ErrorKind::PaymentRequired},
                            StatusCase{429, ErrorKind::RateLimited},
                            StatusCase{500, ErrorKind::Http}};
  for (const auto& test : statuses) {
    const auto result = client.crypto_rpc(
        "status-" + std::to_string(test.status), request);
    REQUIRE_FALSE(result);
    REQUIRE(result.error().kind == test.kind);
    REQUIRE(result.error().status == test.status);
    REQUIRE(result.error().body == R"({"error":"proxy refused"})");
    REQUIRE(result.error().metadata.header("x-protocol-trace") ==
            "crypto-rpc-error");
    if (test.status == 402)
      REQUIRE(result.error().metadata.payment_required ==
              "crypto-payment-requirements");
  }

  for (const std::string network : {"wrong-media", "invalid-json", "malformed"}) {
    const auto result = client.crypto_rpc(network, request);
    REQUIRE_FALSE(result);
    REQUIRE(result.error().is(ErrorKind::Parse));
    REQUIRE(result.error().status == 200);
  }
}

TEST_CASE("cancellation interrupts a stalled crypto RPC call",
          "[transport][crypto-rpc][cancel][failure]") {
  const TestServer server;
  const Client client{"rpc-key", server.base_url()};
  venice::CancelToken token;
  std::thread canceller{[&] {
    while (server.crypto_rpc_stall_hits() == 0)
      std::this_thread::sleep_for(5ms);
    token.cancel();
  }};

  std::expected<venice::CryptoRpcResponse, venice::Error> result;
  const auto elapsed = timed([&] {
    result = client.crypto_rpc(
        "stall",
        venice::crypto_rpc_input::request(
            "eth_chainId", nlohmann::json::array(), nlohmann::json(1)),
        {.cancel = &token});
  });
  canceller.join();
  REQUIRE_FALSE(result);
  REQUIRE(result.error().is(ErrorKind::Cancelled));
  REQUIRE(elapsed < kPromptly);
  REQUIRE(server.crypto_rpc_stall_hits() == 1);
}

TEST_CASE("x402 endpoints reject impossible auth and empty wallets before the socket",
          "[transport][x402][auth][failure]") {
  const TestServer server;
  const Client public_client{Authentication::public_access(), server.base_url()};
  const Client bearer{"bearer-secret", server.base_url()};
  const Client wallet{Authentication::sign_in_with_x("signed-wallet"),
                      server.base_url()};
  const Client payment{Authentication::x402_payment("signed-payment"),
                       server.base_url()};

  REQUIRE_FALSE(public_client.x402_balance("0xabc"));
  REQUIRE_FALSE(bearer.x402_balance("0xabc"));
  REQUIRE_FALSE(payment.x402_balance("0xabc"));
  REQUIRE_FALSE(wallet.x402_balance(""));

  REQUIRE_FALSE(bearer.x402_top_up());
  REQUIRE_FALSE(wallet.x402_top_up());

  REQUIRE_FALSE(public_client.x402_transactions("0xabc"));
  REQUIRE_FALSE(bearer.x402_transactions("0xabc"));
  REQUIRE_FALSE(payment.x402_transactions("0xabc"));
  REQUIRE_FALSE(wallet.x402_transactions(""));
  REQUIRE(server.x402_hits() == 0);
}

TEST_CASE("x402 top-up sends an empty public discovery or one payment header",
          "[transport][x402][top-up]") {
  const TestServer server;
  const Client public_client{Authentication::public_access(), server.base_url()};

  const auto discovery = public_client.x402_top_up();
  REQUIRE(discovery.has_value());
  const auto* requirements =
      std::get_if<venice::X402PaymentRequirements>(&*discovery);
  REQUIRE(requirements != nullptr);
  REQUIRE(requirements->x402_version == 2);
  REQUIRE(requirements->accepts.size() == 1);
  REQUIRE(requirements->accepts.front().amount == "5000000");
  REQUIRE(requirements->metadata.payment_required == "opaque-requirements");
  REQUIRE(requirements->raw.at("fixture") == "public-discovery");

  const auto public_capture = server.last_x402();
  REQUIRE(public_capture.method == "POST");
  REQUIRE(public_capture.path == "/api/v1/x402/top-up");
  REQUIRE(public_capture.content_type.empty());
  REQUIRE(public_capture.authorization.empty());
  REQUIRE(public_capture.siwx.empty());
  REQUIRE(public_capture.payment.empty());
  REQUIRE(public_capture.body.empty());

  const std::string signed_payment = "signed-payment-secret";
  const auto receipt = public_client.x402_top_up(
      {.authentication = Authentication::x402_payment(signed_payment)});
  REQUIRE(receipt.has_value());
  const auto* paid = std::get_if<venice::X402TopUpReceipt>(&*receipt);
  REQUIRE(paid != nullptr);
  REQUIRE(paid->wallet_address == "0xnormalized");
  REQUIRE(paid->amount_credited == 10.0);
  REQUIRE(paid->new_balance == 22.5);
  REQUIRE(paid->payment_id == "payment-1");
  REQUIRE(paid->metadata.payment_response == "opaque-settlement");
  REQUIRE(paid->raw.dump().find(signed_payment) == std::string::npos);

  const auto paid_capture = server.last_x402();
  REQUIRE(paid_capture.method == "POST");
  REQUIRE(paid_capture.content_type.empty());
  REQUIRE(paid_capture.authorization.empty());
  REQUIRE(paid_capture.siwx.empty());
  REQUIRE(paid_capture.payment == signed_payment);
  REQUIRE(paid_capture.body.empty());
  REQUIRE(server.x402_hits() == 2);
}

TEST_CASE("x402 wallet reads send canonical SIWX with encoded path and query",
          "[transport][x402][wallet]") {
  const TestServer server;
  const std::string signed_siwx = "signed-siwx-secret";
  const Client wallet{Authentication::sign_in_with_x(signed_siwx),
                      server.base_url()};

  const auto balance = wallet.x402_balance("wallet/with space");
  REQUIRE(balance.has_value());
  REQUIRE(balance->wallet_address == "0xnormalized");
  REQUIRE(balance->balance_usd == 12.5);
  REQUIRE(balance->metadata.x_balance_remaining == "12.500000");
  REQUIRE(balance->raw.at("seen_target") ==
          "/api/v1/x402/balance/wallet%2Fwith%20space");
  auto capture = server.last_x402();
  REQUIRE(capture.path == "/api/v1/x402/balance/wallet%2Fwith%20space");
  REQUIRE(capture.siwx == signed_siwx);
  REQUIRE(capture.authorization.empty());
  REQUIRE(capture.payment.empty());

  venice::X402TransactionsQuery query;
  query.limit = 2;
  query.offset = 1;
  query.extra = {{"future", "a/b"}};
  const auto page = wallet.x402_transactions("wallet/with space", query);
  REQUIRE(page.has_value());
  REQUIRE(page->entries.size() == 1);
  REQUIRE(page->entries.front().id ==
          std::optional<std::string>{"ledger-1"});
  REQUIRE(page->pagination->limit == std::optional<int>{2});
  REQUIRE(page->raw.at("seen_target") ==
          "/api/v1/x402/transactions/wallet%2Fwith%20space?limit=2&offset=1&future=a%2Fb");
  capture = server.last_x402();
  REQUIRE(capture.path ==
          "/api/v1/x402/transactions/wallet%2Fwith%20space?limit=2&offset=1&future=a%2Fb");
  REQUIRE(capture.siwx == signed_siwx);
  REQUIRE(capture.body.empty());
}

TEST_CASE("x402 status errors precede media validation and accepted statuses parse",
          "[transport][x402][failure]") {
  const TestServer server;
  const Client wallet{Authentication::sign_in_with_x("signed-wallet"),
                      server.base_url()};
  const Client public_client{Authentication::public_access(), server.base_url()};

  struct StatusCase {
    int status;
    ErrorKind kind;
  };
  const std::array wallet_statuses{
      StatusCase{400, ErrorKind::Http}, StatusCase{401, ErrorKind::Auth},
      StatusCase{403, ErrorKind::Auth}, StatusCase{429, ErrorKind::RateLimited},
      StatusCase{500, ErrorKind::Http}};
  for (const auto& test : wallet_statuses) {
    const auto balance =
        wallet.x402_balance("status-" + std::to_string(test.status));
    REQUIRE_FALSE(balance);
    REQUIRE(balance.error().kind == test.kind);
    REQUIRE(balance.error().status == test.status);
    REQUIRE(balance.error().body == R"({"error":"x402 refused"})");
    REQUIRE(balance.error().metadata.header("X-Protocol-Trace") ==
            "x402-error");

    const auto transactions =
        wallet.x402_transactions("status-" + std::to_string(test.status));
    REQUIRE_FALSE(transactions);
    REQUIRE(transactions.error().kind == test.kind);
    REQUIRE(transactions.error().status == test.status);
  }

  for (const std::string control : {"wrong-media", "invalid-json", "malformed"}) {
    const auto balance = wallet.x402_balance(control);
    REQUIRE_FALSE(balance);
    REQUIRE(balance.error().is(ErrorKind::Parse));
    REQUIRE(balance.error().status == 200);
    const auto transactions = wallet.x402_transactions(control);
    REQUIRE_FALSE(transactions);
    REQUIRE(transactions.error().is(ErrorKind::Parse));
    REQUIRE(transactions.error().status == 200);
  }

  for (const int status : {400, 429, 500}) {
    const auto result = public_client.x402_top_up(
        {.idempotency_key = "status-" + std::to_string(status)});
    REQUIRE_FALSE(result);
    REQUIRE(result.error().status == status);
    REQUIRE(result.error().body == R"({"error":"x402 refused"})");
    REQUIRE(result.error().metadata.header("X-Protocol-Trace") ==
            "x402-error");
  }
  for (const std::string control : {"wrong-media-402", "invalid-json-402",
                                    "malformed-402", "wrong-media-200",
                                    "invalid-json-200", "malformed-200"}) {
    const auto result =
        public_client.x402_top_up({.idempotency_key = control});
    REQUIRE_FALSE(result);
    REQUIRE(result.error().is(ErrorKind::Parse));
    REQUIRE(result.error().status ==
            (control.ends_with("402") ? 402 : 200));
  }
}

TEST_CASE("cancellation and timeout interrupt a stalled x402 discovery",
          "[transport][x402][cancel][timeout][failure]") {
  {
    const TestServer server;
    const Client client{Authentication::public_access(), server.base_url()};
    venice::CancelToken token;
    std::thread canceller{[&] {
      while (server.x402_stall_hits() == 0)
        std::this_thread::sleep_for(5ms);
      token.cancel();
    }};

    std::expected<venice::X402TopUpResult, venice::Error> result;
    const auto elapsed = timed([&] {
      result = client.x402_top_up(
          {.cancel = &token, .idempotency_key = "stall"});
    });
    canceller.join();
    REQUIRE_FALSE(result);
    REQUIRE(result.error().is(ErrorKind::Cancelled));
    REQUIRE(elapsed < kPromptly);
    REQUIRE(server.x402_stall_hits() == 1);
  }

  {
    const TestServer server;
    const Client client{Authentication::public_access(), server.base_url()};
    const auto started = std::chrono::steady_clock::now();
    const auto result = client.x402_top_up(
        {.read_timeout = 100ms, .idempotency_key = "stall"});
    const auto elapsed = std::chrono::steady_clock::now() - started;
    REQUIRE_FALSE(result);
    REQUIRE(result.error().is(ErrorKind::Network));
    REQUIRE(elapsed < kPromptly);
    REQUIRE(server.x402_stall_hits() == 1);
  }
}

TEST_CASE("Responses API uses buffered JSON transport and preserves metadata",
          "[transport][responses]") {
  const TestServer server;
  const Client bearer{"test-key", server.base_url()};

  auto request = minimal_response();
  request.extra["stream"] = true;
  const auto response = bearer.create_response(request);
  REQUIRE(response.has_value());
  REQUIRE(response->output_text() == "ok");
  REQUIRE(response->raw.at("seen_stream") == false);
  REQUIRE(response->raw.at("seen_authorization") == "Bearer test-key");
  REQUIRE(response->metadata.x_balance_remaining == "3.210000");
  REQUIRE(response->metadata.header("x-protocol-trace") == "responses-fixture");
  REQUIRE(server.responses_hits() == 1);

  const Client siwx{Authentication::sign_in_with_x("signed-proof"), server.base_url()};
  const auto signed_response = siwx.create_response(minimal_response());
  REQUIRE(signed_response.has_value());
  REQUIRE(signed_response->raw.at("seen_siwx") == "signed-proof");
  REQUIRE(signed_response->raw.at("seen_authorization") == "");
}

TEST_CASE("Responses status classification precedes media and success parsing stays loud",
          "[transport][responses][failure]") {
  const TestServer server;
  const Client client{"test-key", server.base_url()};

  struct StatusCase {
    int status;
    ErrorKind kind;
  };
  const std::array cases{StatusCase{400, ErrorKind::Http},
                         StatusCase{401, ErrorKind::Auth},
                         StatusCase{402, ErrorKind::PaymentRequired},
                         StatusCase{429, ErrorKind::RateLimited},
                         StatusCase{500, ErrorKind::Http}};
  for (const auto& test : cases) {
    auto request = minimal_response();
    request.model = "status-" + std::to_string(test.status);
    const auto result = client.create_response(request);
    REQUIRE_FALSE(result);
    REQUIRE(result.error().kind == test.kind);
    REQUIRE(result.error().status == test.status);
    REQUIRE(result.error().body == R"({"error":"responses refused"})");
    REQUIRE(result.error().metadata.header("x-protocol-trace") == "responses-fixture");
  }

  for (const std::string model : {"wrong-media", "invalid-json", "malformed"}) {
    auto request = minimal_response();
    request.model = model;
    const auto result = client.create_response(request);
    REQUIRE_FALSE(result);
    REQUIRE(result.error().kind == ErrorKind::Parse);
    REQUIRE(result.error().status == 200);
    REQUIRE(result.error().metadata.x_balance_remaining == "3.210000");
  }
}

TEST_CASE("Responses rejects unsupported authentication before a socket",
          "[transport][responses][auth][failure]") {
  const TestServer server;
  const Client public_client{Authentication::public_access(), server.base_url()};
  const auto result = public_client.create_response(minimal_response());
  REQUIRE_FALSE(result);
  REQUIRE(result.error().kind == ErrorKind::InvalidArg);
  REQUIRE(server.responses_hits() == 0);
}
