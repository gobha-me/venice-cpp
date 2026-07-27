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

    # cpp-httplib's HTTPLIB_INSTALL defaults ON even as a subproject, and both
    # states of that are wrong for us — so tie it to our own option instead.
    #
    #   _INSTALL=ON  (top level): keeps the `httplib` target in an export set, so
    #     install(EXPORT venice-cppTargets) can resolve httplib::httplib. Turning
    #     it OFF here would fail the generate step with
    #       install(EXPORT ...) includes target "venice-cpp_lib" which requires
    #       target "httplib" that is not in any export set.
    #
    #   _INSTALL=OFF (consumed): stops cpp-httplib's headers, cmake package,
    #     README, LICENSE and its include(CPack) from landing in a downstream
    #     project's install prefix. That leak is live today.
    #
    # Normal variable, not CACHE FORCE: cpp-httplib requires CMake >= 3.14, so
    # CMP0077 is NEW there and a parent-scope variable wins over its option().
    set(HTTPLIB_INSTALL ${${PROJECT_NAME}_INSTALL})

    # cpp-httplib installs its own README to ${CMAKE_INSTALL_DOCDIR}. GNUInstallDirs
    # computed that from the *top-level* project name, so it would land at
    # share/doc/venice-cpp/README.md — someone else's readme filed as our
    # documentation. Point it at httplib's own docdir for the duration of the
    # subdirectory, matching the way its LICENSE is already namespaced under
    # share/licenses/httplib. install() records the value in effect when the
    # subdirectory is processed, so save/restore around MakeAvailable is enough.
    set(_venice_saved_docdir "${CMAKE_INSTALL_DOCDIR}")
    set(CMAKE_INSTALL_DOCDIR "${CMAKE_INSTALL_DATAROOTDIR}/doc/httplib")

    FetchContent_MakeAvailable(httplib)

    set(CMAKE_INSTALL_DOCDIR "${_venice_saved_docdir}")
    unset(_venice_saved_docdir)
endif()
