// A downstream translation unit, in miniature — see CMakeLists.txt in this
// directory. Every check here is about what the *build system* delivered, not
// about what the library computes: this file exists to fail when a usage
// requirement stops travelling to a consumer.
//
// venice-cpp is header-only, so there is no archive to link and no link-time
// proof available. The compile-time assertions below stand in for it. Each one
// has been observed failing with the corresponding line removed from
// src/lib/CMakeLists.txt.

#include <version>  // feature-test macros only; pulls in no library facilities

// The cxx_std_23 usage requirement on venice-cpp::lib. Tested with the library
// feature-test macro rather than __cplusplus, because CMake maps cxx_std_23 to
// -std=c++2b on GCC 13, where __cplusplus is 202100L and not the final value.
//
// std::expected is *the* public API here — every fallible entry point in
// include/venice/client.hpp returns one — so this is the on-point check. An
// undefined macro evaluates to 0 in #if, so "the requirement never arrived at
// all" is caught by the same line.
#if __cpp_lib_expected < 202202L
#error "venice-cpp::lib did not deliver its cxx_std_23 usage requirement (no std::expected)"
#endif

#include <cstdio>

#include <venice/venice.hpp>

// The TLS support carried as an INTERFACE compile definition by httplib's
// imported target, reached transitively through venice-cpp::lib. Drop either
// OpenSSL::SSL or httplib::httplib from src/lib/CMakeLists.txt and this fires.
// The Venice API is TLS-only, so a consumer that builds without it would compile
// here and fail on the first request.
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#error "venice-cpp::lib did not deliver httplib's OpenSSL usage requirement"
#endif

auto main() -> int {
  // Constructed, never used to make a request: this binary runs in CI with no
  // network and no API key. Reaching the constructor at all proves the headers
  // were found through the imported target's include directories.
  const venice::Client client{"not-a-real-key"};

  // verify.sh compares this against the documented default base URL. Printing
  // something the *library* supplies, rather than a literal from this file,
  // is what makes the comparison mean anything.
  std::printf("%s\n", client.base_url().c_str());

  return venice::to_string(venice::ErrorKind::Auth) == "auth" ? 0 : 1;
}
