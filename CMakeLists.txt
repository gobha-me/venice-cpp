cmake_minimum_required(VERSION 3.28)

include(${CMAKE_CURRENT_LIST_DIR}/cmake/version.cmake)

if (NOT CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_LIST_DIR}/cmake/toolchain/default.cmake)
endif()

# https://stackoverflow.com/questions/12344368/automatically-use-the-directory-as-the-project-name-in-cmake
get_filename_component(ProjectId ${CMAKE_CURRENT_LIST_DIR} NAME)
string(REPLACE " " "_" ProjectId ${ProjectId})

project(${ProjectId}
  VERSION ${VERSION}
  LANGUAGES CXX
)

message(STATUS "${PROJECT_NAME}:${CMAKE_PROJECT_VERSION} (tweak=${VERSION_TWEAK} dirty=${VERSION_DIRTY})")

set (VERSION_HEADER_IN "${CMAKE_CURRENT_LIST_DIR}/include/version.hpp.in.cmake")

if (EXISTS ${VERSION_HEADER_IN})
  string (REPLACE ".hpp.in.cmake" ".hpp" VERSION_HEADER ${VERSION_HEADER_IN})
  message(STATUS "${VERSION_HEADER_IN} ${VERSION_HEADER}")
  configure_file (${VERSION_HEADER_IN} ${VERSION_HEADER})
endif ()

find_package(Git REQUIRED) # some deps may require this, depends .. :D
#message(STATUS "git found: ${GIT_EXECUTABLE} in version ${GIT_VERSION_STRING}")

# Dependencies this project uses. Each name maps to cmake/deps/<name>.cmake
# (find_package first, FetchContent fallback). The list is the switch: a recipe
# file that is not named here is never included, and a name here with no recipe
# is a hard configure error.
set(${PROJECT_NAME}_DEPS
  catch2         # Catch2 v3     — test framework              (test/)
  httplib        # cpp-httplib   — HTTP/TLS transport          (src/lib)
  nlohmann_json  # nlohmann/json — JSON (de)serialization      (src/lib)
)

include(${CMAKE_CURRENT_LIST_DIR}/cmake/dependencies.cmake)

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/src/lib)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/src/bin)

option(${PROJECT_NAME}_TESTS "ENABLE TESTS" ON)

if (${PROJECT_NAME}_TESTS)
  include(CTest)
  enable_testing()

  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/test)
endif(${PROJECT_NAME}_TESTS)

# TODO VC-07 (#8): install()/export() + package config, so `find_package(venice-cpp
# CONFIG)` works and consumers aren't forced into add_subdirectory/FetchContent.
# Deferred deliberately: cpp-template CT-04 (gobha-me/cpp-template#5) solves this
# once for the family — crib that pattern here rather than inventing a second one.
