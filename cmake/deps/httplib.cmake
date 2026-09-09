# #120: one shared 0.51 header-only OpenSSL3 target. An installed package wins,
# but its version string alone cannot prove the selected header/API contract.
if (NOT TARGET httplib::httplib)
  find_package(httplib 0.51...<0.52 CONFIG QUIET COMPONENTS OpenSSL)
endif ()

if (NOT TARGET httplib::httplib AND NOT httplib_FOUND)
  include(FetchContent)
  if (HTTPLIB_URI OR HTTPLIB_TAG)
    if (NOT HTTPLIB_URI)
      set(HTTPLIB_URI https://github.com/yhirose/cpp-httplib.git)
    endif ()
    if (NOT HTTPLIB_TAG)
      set(HTTPLIB_TAG d66d9a95997d51a8ba9822a611d1267757741535)
    endif ()
    FetchContent_Declare(httplib GIT_REPOSITORY "${HTTPLIB_URI}" GIT_TAG "${HTTPLIB_TAG}")
  else ()
    FetchContent_Declare(httplib
      URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.51.0.tar.gz
      URL_HASH SHA256=d740ced75352f44e9d66d08806dc231b5621bb3592b5a5d5b2bd890a9a9d86cc)
  endif ()

  # Normal variables under upstream's NEW CMP0077 policy keep caller cache
  # values intact. The selected target records these options at construction.
  block(SCOPE_FOR VARIABLES)
    set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL TRUE)
    set(HTTPLIB_REQUIRE_OPENSSL ON)
    set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE ON)
    set(HTTPLIB_REQUIRE_WOLFSSL OFF)
    set(HTTPLIB_USE_WOLFSSL_IF_AVAILABLE OFF)
    set(HTTPLIB_REQUIRE_MBEDTLS OFF)
    set(HTTPLIB_USE_MBEDTLS_IF_AVAILABLE OFF)
    set(HTTPLIB_NO_EXCEPTIONS OFF)
    set(HTTPLIB_USE_NON_BLOCKING_GETADDRINFO OFF)
    set(HTTPLIB_COMPILE OFF)
    set(HTTPLIB_BUILD_MODULES OFF)
    set(HTTPLIB_TEST OFF)
    # 0.18 supported zlib/Brotli; preserve those shared optional features. Zstd
    # is newly available in 0.51 and is not part of this compatibility update.
    set(HTTPLIB_REQUIRE_ZSTD OFF)
    set(HTTPLIB_USE_ZSTD_IF_AVAILABLE OFF)
    set(HTTPLIB_INSTALL ${${PROJECT_NAME}_INSTALL})
    set(CMAKE_INSTALL_DOCDIR "${CMAKE_INSTALL_DATAROOTDIR}/doc/httplib")
    FetchContent_MakeAvailable(httplib)
  endblock()
endif ()

include(${CMAKE_CURRENT_LIST_DIR}/../httplib-contract.cmake)
venice_cpp_check_httplib(_venice_httplib_valid _venice_httplib_reason
  "${CMAKE_CURRENT_LIST_DIR}/../../include")
if (NOT _venice_httplib_valid)
  message(FATAL_ERROR "venice-cpp: ${_venice_httplib_reason}")
endif ()
unset(_venice_httplib_valid)
unset(_venice_httplib_reason)
