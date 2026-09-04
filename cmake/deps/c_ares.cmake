# 1.34.0 introduced ares_process_fds(), which lets the downloader drive every
# DNS wait from its own deadline/cancellation loop. The fallback stays on the
# current compatible 1.34 patch rather than weakening that bounded API.
find_package(c-ares 1.34.0 CONFIG QUIET)

if (c-ares_FOUND)
else ()
    if (NOT CARES_URI)
        set(CARES_URI https://github.com/c-ares/c-ares.git)
    endif()

    if (NOT CARES_TAG)
        set(CARES_TAG v1.34.8)
    endif()

    include(FetchContent)
    FetchContent_Declare(
        c-ares
        GIT_REPOSITORY ${CARES_URI}
        GIT_TAG ${CARES_TAG}
    )

    # c-ares supports old CMake releases and its option() policy has varied
    # across the compatible range. Set both the directory variable (NEW
    # CMP0077) and cache entry (OLD CMP0077) while configuring the dependency,
    # then put the embedding project's values back exactly. A dependency recipe
    # must not silently rewrite a superproject's cache merely because it needed
    # different settings for this one subdirectory.
    block(SCOPE_FOR VARIABLES)
    foreach(_cares_option IN ITEMS
        CARES_BUILD_TESTS
        CARES_BUILD_CONTAINER_TESTS
        CARES_BUILD_TOOLS
        CARES_INSTALL)
      get_property(_venice_cares_cache_defined
        CACHE ${_cares_option} PROPERTY TYPE SET)
      set(_venice_cares_${_cares_option}_cache_defined
          ${_venice_cares_cache_defined})
      if (_venice_cares_cache_defined)
        get_property(_venice_cares_${_cares_option}_cache_value
          CACHE ${_cares_option} PROPERTY VALUE)
        get_property(_venice_cares_${_cares_option}_cache_type
          CACHE ${_cares_option} PROPERTY TYPE)
        get_property(_venice_cares_${_cares_option}_cache_help
          CACHE ${_cares_option} PROPERTY HELPSTRING)
        get_property(_venice_cares_${_cares_option}_cache_advanced_defined
          CACHE ${_cares_option} PROPERTY ADVANCED SET)
        get_property(_venice_cares_${_cares_option}_cache_advanced
          CACHE ${_cares_option} PROPERTY ADVANCED)
      endif ()

      if (_cares_option STREQUAL "CARES_INSTALL")
        set(_venice_cares_value ${${PROJECT_NAME}_INSTALL})
      else ()
        set(_venice_cares_value OFF)
      endif ()
      set(${_cares_option} ${_venice_cares_value})
      set(${_cares_option} ${_venice_cares_value}
          CACHE BOOL "venice-cpp scoped c-ares option" FORCE)
    endforeach()

    FetchContent_MakeAvailable(c-ares)

    foreach(_cares_option IN ITEMS
        CARES_BUILD_TESTS
        CARES_BUILD_CONTAINER_TESTS
        CARES_BUILD_TOOLS
        CARES_INSTALL)
      if (_venice_cares_${_cares_option}_cache_defined)
        set(${_cares_option}
            "${_venice_cares_${_cares_option}_cache_value}"
            CACHE "${_venice_cares_${_cares_option}_cache_type}"
            "${_venice_cares_${_cares_option}_cache_help}" FORCE)
        if (_venice_cares_${_cares_option}_cache_advanced_defined)
          if (_venice_cares_${_cares_option}_cache_advanced)
            mark_as_advanced(FORCE ${_cares_option})
          else ()
            mark_as_advanced(CLEAR ${_cares_option})
          endif ()
        else ()
          set_property(CACHE ${_cares_option} PROPERTY ADVANCED)
        endif ()
      else ()
        unset(${_cares_option} CACHE)
      endif ()

      unset(_venice_cares_${_cares_option}_cache_defined)
      unset(_venice_cares_${_cares_option}_cache_value)
      unset(_venice_cares_${_cares_option}_cache_type)
      unset(_venice_cares_${_cares_option}_cache_help)
      unset(_venice_cares_${_cares_option}_cache_advanced_defined)
      unset(_venice_cares_${_cares_option}_cache_advanced)
    endforeach()
    unset(_venice_cares_cache_defined)
    unset(_venice_cares_value)
    unset(_cares_option)
    endblock()
endif()
