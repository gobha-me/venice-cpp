# Failure-matrix-first self-test for the README FetchContent release pin
# (VC-12, #17). Pure CMake, no repository or tags required.

cmake_minimum_required(VERSION 3.28)

include(${CMAKE_CURRENT_LIST_DIR}/readme_tag_check.cmake)

set(_fixture "${CMAKE_CURRENT_BINARY_DIR}/readme_tag_selftest_fixture.md")
set(_fail_count 0)

function(expect NAME BODY LATEST_TAG EXPECTED_VIOLATIONS EXPECTED_TAG EXPECTED_SKIPPED)
  file(WRITE "${_fixture}" "${BODY}")
  check_readme_fetchcontent_tag(
    "${_fixture}" "${LATEST_TAG}" _violations _report _tag _skipped)

  set(_problems "")
  if(NOT _violations EQUAL EXPECTED_VIOLATIONS)
    string(APPEND _problems " violations=${_violations}(want ${EXPECTED_VIOLATIONS})")
  endif()
  if(NOT _tag STREQUAL "${EXPECTED_TAG}")
    string(APPEND _problems " tag='${_tag}'(want '${EXPECTED_TAG}')")
  endif()
  if(NOT _skipped STREQUAL "${EXPECTED_SKIPPED}")
    string(APPEND _problems " skipped=${_skipped}(want ${EXPECTED_SKIPPED})")
  endif()

  if(_problems STREQUAL "")
    message(STATUS "ok   : ${NAME}")
  else()
    message(WARNING "FAIL : ${NAME}:${_problems}${_report}")
    math(EXPR _fail_count "${_fail_count} + 1")
    set(_fail_count "${_fail_count}" PARENT_SCOPE)
  endif()
endfunction()

set(_prefix "include(FetchContent)\nFetchContent_Declare(venice-cpp\n  GIT_REPOSITORY https://github.com/gobha-me/venice-cpp.git\n")

# Failure matrix first: age, syntax, missing/ambiguous structure.
expect("stale patch pin" "${_prefix}  GIT_TAG v0.13.9)\n" "v0.14.0" 1 "v0.13.9" FALSE)
expect("stale minor pin" "${_prefix}  GIT_TAG v0.9.9)\n" "v0.14.0" 1 "v0.9.9" FALSE)
expect("malformed two-component pin" "${_prefix}  GIT_TAG v0.14)\n" "v0.14.0" 1 "v0.14" FALSE)
expect("describe suffix is not a release tag" "${_prefix}  GIT_TAG v0.14.0-1-gabc)\n" "v0.14.0" 1 "v0.14.0-1-gabc" FALSE)
expect("missing GIT_TAG" "FetchContent_Declare(venice-cpp\n  GIT_REPOSITORY example.invalid/repo.git)\n" "v0.14.0" 1 "" FALSE)
expect("missing declaration" "# no FetchContent declaration\n" "v0.14.0" 2 "" FALSE)
expect("duplicate declaration" "${_prefix}  GIT_TAG v0.14.0)\n${_prefix}  GIT_TAG v0.14.0)\n" "v0.14.0" 2 "v0.14.0" FALSE)

# A shallow/tarball checkout still enforces the local README shape.
expect("no tags skips age only" "${_prefix}  GIT_TAG v0.14.0)\n" "" 0 "v0.14.0" TRUE)
expect("no tags does not excuse malformed pin" "${_prefix}  GIT_TAG latest)\n" "" 1 "latest" FALSE)

# Release-PR and settled-release paths.
expect("future patch is allowed before tagging" "${_prefix}  GIT_TAG v0.14.1)\n" "v0.14.0" 0 "v0.14.1" FALSE)
expect("equal release is current" "${_prefix}  GIT_TAG r2.4.6)\n" "v2.4.6" 0 "r2.4.6" FALSE)

file(REMOVE "${_fixture}")

if(_fail_count GREATER 0)
  message(FATAL_ERROR "readme_tag self-test: ${_fail_count} case(s) failed")
endif()
message(STATUS "readme_tag self-test: all cases passed")
