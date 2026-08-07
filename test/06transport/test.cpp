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

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>

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
    m_svr.Get("/api/v1/api_keys/rate_limits",
              [this](const httplib::Request&, httplib::Response& res) {
                ++m_stall_hits;
                m_gate.wait(kStallCap);
                res.set_content("{}", "application/json");
              });

    m_svr.Get("/api/v1/models", [this](const httplib::Request&, httplib::Response& res) {
      ++m_models_hits;
      res.set_content(R"({"data":[{"id":"test-model","type":"text"}]})", "application/json");
    });

    m_svr.Post("/api/v1/chat/completions",
               [this](const httplib::Request&, httplib::Response& res) {
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
