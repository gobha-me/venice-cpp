find_package(httplib QUIET)

if (httplib_FOUND)
else ()
    if (NOT HTTPLIB_URI)
        set(HTTPLIB_URI https://github.com/yhirose/cpp-httplib.git)
    endif()

    if (NOT HTTPLIB_TAG)
        set(HTTPLIB_TAG v0.18.3)
    endif()

    include(FetchContent)
    FetchContent_Declare(
        httplib
        GIT_REPOSITORY ${HTTPLIB_URI}
        GIT_TAG ${HTTPLIB_TAG}
    )

    # cpp-httplib needs OpenSSL for HTTPS (the Venice API is TLS-only).
    set(HTTPLIB_REQUIRE_OPENSSL ON CACHE BOOL "" FORCE)
    set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(httplib)
endif()
