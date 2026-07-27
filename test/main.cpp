#include <catch2/catch_session.hpp>

// Pulled from https://github.com/catchorg/Catch2/blob/devel/docs/own-main.md
// This is common to all test/*/test.cpp that does not specify a CMakeLists.txt
//
// Scope matters here. This main() runs once per test *binary*, so it is the
// place for process-local setup: a logging sink, a temp directory, a Catch2
// event listener. Anything shared across the whole suite — a database, a broker,
// a container, any service the tests talk to — belongs in the ctest fixtures
// (cmake/startup.sh and cmake/shutdown.sh), which run once for the entire run
// and are wired up in test/CMakeLists.txt.

auto main(int argc, char **argv) -> int {
  // Per-binary setup goes here.

  int result = Catch::Session().run(argc, argv);

  // Per-binary cleanup goes here.

  return result;
}
