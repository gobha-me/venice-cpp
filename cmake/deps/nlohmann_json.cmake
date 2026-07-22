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

    set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(nlohmann_json)
endif()
