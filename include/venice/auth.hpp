#pragma once

// venice-cpp — explicit request authentication (VC-23, #38).
//
// Credentials are transport metadata, never request JSON.  The library accepts
// already-produced SIWX and x402 payloads, but deliberately does not own wallet
// keys, signatures, or USDC transaction construction.

#include <string>
#include <string_view>
#include <utility>

namespace venice {

enum class AuthenticationKind {
  Public,
  Bearer,
  SignInWithX,
  X402Payment,
};

class Authentication {
 public:
  [[nodiscard]] static auto public_access() -> Authentication {
    return Authentication{AuthenticationKind::Public, {}};
  }

  [[nodiscard]] static auto bearer(std::string token) -> Authentication {
    return Authentication{AuthenticationKind::Bearer, std::move(token)};
  }

  [[nodiscard]] static auto sign_in_with_x(std::string payload) -> Authentication {
    return Authentication{AuthenticationKind::SignInWithX, std::move(payload)};
  }

  [[nodiscard]] static auto x402_payment(std::string payload) -> Authentication {
    return Authentication{AuthenticationKind::X402Payment, std::move(payload)};
  }

  [[nodiscard]] auto kind() const noexcept -> AuthenticationKind { return m_kind; }
  [[nodiscard]] auto value() const noexcept -> std::string_view { return m_value; }

 private:
  AuthenticationKind m_kind;
  std::string m_value;

  explicit Authentication(AuthenticationKind kind, std::string value)
      : m_kind(kind), m_value(std::move(value)) {}
};

}  // namespace venice
