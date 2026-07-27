cmake_minimum_required(VERSION 3.28)

# ── A downstream project, in miniature ──────────────────────────────────────
# This is the acceptance test for VC-07 (#8): it builds against venice-cpp the
# three ways a real project might acquire it, and asserts that each one yields the
# same target and drags nothing else along.
#
# Drive it with example/consumer/verify.sh — that script sets the cache variables
# below and runs all three modes. Read it for the copy-paste each mode reduces to.
#
# Deliberately NOT under test/: test/CMakeLists.txt globs test/* and would
# auto-discover this as a Catch2 test directory.

project(consumer LANGUAGES CXX)

# venice-cpp spells its project name out rather than deriving it from its
# directory, so a consumer can rely on it. Hardcoding it here is the point: if
# that pin is ever removed, the fetchcontent mode below breaks and says so.
set(VENICE_PACKAGE venice-cpp)

set(CONSUMER_MODE "find_package" CACHE STRING
    "how to acquire the library: add_subdirectory | fetchcontent | find_package")

# ── Mode 1: add_subdirectory ────────────────────────────────────────────────
# A vendored or submoduled copy sitting in your tree.
if (CONSUMER_MODE STREQUAL "add_subdirectory")
  if (NOT VENICE_SOURCE_DIR)
    message(FATAL_ERROR "add_subdirectory mode needs -DVENICE_SOURCE_DIR=<path to venice-cpp>")
  endif ()

  # EXCLUDE_FROM_ALL is not required — venice-cpp's own PROJECT_IS_TOP_LEVEL
  # guards already keep its smoke binary and tests out of a consumed build — but
  # it is what you would write anyway, and it means a target that *does* leak
  # still costs nothing to build.
  add_subdirectory(${VENICE_SOURCE_DIR} ${CMAKE_BINARY_DIR}/_venice EXCLUDE_FROM_ALL)

# ── Mode 2: FetchContent ────────────────────────────────────────────────────
elseif (CONSUMER_MODE STREQUAL "fetchcontent")
  if (NOT VENICE_GIT_REPOSITORY OR NOT VENICE_GIT_TAG)
    message(FATAL_ERROR "fetchcontent mode needs -DVENICE_GIT_REPOSITORY= and -DVENICE_GIT_TAG=")
  endif ()

  include(FetchContent)

  # ⚠ SOURCE_DIR is deliberately NOT pinned here, and that is the assertion.
  #
  # FetchContent checks out into <base>/venice-cpp-src. A project that named
  # itself after its directory would come out called "venice-cpp-src", and the
  # target a consumer has to link would be "venice-cpp-src::lib". venice-cpp pins
  # its project name precisely so this works without the consumer knowing to
  # compensate — so this mode leaves the trap armed and checks it does not fire.
  FetchContent_Declare(${VENICE_PACKAGE}
    GIT_REPOSITORY ${VENICE_GIT_REPOSITORY}
    GIT_TAG        ${VENICE_GIT_TAG}
  )

  FetchContent_MakeAvailable(${VENICE_PACKAGE})

# ── Mode 3: installed package ───────────────────────────────────────────────
# `cmake --install`ed somewhere on CMAKE_PREFIX_PATH. CONFIG mode only: there is
# no Findvenice-cpp.cmake module and there should not be one — the package ships
# its own config, generated from cmake/project-config.cmake.in.
elseif (CONSUMER_MODE STREQUAL "find_package")
  find_package(${VENICE_PACKAGE} CONFIG REQUIRED)

else ()
  message(FATAL_ERROR "unknown CONSUMER_MODE '${CONSUMER_MODE}'")
endif ()

# ── What every mode must deliver ────────────────────────────────────────────
# One target, spelled the same way regardless of how it arrived.
if (NOT TARGET ${VENICE_PACKAGE}::lib)
  message(FATAL_ERROR
    "${CONSUMER_MODE}: no target ${VENICE_PACKAGE}::lib. "
    "In find_package mode the export NAMESPACE or EXPORT_NAME in "
    "cmake/install.cmake is wrong; in fetchcontent mode the usual cause is the "
    "project name no longer being pinned in the root CMakeLists.")
endif ()

# ...and nothing else. Each of these leaked before VC-07: the smoke binary, the
# test framework, and the test targets. Naming them individually means a failure
# points at the guard that regressed rather than at "something got bigger".
foreach (_leak IN ITEMS ${VENICE_PACKAGE} Catch2 01client-test 30sanitizer-smoke-test)
  if (TARGET ${_leak})
    message(FATAL_ERROR
      "${CONSUMER_MODE}: consuming the library also created the target '${_leak}'. "
      "Check the PROJECT_IS_TOP_LEVEL defaults on venice-cpp_BUILD_BIN / _TESTS "
      "and the component gating of venice-cpp_DEPS in the root CMakeLists.")
  endif ()
endforeach ()

add_executable(consumer main.cpp)

# The only line a real consumer writes. No include directories, no C++ standard,
# no OpenSSL: all three arrive as usage requirements of the imported target.
target_link_libraries(consumer PRIVATE ${VENICE_PACKAGE}::lib)
