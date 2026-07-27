#include <catch2/catch_test_macros.hpp>   // included BEFORE the __has_feature shim

#include <cstddef>
#include <limits>
#include <thread>

// Portable sanitizer detection.
//   * Clang and GCC 14+ expose __has_feature.
//   * GCC (13+) additionally predefines __SANITIZE_ADDRESS__ / __SANITIZE_THREAD__.
//   * GCC has NO standard UBSan predefine before __has_feature (GCC 14+), so on
//     GCC <= 13 the UBSan branch relies on -DVENICE_UBSAN from undefined.cmake.
// The buggy branch is compiled ONLY under the matching sanitizer, so a default
// build runs a trivially-passing test and never executes UB.
#if !defined(__has_feature)
#  define __has_feature(x) 0
#endif

#if defined(__SANITIZE_ADDRESS__) || __has_feature(address_sanitizer)
#  define SMOKE_ASAN 1
#endif
#if defined(__SANITIZE_THREAD__) || __has_feature(thread_sanitizer)
#  define SMOKE_TSAN 1
#endif
#if __has_feature(undefined_behavior_sanitizer) || defined(VENICE_UBSAN)
#  define SMOKE_UBSAN 1
#endif

TEST_CASE("sanitizer smoke: harness runs", "[smoke]") {
  REQUIRE(true);   // always compiled: a binary with 0 TEST_CASEs exits nonzero
}

#ifdef SMOKE_ASAN
TEST_CASE("ASan detects heap-buffer-overflow", "[smoke][asan]") {
  int* p = new int[1];
  volatile std::size_t i = 3;   // volatile defeats compile-time OOB elision (survives -O3)
  p[i] = 42;                    // heap-buffer-overflow -> ASan aborts here
  const int v = p[i];
  delete[] p;
  REQUIRE(v == 42);             // unreachable once ASan is engaged
}
#endif

#ifdef SMOKE_TSAN
TEST_CASE("TSan detects data race", "[smoke][tsan]") {
  int shared = 0;
  auto bump = [&shared] { for (int n = 0; n < 100000; ++n) ++shared; };
  std::thread a(bump);
  std::thread b(bump);          // unsynchronized writes -> data race
  a.join();
  b.join();
  REQUIRE(shared > 0);
}
#endif

#ifdef SMOKE_UBSAN
TEST_CASE("UBSan detects signed-integer overflow", "[smoke][ubsan]") {
  volatile int x = std::numeric_limits<int>::max();
  const int y = x + 1;          // signed overflow (UB) -> UBSan reports
  REQUIRE(y != 0);
}
#endif
