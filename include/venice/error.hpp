#pragma once

// venice-cpp — error model.
//
// Every fallible operation returns std::expected<T, Error>. The library never
// throws across its public API and never aborts on a transport/parse failure;
// the failure is a value the caller can inspect, log, or degrade on.

#include <cstdint>
#include <string>
#include <string_view>

namespace venice {

// Broad category so a caller can branch on *kind* of failure without parsing
// message text.
//
// Cancelled is deliberately not folded into Network even though both surface as
// a socket that stopped working. A dead network is a fault to report, retry or
// degrade on; a cancellation is the caller's own decision arriving back at them,
// and nothing about it should be logged as a failure. Collapsing the two would
// make that distinction unavailable except by parsing the message — the exact
// thing this enum exists to avoid. Note also that Cancelled is *not* what a
// streaming caller gets for stopping via on_token: that is a deliberate early
// stop and still returns the partial ChatResponse (see Client::chat_stream).
enum class ErrorKind {
  Network,      // connect/TLS/timeout — no HTTP response at all
  Http,         // got a response, non-2xx status (see status + body)
  Parse,        // response wasn't the JSON shape we expected
  Auth,         // 401/403 — key missing/invalid/insufficient
  RateLimited,  // 429
  InvalidArg,   // caller passed something we can't send (e.g. empty prompt)
  Cancelled,    // aborted through a RequestOptions::cancel token (VC-06)
};

struct Error {
  ErrorKind kind{ErrorKind::Network};
  int status{0};                 // HTTP status when kind is Http/Auth/RateLimited
  std::string message;           // human-readable summary
  std::string body;              // raw response body when available (for logs)

  [[nodiscard]] auto is(ErrorKind k) const noexcept -> bool { return kind == k; }
};

[[nodiscard]] inline auto to_string(ErrorKind k) noexcept -> std::string_view {
  switch (k) {
    case ErrorKind::Network: return "network";
    case ErrorKind::Http: return "http";
    case ErrorKind::Parse: return "parse";
    case ErrorKind::Auth: return "auth";
    case ErrorKind::RateLimited: return "rate_limited";
    case ErrorKind::InvalidArg: return "invalid_arg";
    case ErrorKind::Cancelled: return "cancelled";
  }
  return "unknown";
}

// Map an HTTP status to a kind (429/401/403 get their own, else generic http).
[[nodiscard]] inline auto kind_for_status(int status) noexcept -> ErrorKind {
  if (status == 429) return ErrorKind::RateLimited;
  if (status == 401 || status == 403) return ErrorKind::Auth;
  return ErrorKind::Http;
}

}  // namespace venice
