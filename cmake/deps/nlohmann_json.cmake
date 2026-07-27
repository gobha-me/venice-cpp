find_package(nlohmann_json QUIET)

if (nlohmann_json_FOUND)
else ()
    if (NOT JSON_URI)
        set(JSON_URI https://github.com/nlohmann/json.git)
    endif()

    if (NOT JSON_TAG)
        set(JSON_TAG v3.11.3)
    endif()

    include(FetchContent)
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY ${JSON_URI}
        GIT_TAG ${JSON_TAG}
    )

    # nlohmann/json's JSON_Install defaults to MAIN_PROJECT — i.e. OFF whenever it
    # is a subproject — which leaves the `nlohmann_json` target in no export set at
    # all. Our library links it publicly, so install(EXPORT) then fails at generate
    # time. Observed, before this line existed:
    #
    #   install(EXPORT "venice-cppTargets" ...) includes target "venice-cpp_lib"
    #   which requires target "nlohmann_json" that is not in any export set.
    #
    # There is no way to export an interface while omitting an edge of it, so on
    # the fetched path "we install" and "our public dependencies install" are the
    # same statement. Tied to our option rather than forced ON: when venice-cpp is
    # consumed, _INSTALL is OFF, we emit no install(EXPORT), and nlohmann's rules
    # must not land in someone else's prefix.
    #
    # Normal variable, not CACHE FORCE — nlohmann/json sets CMP0077 NEW itself.
    set(JSON_Install ${${PROJECT_NAME}_INSTALL})

    set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(nlohmann_json)
endif()
