# ── Dependency recipe template (canonical example) ──────────────────────────
# Copy this file for a new dependency, then add its stem to ${PROJECT_NAME}_DEPS
# in the root CMakeLists.txt to activate it (presence alone does nothing).
#
# The shape every recipe follows:
#   1. find_package(<Name> [version] QUIET)   — prefer a system/distro copy.
#   2. On miss, FetchContent fallback with overridable, pinned coordinates:
#        <Dep>_URI  — git repository (default set below; override with -D or by
#                     setting it before this file is included).
#        <Dep>_TAG  — git tag/ref to pin (reproducible fallback).
# Bump pins deliberately and say why in the commit (see AGENTS.md).

find_package(Catch2 3 QUIET)

if (Catch2_FOUND)
else ()
    if (NOT Catch2_URI)
        set(Catch2_URI https://github.com/catchorg/Catch2.git) 
    endif()

    if (NOT Catch2_TAG)
        set(Catch2_TAG v3.5.2)
    endif()

    include(FetchContent)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY ${Catch2_URI}
        GIT_TAG ${Catch2_TAG}
    )

    FetchContent_MakeAvailable(Catch2)
endif()
