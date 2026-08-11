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
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <venice/venice.hpp>

using venice::ChatRequest;
using venice::Client;
using venice::ErrorKind;
using venice::Message;
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
  TestServer() {
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
              [this](const httplib::Request&, httplib::Response& res) {
                ++m_stall_hits;
                m_gate.wait(kStallCap);
                res.set_content("{}", "application/json");
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
                          {"authorization", req.get_header_value("Authorization")}}},
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
                          {"authorization", req.get_header_value("Authorization")}}},
                        {"object", "list"},
                        {"type", "text"}}
                        .dump(),
                    "application/json");
              });

    m_svr.Get("/api/v1/models", [this](const httplib::Request& req, httplib::Response& res) {
      ++m_models_hits;
      const nlohmann::json model{{"id", "test-model"},
                                 {"type", "text"},
                                 {"authorization", req.get_header_value("Authorization")},
                                 {"siwx", req.get_header_value("SIGN-IN-WITH-X")},
                                 {"payment", req.get_header_value("PAYMENT-SIGNATURE")}};
      res.set_content(nlohmann::json{{"data", nlohmann::json::array({model})}}.dump(),
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
  [[nodiscard]] auto character_hits() const -> int { return m_character_hits.load(); }
  [[nodiscard]] auto review_hits() const -> int { return m_review_hits.load(); }
  [[nodiscard]] auto traits_hits() const -> int { return m_traits_hits.load(); }
  [[nodiscard]] auto compat_hits() const -> int { return m_compat_hits.load(); }
  [[nodiscard]] auto multipart_stall_hits() const -> int { return m_multipart_stall_hits.load(); }

  static constexpr int kFrames = 2;
  static constexpr const char* kDelta[kFrames] = {"hel", "lo"};

 private:
  static constexpr auto kStallCap = 10s;

  httplib::Server m_svr;
  std::thread m_thread;
  Gate m_gate;
  int m_port = 0;
  std::atomic<int> m_stall_hits{0};
  std::atomic<int> m_models_hits{0};
  std::atomic<int> m_chat_hits{0};
  std::atomic<int> m_character_hits{0};
  std::atomic<int> m_review_hits{0};
  std::atomic<int> m_traits_hits{0};
  std::atomic<int> m_compat_hits{0};
  std::atomic<int> m_multipart_stall_hits{0};
};

auto minimal_chat() -> ChatRequest {
  ChatRequest r;
  r.model = "test-model";
  r.messages = {Message::user("hi")};
  return r;
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

  const Client empty_bearer{"", server.base_url()};
  const auto empty_models = empty_bearer.models();
  REQUIRE_FALSE(empty_models.has_value());
  REQUIRE(empty_models.error().kind == ErrorKind::InvalidArg);
  REQUIRE(server.models_hits() == 0);
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

// ── the catalogue sub-paths (VC-38, #59) ──────────────────────────────────
//
// Failure matrix first, as everywhere. What is only visible from here — and not
// from test/09catalogue/ — is the target that actually went on the wire, the
// header the client chose to send, and how a non-2xx status is classified.

TEST_CASE("the catalogue calls reject impossible auth modes before the socket",
          "[transport][auth][catalogue][failure]") {
  const TestServer server;

  const Client siwx_client{Authentication::sign_in_with_x("signed-wallet"), server.base_url()};
  const auto siwx_traits = siwx_client.model_traits();
  REQUIRE_FALSE(siwx_traits.has_value());
  REQUIRE(siwx_traits.error().kind == ErrorKind::InvalidArg);

  const Client payment_client{Authentication::x402_payment("payment-payload"),
                              server.base_url()};
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

  // Nothing reached the peer in any of the four.
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
  REQUIRE(*bare->find("target") == "/api/v1/models/traits");

  const auto filtered = client.model_traits("image");
  REQUIRE(filtered.has_value());
  REQUIRE(*filtered->find("target") == "/api/v1/models/traits?type=image");

  // A value needing encoding. The pair separators must survive as data rather
  // than becoming structure — otherwise a filter could address a different query.
  const auto encoded = client.model_traits("a b&type=admin");
  REQUIRE(encoded.has_value());
  REQUIRE(*encoded->find("target") == "/api/v1/models/traits?type=a%20b%26type%3Dadmin");

  const auto compat = client.model_compatibility_mapping("text");
  REQUIRE(compat.has_value());
  REQUIRE(*compat->find("target") == "/api/v1/models/compatibility_mapping?type=text");

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

  // These are the first endpoints where public is the *expected* mode rather
  // than a tolerated one, so this is the assertion that the header is absent
  // rather than empty-but-present.
  const auto traits = public_client.model_traits();
  REQUIRE(traits.has_value());
  REQUIRE(traits->find("authorization")->empty());

  const auto compat = public_client.model_compatibility_mapping();
  REQUIRE(compat.has_value());
  REQUIRE(compat->find("authorization")->empty());

  // Both directions of the per-call override, on both operations.
  const auto bearer_override =
      public_client.model_traits({}, {.authentication = Authentication::bearer("override-token")});
  REQUIRE(bearer_override.has_value());
  REQUIRE(*bearer_override->find("authorization") == "Bearer override-token");

  const Client bearer_client{"default-token", server.base_url()};
  const auto keyed = bearer_client.model_compatibility_mapping();
  REQUIRE(keyed.has_value());
  REQUIRE(*keyed->find("authorization") == "Bearer default-token");

  const auto public_override = bearer_client.model_compatibility_mapping(
      {}, {.authentication = Authentication::public_access()});
  REQUIRE(public_override.has_value());
  REQUIRE(public_override->find("authorization")->empty());
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
