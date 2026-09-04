#!/usr/bin/env bash
# ── Consumer acceptance check (VC-07 / #8) ──────────────────────────────────
# Builds example/consumer/ against this project three ways — add_subdirectory,
# FetchContent, and an installed find_package — and runs the result each time.
#
#   example/consumer/verify.sh                 # $CXX or the platform default
#   CXX=clang++ example/consumer/verify.sh     # the other compiler
#
# Every mode must produce a binary that prints the library's default base URL.
# Printing a value the *library* supplies, rather than a literal from the
# consumer's own source, is what makes the comparison mean anything.
#
# Everything is built under mktemp -d, so this leaves no build dirs behind and
# needs no .gitignore entry.
set -euo pipefail

BUILD_PARALLEL=(--parallel)
if [ -n "${VENICE_BUILD_JOBS:-}" ]; then
  if [[ ! "${VENICE_BUILD_JOBS}" =~ ^[1-9][0-9]*$ ]]; then
    echo "VENICE_BUILD_JOBS must be a positive integer" >&2
    exit 2
  fi
  BUILD_PARALLEL+=("${VENICE_BUILD_JOBS}")
fi

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CONSUMER_DIR="${REPO_ROOT}/example/consumer"
NAME="venice-cpp"                      # pinned in the root CMakeLists; see D2 there
EXPECTED="https://api.venice.ai/api/v1"

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

pass=0
fail=0

cares_cache_is() {
  local build="$1"
  local name="$2"
  local expected="$3"
  grep -Fqx "${name}:BOOL=${expected}" "${build}/CMakeCache.txt"
}

cares_source_sentinels_survived() {
  local build="$1"
  cares_cache_is "${build}" CARES_BUILD_TESTS ON \
    && cares_cache_is "${build}" CARES_BUILD_CONTAINER_TESTS ON \
    && cares_cache_is "${build}" CARES_BUILD_TOOLS ON \
    && cares_cache_is "${build}" CARES_INSTALL ON
}

# Optional local speed-up: point at an existing _deps directory to reuse already
# downloaded dependency sources. Deliberately unset in CI, where fetching for
# real is part of what is being tested.
#
#   VENICE_DEPS_CACHE=/path/to/build/_deps example/consumer/verify.sh
DEP_ARGS=()
if [ -n "${VENICE_DEPS_CACHE:-}" ]; then
  DEP_ARGS+=("-DFETCHCONTENT_SOURCE_DIR_C-ARES=${VENICE_DEPS_CACHE}/c-ares-src")
  DEP_ARGS+=("-DFETCHCONTENT_SOURCE_DIR_HTTPLIB=${VENICE_DEPS_CACHE}/httplib-src")
  DEP_ARGS+=("-DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=${VENICE_DEPS_CACHE}/nlohmann_json-src")
fi

# Run one mode end to end: configure, build, execute, compare stdout.
# Extra -D arguments for the mode are passed after the mode name.
run_mode() {
  local mode="$1"; shift
  local build="${WORK}/build-${mode}"

  echo "── ${mode} ───────────────────────────────────────────────"

  if cmake -S "${CONSUMER_DIR}" -B "${build}" \
       -DCONSUMER_MODE="${mode}" \
       "${DEP_ARGS[@]}" \
       "$@" > "${build}.log" 2>&1 \
     && { [ "${mode}" = find_package ] \
          || cares_source_sentinels_survived "${build}"; } \
     && cmake --build "${build}" "${BUILD_PARALLEL[@]}" >> "${build}.log" 2>&1
  then
    local got
    got="$("${build}/consumer")"
    if [ "${got}" = "${EXPECTED}" ]; then
      echo "PASS ${mode}: consumer printed '${got}'"
      pass=$((pass + 1))
      return 0
    fi
    echo "FAIL ${mode}: expected '${EXPECTED}', got '${got}'"
  else
    echo "FAIL ${mode}: configure or build failed"
    tail -n 30 "${build}.log" | sed 's/^/     | /'
  fi

  fail=$((fail + 1))
  return 0
}

# ── Mode 1: add_subdirectory ────────────────────────────────────────────────
run_mode add_subdirectory \
  -DVENICE_SOURCE_DIR="${REPO_ROOT}" \
  -DCARES_BUILD_TESTS:BOOL=ON \
  -DCARES_BUILD_CONTAINER_TESTS:BOOL=ON \
  -DCARES_BUILD_TOOLS:BOOL=ON \
  -DCARES_INSTALL:BOOL=ON

# ── Mode 2: FetchContent ────────────────────────────────────────────────────
# A real consumer writes a public URL and a tag. We point at a throwaway repo
# built from the *working tree* instead, which keeps this offline and — the part
# that matters — tests the code you are about to commit.
#
# Fetching from ${REPO_ROOT} directly would clone HEAD, so on a dirty tree this
# mode would quietly build different sources than the other two and report a
# failure that has nothing to do with your changes.
#
# The snapshot has no tags, so version.cmake falls back to 0.0.0 with a reason —
# which incidentally exercises the tagless path for free.
SNAPSHOT="${WORK}/snapshot"
mkdir -p "${SNAPSHOT}"
(
  cd "${REPO_ROOT}"
  # Tracked files plus new, non-ignored ones: what a commit right now would hold.
  git ls-files --cached --others --exclude-standard -z | xargs -0 cp -p --parents -t "${SNAPSHOT}"
  git -C "${SNAPSHOT}" init -q -b main
  git -C "${SNAPSHOT}" add -A
  # -c user.*: CI runners have no git identity configured, and commit would fail.
  git -C "${SNAPSHOT}" -c user.email=verify@example.invalid -c user.name=verify \
      commit -qm "working tree snapshot"
) > "${WORK}/snapshot.log" 2>&1

run_mode fetchcontent \
  -DVENICE_GIT_REPOSITORY="file://${SNAPSHOT}" \
  -DVENICE_GIT_TAG="$(git -C "${SNAPSHOT}" rev-parse HEAD)" \
  -DCARES_BUILD_TESTS:BOOL=ON \
  -DCARES_BUILD_CONTAINER_TESTS:BOOL=ON \
  -DCARES_BUILD_TOOLS:BOOL=ON \
  -DCARES_INSTALL:BOOL=ON

# ── Mode 3: installed find_package ──────────────────────────────────────────
# Install the library only. The smoke binary and tests are off by their own
# options here, which doubles as proof that a library-only install is supported.
PREFIX="${WORK}/prefix"
if {
  cmake -S "${REPO_ROOT}" -B "${WORK}/build-install" \
    -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
    "${DEP_ARGS[@]}" \
    "-D${NAME}_BUILD_BIN=OFF" "-D${NAME}_TESTS=OFF" \
    -DCARES_BUILD_TESTS:BOOL=ON \
    -DCARES_BUILD_CONTAINER_TESTS:BOOL=ON \
    -DCARES_BUILD_TOOLS:BOOL=ON \
    -DCARES_INSTALL:BOOL=OFF
  cares_cache_is "${WORK}/build-install" CARES_BUILD_TESTS ON
  cares_cache_is "${WORK}/build-install" CARES_BUILD_CONTAINER_TESTS ON
  cares_cache_is "${WORK}/build-install" CARES_BUILD_TOOLS ON
  cares_cache_is "${WORK}/build-install" CARES_INSTALL OFF
  cmake --build "${WORK}/build-install" "${BUILD_PARALLEL[@]}"
  cmake --install "${WORK}/build-install"
} > "${WORK}/install.log" 2>&1
then
  # The install *layout* is part of the contract, not just the fact that a
  # consumer can build. These two paths are what find_package(venice-cpp CONFIG)
  # and #include <venice/venice.hpp> resolve through.
  for want in "lib/cmake/${NAME}/${NAME}Config.cmake" \
              "lib/cmake/${NAME}/${NAME}Targets.cmake" \
              "include/venice/venice.hpp"; do
    if [ ! -f "${PREFIX}/${want}" ]; then
      echo "FAIL install layout: ${want} missing from the prefix"
      fail=$((fail + 1))
    fi
  done

  # The generated version header is deliberately not installed: it declares
  # unprefixed globals (PROGRAM_NAME, VERSION_MAJOR) that would land on every
  # consumer's include path. See divergence 3 in cmake/install.cmake.
  if [ -f "${PREFIX}/include/version.hpp" ]; then
    echo "FAIL install layout: include/version.hpp leaked into the prefix"
    fail=$((fail + 1))
  fi

  run_mode find_package -DCMAKE_PREFIX_PATH="${PREFIX}"
else
  echo "FAIL find_package: could not build and install the project"
  tail -n 30 "${WORK}/install.log" | sed 's/^/     | /'
  fail=$((fail + 1))
fi

echo "──────────────────────────────────────────────────────────"
echo "consumer verify: ${pass} passed, ${fail} failed  (CXX=${CXX:-default})"
[ "${fail}" -eq 0 ]
