// Transport timeouts and cancellation — VC-06 (#7).
//
// Charter: venice::RequestOptions and venice::CancelToken, i.e. everything
// about *when a call gives up*. What goes on the wire is test/02request/ and
// test/05query/; what comes back off it is test/04models/; what never reaches
// the wire at all is test/03guards/. Nothing here inspects a body.
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
              [](const httplib::Request&, httplib::Response& res) { res.body = R"({"ok":true})"; });
    m_svr.Get("/api/v1/transport/wrong-type", [](const httplib::Request&, httplib::Response& res) {
      res.set_content(R"({"ok":true})", "text/plain");
    });
    m_svr.Get("/api/v1/transport/vendor-json", [](const httplib::Request&, httplib::Response& res) {
      res.set_content(R"({"kind":"problem"})", "Application/Problem+JSON; charset=UTF-8");
    });
    m_svr.Get("/api/v1/transport/malformed-json",
              [](const httplib::Request&, httplib::Response& res) {
                res.set_content("{", "application/json");
              });
    m_svr.Get("/api/v1/transport/http-error", [](const httplib::Request&, httplib::Response& res) {
      res.status = 418;
      res.set_content("not JSON and that does not matter", "text/plain");
    });

    m_svr.Get("/api/v1/api_keys/rate_limits",
              [this](const httplib::Request&, httplib::Response& res) {
                ++m_stall_hits;
                m_gate.wait(kStallCap);
                res.set_content("{}", "application/json");
              });

    m_svr.Get("/api/v1/models", [this](const httplib::Request& req, httplib::Response& res) {
      ++m_models_hits;
      if (req.get_header_value("Authorization") != "Bearer not-a-real-key") {
        res.status = 401;
        res.set_content(R"({"error":"missing test bearer"})", "application/json");
        return;
      }
      res.set_content(R"({"data":[{"id":"test-model","type":"text"}]})", "application/json");
    });

    m_svr.Post("/api/v1/chat/completions",
               [this](const httplib::Request& req, httplib::Response& res) {
                 if (req.get_header_value("Authorization") != "Bearer not-a-real-key") {
                   res.status = 401;
                   res.set_content(R"({"error":"missing test bearer"})", "application/json");
                   return;
                 }
                 if (req.get_header_value("Content-Type") != "application/json") {
                   res.status = 415;
                   res.set_content(R"({"error":"wrong content type"})", "application/json");
                   return;
                 }
                 const auto body = nlohmann::json::parse(req.body);
                 if (!body.at("stream").get<bool>()) {
                   res.set_content(
                       R"({"id":"buffered-chat","choices":[{"message":{"role":"assistant","content":"ok"}}]})",
                       "application/json");
                   return;
                 }
                 // Per-request, not per-server: two tests drive this endpoint,
                 // and a member counter would leave the second one starting
                 // mid-stream. The shared_ptr is what gives the provider — which
                 // outlives this lambda — somewhere to keep it.
                 auto sent = std::make_shared<int>(0);
                 res.set_chunked_content_provider(
                     "text/event-stream",
                     [this, sent](size_t /*offset*/, httplib::DataSink& sink) {
                       if (*sent < kFrames) {
                         const std::string frame = std::string{"data: {\"choices\":[{\"delta\":"} +
                                                   "{\"content\":\"" + kDelta[*sent] + "\"}}]}\n\n";
                         ++*sent;
                         return sink.write(frame.data(), frame.size());
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
