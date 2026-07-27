cmake_minimum_required(VERSION 3.28)

# Toolchain defaulting, top-level only. A toolchain file has to be set before the
# first project() call in the whole build, so this cannot use PROJECT_IS_TOP_LEVEL
# — that variable does not exist until project() runs, a dozen lines down. This is
# the same test, spelled the only way that works this early.
#
# When venice-cpp is consumed (add_subdirectory / FetchContent) the consumer owns
# the toolchain, and setting one here would override theirs.
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR AND NOT CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_LIST_DIR}/cmake/toolchain/default.cmake)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/cmake/version.cmake)

# The project name is spelled out, not derived from the directory name.
#
# Upstream cpp-template derives it (get_filename_component on CMAKE_CURRENT_LIST_DIR)
# so that a fork can copy its CMake files verbatim. That trade stops paying here:
# since VC-07 the name is public API — it is the spelling in
# find_package(venice-cpp CONFIG), in the venice-cpp::lib target, in
# venice-cppConfig.cmake and in lib/cmake/venice-cpp. Derived, it would follow the
# checkout directory, so FetchContent's default <base>/venice-cpp-src would rename
# the package to "venice-cpp-src" and the target to "venice-cpp-src::lib" — a
# broken published contract rather than merely an odd target name.
#
# example/consumer/ asserts this: its fetchcontent mode deliberately does not
# pin SOURCE_DIR, and still gets venice-cpp::lib.
project(venice-cpp
  VERSION ${VERSION}
  LANGUAGES CXX
)

# CMAKE_INSTALL_{INCLUDE,LIB,BIN}DIR, used by src/lib and cmake/install.cmake.
# Included explicitly rather than inherited: httplib and nlohmann_json each
# include this module themselves, so those variables happen to exist today as a
# side effect of the FetchContent path — and would not on the find_package path.
include(GNUInstallDirs)

# PROJECT_VERSION, not CMAKE_PROJECT_VERSION: the CMAKE_-prefixed variable holds
# the *top-level* project's version, which is someone else's when we are consumed.
message(STATUS "${PROJECT_NAME}:${PROJECT_VERSION} (tweak=${VERSION_TWEAK} dirty=${VERSION_DIRTY} top_level=${PROJECT_IS_TOP_LEVEL})")

set (VERSION_HEADER_IN "${CMAKE_CURRENT_LIST_DIR}/include/version.hpp.in.cmake")

if (EXISTS ${VERSION_HEADER_IN})
  string (REPLACE ".hpp.in.cmake" ".hpp" VERSION_HEADER ${VERSION_HEADER_IN})
  message(STATUS "${VERSION_HEADER_IN} ${VERSION_HEADER}")
  configure_file (${VERSION_HEADER_IN} ${VERSION_HEADER})
endif ()

# ── What gets built ─────────────────────────────────────────────────────────
# Everything except the library defaults to PROJECT_IS_TOP_LEVEL, so a project
# consuming venice-cpp gets the library and nothing else: no smoke binary, no
# test suite, and no install rules landing in someone else's `cmake --install`.
# Building this repo directly is unchanged — all three are ON.
#
# The library itself is unconditional: it is the thing worth consuming.
#
# Declared above the dependency list because that list is gated on them, and
# because cmake/deps/httplib.cmake and cmake/deps/nlohmann_json.cmake read
# _INSTALL to decide whether to enable each dependency's own install rules.
# See cmake/install.cmake for why those must agree with ours.
option(${PROJECT_NAME}_BUILD_BIN "Build the live-API smoke binary in src/bin"    ${PROJECT_IS_TOP_LEVEL})
option(${PROJECT_NAME}_TESTS     "Build and register the test suite"             ${PROJECT_IS_TOP_LEVEL})
option(${PROJECT_NAME}_INSTALL   "Generate install / export / package-config rules" ${PROJECT_IS_TOP_LEVEL})

# Dependencies this project uses. Each name maps to cmake/deps/<name>.cmake
# (find_package first, FetchContent fallback). The list is the switch: a recipe
# file that is not named here is never included, and a name here with no recipe
# is a hard configure error.
set(${PROJECT_NAME}_DEPS
  catch2         # Catch2 v3     — test framework              (test/)
  httplib        # cpp-httplib   — HTTP/TLS transport          (src/lib, public)
  nlohmann_json  # nlohmann/json — JSON (de)serialization      (src/lib, public)
)

# Component gating: a dependency that exists only to serve a target we are not
# building does not get fetched. Without this, consuming the library alone still
# downloads a test framework.
#
# The list above stays the single prune point — this only ever REMOVEs a name
# from it, never adds one.
#
# httplib and nlohmann_json are never gated: they are *public* usage requirements
# of the library itself, so every consumer needs them however it is built.
if (NOT ${PROJECT_NAME}_TESTS)
  list(REMOVE_ITEM ${PROJECT_NAME}_DEPS catch2)
endif ()

include(${CMAKE_CURRENT_LIST_DIR}/cmake/dependencies.cmake)

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/src/lib)

if (${PROJECT_NAME}_BUILD_BIN)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/src/bin)
endif ()

if (${PROJECT_NAME}_TESTS)
  include(CTest)
  enable_testing()

  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/test)
endif (${PROJECT_NAME}_TESTS)

# Install rules, the exported target set and the package config (VC-07, #8).
# find_package(venice-cpp CONFIG) is real now; example/consumer/verify.sh is the
# acceptance check.
if (${PROJECT_NAME}_INSTALL)
  include(${CMAKE_CURRENT_LIST_DIR}/cmake/install.cmake)
endif ()
