#!/usr/bin/env bash
# #120: actual source/install consumers plus rejected dependency profiles.
set -euo pipefail
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PROBE="${REPO_ROOT}/example/httplib-contract"
WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT
SOURCE_ARGS=()
if [ -n "${VENICE_HTTPLIB_SOURCE:-}" ]; then
  SOURCE_ARGS+=("-DFETCHCONTENT_SOURCE_DIR_HTTPLIB=${VENICE_HTTPLIB_SOURCE}")
fi
configure() {
  local name="$1"; shift
  cmake -S "${PROBE}" -B "${WORK}/${name}" \
    -DVENICE_SOURCE_DIR="${REPO_ROOT}" "${SOURCE_ARGS[@]}" "$@" \
    > "${WORK}/${name}.log" 2>&1
}
accept() {
  local name="$1"; shift
  if ! configure "${name}" "$@"; then
    cat "${WORK}/${name}.log"
    echo "FAIL ${name}: compatible contract rejected" >&2
    exit 1
  fi
  cmake --build "${WORK}/${name}" --parallel "${VENICE_BUILD_JOBS:-2}" >> "${WORK}/${name}.log" 2>&1
  local output
  output="$("${WORK}/${name}/httplib_contract")"
  if [[ ! "${output}" =~ ^0\.51\.[0-9]+$ ]]; then
    echo "FAIL ${name}: consumer did not report the admitted version" >&2
    exit 1
  fi
  echo "PASS ${name}"
}
reject() {
  local name="$1"; shift
  if configure "${name}" "$@"; then
    echo "FAIL ${name}: incompatible contract accepted" >&2
    exit 1
  fi
  if ! grep -Eq 'cpp-httplib|expected installed/imported httplib' "${WORK}/${name}.log"; then
    cat "${WORK}/${name}.log"
    echo "FAIL ${name}: unrelated configure failure" >&2
    exit 1
  fi
  echo "PASS ${name} refused"
}

accept fetched -DCMAKE_DISABLE_FIND_PACKAGE_httplib=ON \
  -DPROBE_SOURCE_SENTINELS=ON -DHTTPLIB_TEST:BOOL=ON \
  -DHTTPLIB_INSTALL:BOOL=ON -DHTTPLIB_COMPILE:BOOL=ON \
  -DHTTPLIB_BUILD_MODULES:BOOL=ON -DHTTPLIB_NO_EXCEPTIONS:BOOL=ON \
  -DHTTPLIB_USE_NON_BLOCKING_GETADDRINFO:BOOL=ON
SOURCE="${VENICE_HTTPLIB_SOURCE:-${WORK}/fetched/_deps/httplib-src}"
# Reconfiguration must not turn preserved parent cache options into new target
# settings. The same cache sentinels and compiled capability proof run again.
accept fetched -DCMAKE_DISABLE_FIND_PACKAGE_httplib=ON \
  -DPROBE_SOURCE_SENTINELS=ON
SOURCE_ARGS=("-DFETCHCONTENT_SOURCE_DIR_HTTPLIB=${SOURCE}")

# Use upstream's real install/export, not a fabricated successful package.
cmake -S "${SOURCE}" -B "${WORK}/upstream" \
  -DCMAKE_INSTALL_PREFIX="${WORK}/installed" \
  -DHTTPLIB_REQUIRE_OPENSSL=ON -DHTTPLIB_USE_OPENSSL_IF_AVAILABLE=ON \
  -DHTTPLIB_REQUIRE_WOLFSSL=OFF -DHTTPLIB_USE_WOLFSSL_IF_AVAILABLE=OFF \
  -DHTTPLIB_REQUIRE_MBEDTLS=OFF -DHTTPLIB_USE_MBEDTLS_IF_AVAILABLE=OFF \
  -DHTTPLIB_NO_EXCEPTIONS=OFF -DHTTPLIB_USE_NON_BLOCKING_GETADDRINFO=OFF \
  -DHTTPLIB_COMPILE=OFF -DHTTPLIB_BUILD_MODULES=OFF -DHTTPLIB_TEST=OFF \
  -DHTTPLIB_REQUIRE_ZSTD=OFF -DHTTPLIB_USE_ZSTD_IF_AVAILABLE=OFF \
  -DHTTPLIB_INSTALL=ON > "${WORK}/upstream.log" 2>&1
cmake --install "${WORK}/upstream" >> "${WORK}/upstream.log" 2>&1
accept installed -DCMAKE_PREFIX_PATH="${WORK}/installed" -DPROBE_EXPECT_IMPORTED=ON

PROFILE=("-DPROBE_HEADER_DIR=${SOURCE}" -DPROBE_PREEXISTING=interface \
  -DPROBE_DEFINITIONS=CPPHTTPLIB_OPENSSL_SUPPORT)
accept preexisting "${PROFILE[@]}"
reject compiled "${PROFILE[@]}" -DPROBE_PREEXISTING=compiled
reject missing-tls "${PROFILE[@]}" -DPROBE_DEFINITIONS=
reject missing-tls-link "${PROFILE[@]}" -DPROBE_PREEXISTING=missing-tls-link
reject resolver "${PROFILE[@]}" \
  '-DPROBE_DEFINITIONS=CPPHTTPLIB_OPENSSL_SUPPORT;CPPHTTPLIB_USE_NON_BLOCKING_GETADDRINFO'
reject header-count "${PROFILE[@]}" \
  '-DPROBE_DEFINITIONS=CPPHTTPLIB_OPENSSL_SUPPORT;CPPHTTPLIB_HEADER_MAX_COUNT=200'
reject header-line "${PROFILE[@]}" \
  '-DPROBE_DEFINITIONS=CPPHTTPLIB_OPENSSL_SUPPORT;CPPHTTPLIB_HEADER_MAX_LENGTH=16384'
reject line-growth "${PROFILE[@]}" \
  '-DPROBE_DEFINITIONS=CPPHTTPLIB_OPENSSL_SUPPORT;CPPHTTPLIB_MAX_LINE_LENGTH=65536'
reject no-exceptions "${PROFILE[@]}" \
  '-DPROBE_DEFINITIONS=CPPHTTPLIB_OPENSSL_SUPPORT;CPPHTTPLIB_NO_EXCEPTIONS'
reject alternate-tls "${PROFILE[@]}" \
  '-DPROBE_DEFINITIONS=CPPHTTPLIB_OPENSSL_SUPPORT;CPPHTTPLIB_WOLFSSL_SUPPORT'

mkdir -p "${WORK}/wrong-header"
sed 's/CPPHTTPLIB_VERSION "0\.51\.[0-9]*"/CPPHTTPLIB_VERSION "0.50.0"/' \
  "${SOURCE}/httplib.h" > "${WORK}/wrong-header/httplib.h"
reject wrong-header "${PROFILE[@]}" -DPROBE_HEADER_DIR="${WORK}/wrong-header"
mkdir -p "${WORK}/malformed-version" "${WORK}/old-openssl"
sed 's/CPPHTTPLIB_VERSION "0\.51\.[0-9]*"/CPPHTTPLIB_VERSION "0.51.future"/' \
  "${SOURCE}/httplib.h" > "${WORK}/malformed-version/httplib.h"
reject malformed-version "${PROFILE[@]}" -DPROBE_HEADER_DIR="${WORK}/malformed-version"
# This is an actual-header guard regression, not a claimed OpenSSL2 runtime run.
cat "${SOURCE}/httplib.h" > "${WORK}/old-openssl/httplib.h"
cat >> "${WORK}/old-openssl/httplib.h" <<'HEADER'
#undef OPENSSL_VERSION_MAJOR
#define OPENSSL_VERSION_MAJOR 2
HEADER
reject old-openssl-header "${PROFILE[@]}" -DPROBE_HEADER_DIR="${WORK}/old-openssl"
python3 "${SOURCE}/split.py" -o "${WORK}/split" > "${WORK}/split.log"
reject split-header-wrapper "${PROFILE[@]}" -DPROBE_HEADER_DIR="${WORK}/split"

# A claimed compatible config without its canonical target must not fall back.
mkdir -p "${WORK}/missing" "${WORK}/obsolete"
cat > "${WORK}/missing/httplibConfig.cmake" <<'CONFIG'
set(httplib_FOUND TRUE)
set(httplib_OpenSSL_FOUND TRUE)
CONFIG
cat > "${WORK}/obsolete/httplibConfig.cmake" <<'CONFIG'
set(httplib_FOUND TRUE)
CONFIG
cat > "${WORK}/versions.cmake" <<VERSIONS
include(CMakePackageConfigHelpers)
write_basic_package_version_file("${WORK}/missing/httplibConfigVersion.cmake"
  VERSION 0.51.0 COMPATIBILITY SameMinorVersion ARCH_INDEPENDENT)
write_basic_package_version_file("${WORK}/obsolete/httplibConfigVersion.cmake"
  VERSION 0.18.3 COMPATIBILITY SameMinorVersion ARCH_INDEPENDENT)
VERSIONS
cmake -P "${WORK}/versions.cmake"
reject missing-target -Dhttplib_DIR="${WORK}/missing"
# A rejected hint may legitimately select a compatible installed alternative.
accept obsolete-hint-alternative -Dhttplib_DIR="${WORK}/obsolete" \
  -DCMAKE_PREFIX_PATH="${WORK}/installed" -DPROBE_EXPECT_IMPORTED=ON
# Isolate the rejection despite a competing installed prefix AND usable fallback
# source; imported proof makes an accidental embedded fallback insufficient.
reject obsolete-installed -Dhttplib_DIR="${WORK}/obsolete" \
  -DCMAKE_PREFIX_PATH="${WORK}/installed" -DPROBE_EXPECT_IMPORTED=ON \
  -DCMAKE_FIND_ROOT_PATH="${WORK}/obsolete" \
  -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY \
  -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=NEVER \
  -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=NEVER \
  -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER
