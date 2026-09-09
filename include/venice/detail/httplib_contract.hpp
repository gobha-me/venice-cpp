#pragma once

// One cpp-httplib definition must serve every translation unit in the process.
// This is a compatibility check, not a private macro override or transport API.
#include <httplib.h>
#include <string_view>

#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#error "venice-cpp requires cpp-httplib with OpenSSL support"
#endif
#if defined(CPPHTTPLIB_WOLFSSL_SUPPORT) || defined(CPPHTTPLIB_MBEDTLS_SUPPORT)
#error "venice-cpp requires the OpenSSL-only cpp-httplib TLS profile"
#endif
#ifdef CPPHTTPLIB_USE_NON_BLOCKING_GETADDRINFO
#error "venice-cpp requires the synchronous cpp-httplib resolver profile"
#endif
#ifdef CPPHTTPLIB_NO_EXCEPTIONS
#error "venice-cpp requires cpp-httplib exception support"
#endif
#if !defined(OPENSSL_VERSION_MAJOR) || OPENSSL_VERSION_MAJOR < 3
#error "venice-cpp requires OpenSSL 3 or newer"
#endif

static_assert([] {
  constexpr std::string_view version{CPPHTTPLIB_VERSION};
  if (!version.starts_with("0.51.") || version.size() == 5) return false;
  for (const char digit : version.substr(5))
    if (digit < '0' || digit > '9') return false;
  return true;
}(), "venice-cpp requires the tested cpp-httplib 0.51 API");
static_assert(CPPHTTPLIB_HEADER_MAX_LENGTH == 8192 &&
                  CPPHTTPLIB_HEADER_MAX_COUNT == 100 &&
                  CPPHTTPLIB_MAX_LINE_LENGTH == 32768,
              "venice-cpp requires the canonical cpp-httplib header limits");

// This implementation-only type is absent from upstream's split/compiled header.
// Target metadata alone cannot detect an interface wrapper around that ABI.
static_assert(sizeof(httplib::detail::SSLSocketStream) != 0,
              "venice-cpp requires the unsplit header-only cpp-httplib source");
