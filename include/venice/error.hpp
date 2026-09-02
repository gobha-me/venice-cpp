#pragma once

// venice-cpp — error model.
//
// Every fallible operation returns std::expected<T, Error>. The library never
// throws across its public API and never aborts on a transport/parse failure;
// the failure is a value the caller can inspect, log, or degrade on.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
  Auth,         // 401/403 — credentials missing, invalid, or forbidden
  PaymentRequired,  // 402 — authenticated or public flow needs payment/balance
  RateLimited,  // 429
  InvalidArg,   // caller passed something we can't send (e.g. empty prompt)
  Cancelled,    // aborted through a RequestOptions::cancel token (VC-06)
  ResponseTooLarge,  // caller-selected decoded response-body ceiling exceeded
};

// Owned response metadata shared by successful inference results and failures.
// Values remain strings: x402 payloads are opaque base64 and balances are
// decimal protocol values whose precision this HTTP client must not reinterpret.
struct ResponseMetadata {
  std::vector<std::pair<std::string, std::string>> headers{};
  std::optional<std::string> x_balance_remaining{};
  std::optional<std::string> payment_required{};
  std::optional<std::string> payment_response{};

  [[nodiscard]] auto header(std::string_view name) const -> std::optional<std::string> {
    for (const auto& [candidate, value] : headers)
      if (ascii_iequal(candidate, name)) return value;
    return std::nullopt;
  }

 private:
  [[nodiscard]] static auto ascii_iequal(std::string_view lhs, std::string_view rhs) -> bool {
    if (lhs.size() != rhs.size()) return false;
    const auto ascii_lower = [](unsigned char value) {
      return value >= 'A' && value <= 'Z' ? static_cast<unsigned char>(value + ('a' - 'A'))
                                         : value;
    };
    for (std::size_t i = 0; i < lhs.size(); ++i) {
      const auto l = static_cast<unsigned char>(lhs[i]);
      const auto r = static_cast<unsigned char>(rhs[i]);
      if (ascii_lower(l) != ascii_lower(r)) return false;
    }
    return true;
  }
};

struct Error {
  ErrorKind kind{ErrorKind::Network};
  int status{0};                 // HTTP status for any response-derived failure
  std::string message;           // human-readable summary
  std::string body;              // raw response body when available (for logs)
  ResponseMetadata metadata{};   // response headers when an HTTP response exists

  [[nodiscard]] auto is(ErrorKind k) const noexcept -> bool { return kind == k; }
};

[[nodiscard]] inline auto to_string(ErrorKind k) noexcept -> std::string_view {
  switch (k) {
    case ErrorKind::Network: return "network";
    case ErrorKind::Http: return "http";
    case ErrorKind::Parse: return "parse";
    case ErrorKind::Auth: return "auth";
    case ErrorKind::PaymentRequired: return "payment_required";
    case ErrorKind::RateLimited: return "rate_limited";
    case ErrorKind::ResponseTooLarge: return "response_too_large";
    case ErrorKind::InvalidArg: return "invalid_arg";
    case ErrorKind::Cancelled: return "cancelled";
  }
  return "unknown";
}

// Map an HTTP status to a kind (402/429/401/403 get their own, else generic HTTP).
[[nodiscard]] inline auto kind_for_status(int status) noexcept -> ErrorKind {
  if (status == 402) return ErrorKind::PaymentRequired;
  if (status == 429) return ErrorKind::RateLimited;
  if (status == 401 || status == 403) return ErrorKind::Auth;
  return ErrorKind::Http;
}

}  // namespace venice
