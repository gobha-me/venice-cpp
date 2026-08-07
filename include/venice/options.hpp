#pragma once

// venice-cpp — per-request transport options and cancellation (VC-06, #7).
//
// Before this header every call was pinned to the transport defaults baked into
// Client::make_transport — 300s read, 30s connect — and a non-streaming call
// could not be aborted at all. A caller that wanted to stop waiting had no move
// but to abandon the thread.
//
//   venice::CancelToken tok;
//   std::thread ui{[&] { /* user hits Esc */ tok.cancel(); }};
//   auto res = client.chat(req, {.read_timeout = 30s, .cancel = &tok});
//   // res.error().kind == ErrorKind::Cancelled
//
// Cancellation necessarily comes from a *different* thread than the call: the
// call is blocked in the transport, so nothing on that thread can run. That
// shapes everything below.

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

#include <httplib.h>

namespace venice {

namespace detail {
class CancelGuard;
}

// A one-shot, sticky cancellation flag shared between the calling thread and
// whichever thread decides to abort.
//
// Sticky and one-shot on purpose — there is no reset(). A reusable token would
// have to be reset between calls, and that reset races the request it is trying
// not to affect: a cancel arriving a moment late would silently apply to the
// *next* call instead. A token is two words and a mutex; construct one per call
// that needs one.
//
// Neither copyable nor movable: a mutex and a condition_variable are neither,
// and a token that could move out from under a running CancelGuard would be a
// dangling reference rather than a convenience.
//
// cancel() and cancelled() are safe to call from any thread, including
// concurrently with the request the token is attached to — that is the entire
// point. Neither is noexcept: locking a mutex is allowed to throw
// std::system_error, and marking these noexcept would convert that into
// std::terminate for a promise the standard does not make.
class CancelToken {
 public:
  CancelToken() = default;
  ~CancelToken() = default;

  CancelToken(const CancelToken&) = delete;
  auto operator=(const CancelToken&) -> CancelToken& = delete;
  CancelToken(CancelToken&&) = delete;
  auto operator=(CancelToken&&) -> CancelToken& = delete;

  // Request cancellation. Idempotent; returns immediately.
  //
  // Notably this does *not* touch the socket itself. Doing so here would block
  // the caller — see detail::CancelGuard for the measurement behind that.
  void cancel() {
    {
      const std::lock_guard<std::mutex> lock{m_mu};
      m_cancelled = true;
    }
    m_cv.notify_all();
  }

  [[nodiscard]] auto cancelled() const -> bool {
    const std::lock_guard<std::mutex> lock{m_mu};
    return m_cancelled;
  }

 private:
  friend class detail::CancelGuard;

  mutable std::mutex m_mu;
  std::condition_variable m_cv;
  bool m_cancelled = false;
};

// Per-request transport overrides. Every field is optional; an unset one keeps
// the client default (300s read / 30s connect, httplib's own write default).
//
// A separate parameter rather than fields on ChatRequest, for the reason
// ChatRequest has no `stream` member (AGENTS.md): none of this is part of the
// request. A timeout is a property of *this call*, is not serialized, and
// applies identically to models() and balance(), which have no request object
// at all.
//
// `cancel` is a borrowed raw pointer, not a shared_ptr. The token must outlive
// the call — which is trivially true for the only sane usage, a token on the
// stack of whatever owns both threads — and a shared_ptr would put an
// allocation and an atomic refcount on every caller to model an ownership
// transfer that never happens.
//
// Every member carries an explicit default initializer, including the three
// std::optionals that would default-construct empty anyway. That is not
// redundancy — it is what makes the designated-initializer spelling this type
// is designed for usable at all. Without them, GCC's
// -Wmissing-field-initializers (on in this project's toolchain) fires three
// times on `{.cancel = &token}`, once per member the caller sensibly left out.
// Measured, not assumed: the first build of test/06transport/ produced eighteen
// of these, and a warning that every correct caller trips is an API defect
// rather than a caller's problem.
struct RequestOptions {
  std::optional<std::chrono::milliseconds> connect_timeout = std::nullopt;
  std::optional<std::chrono::milliseconds> read_timeout = std::nullopt;
  std::optional<std::chrono::milliseconds> write_timeout = std::nullopt;
  CancelToken* cancel = nullptr;
};

namespace detail {

// RAII: watches a CancelToken for the lifetime of one request and shuts the
// transport's socket down if it fires. Constructing one with a null token costs
// nothing and spawns no thread, which is why every entry point can install one
// unconditionally.
//
// Two facts read out of the vendored httplib.h (v0.18.3) determine this shape,
// and neither is guessable from the public API:
//
//   1. ClientImpl::send_ holds socket_mutex_ only around setup and the
//      scope_exit teardown — *not* during the request I/O. So Client::stop(),
//      which takes that mutex and calls shutdown_socket, interrupts a stalled
//      read promptly. httplib's own comment on stop() says shutting the socket
//      down is the only thread-safe thing available while a request is in
//      flight; this is that, not a trick.
//
//   2. create_and_connect_socket runs *under* the same mutex. So a stop() that
//      lands during the connect phase blocks its caller until the connect
//      finishes or times out. That is why this is a thread and not a callback
//      fired from CancelToken::cancel(): a UI thread asking to cancel must not
//      block for the connect timeout to do it.
//
// The retry loop is the non-decorative part. stop() is only *effective* once
// send_ has incremented socket_requests_in_flight_; a stop() that lands before
// that sees a closed socket, does nothing, and the request then proceeds
// happily uncancelled. Retrying until the guard is released closes that window.
// The other window needs no retry and is the one worth knowing: a stop() that
// arrives mid-connect is parked on socket_mutex_ and therefore acquires it the
// instant setup releases it, i.e. immediately after the in-flight counter goes
// up — so it lands exactly where it needs to.
//
// The guard's own "released" flag lives under the *token's* mutex rather than
// getting a second mutex and condvar. One wait point, one notify, and a
// destructor that cannot miss a wakeup. It does mean several guards can share
// one token: notify_all wakes them all and each re-tests its own flag.
class CancelGuard {
 public:
  CancelGuard(CancelToken* token, httplib::Client& cli) : m_token(token) {
    if (m_token == nullptr) return;
    m_thread = std::thread{[this, &cli] { watch(cli); }};
  }

  ~CancelGuard() { release(); }

  CancelGuard(const CancelGuard&) = delete;
  auto operator=(const CancelGuard&) -> CancelGuard& = delete;
  CancelGuard(CancelGuard&&) = delete;
  auto operator=(CancelGuard&&) -> CancelGuard& = delete;

  // True if the token exists and has fired. Entry points use this both before
  // the call (to skip a request nobody wants) and after it (to tell a socket we
  // shut down ourselves from a network that failed on its own).
  [[nodiscard]] auto cancelled() const -> bool {
    return m_token != nullptr && m_token->cancelled();
  }

 private:
  // How long to wait between stop() attempts once cancellation is observed.
  // Only ever reached in the narrow case where cancel() beat the socket into
  // existence, and each wait ends early the moment the request returns, so this
  // is a bound on wasted latency and not a poll interval in any steady state.
  static constexpr auto kRetryInterval = std::chrono::milliseconds{2};

  void watch(httplib::Client& cli) {
    {
      std::unique_lock<std::mutex> lock{m_token->m_mu};
      m_token->m_cv.wait(lock, [this] { return m_token->m_cancelled || m_released; });
      if (!m_token->m_cancelled) return;  // request finished first; nothing to do
    }

    for (;;) {
      cli.stop();
      std::unique_lock<std::mutex> lock{m_token->m_mu};
      if (m_token->m_cv.wait_for(lock, kRetryInterval, [this] { return m_released; })) return;
    }
  }

  void release() {
    if (!m_thread.joinable()) return;
    {
      const std::lock_guard<std::mutex> lock{m_token->m_mu};
      m_released = true;
    }
    m_token->m_cv.notify_all();
    m_thread.join();
  }

  CancelToken* m_token;
  std::thread m_thread;
  bool m_released = false;  // guarded by m_token->m_mu
};

}  // namespace detail

}  // namespace venice
