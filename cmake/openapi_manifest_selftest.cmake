# Dependency-free failure matrix for the checked-in OpenAPI manifest.
# Run directly or through ctest:
#
#   cmake -P cmake/openapi_manifest_selftest.cmake

cmake_minimum_required(VERSION 3.28)

include(${CMAKE_CURRENT_LIST_DIR}/openapi_manifest.cmake)
file(READ "${CMAKE_CURRENT_LIST_DIR}/openapi_manifest.json" _manifest)

set(_fail_count 0)

function(expect_invalid NAME JSON NEEDLE)
  validate_openapi_manifest("${JSON}" _ok _report _total _implemented)
  if(_ok OR NOT _report MATCHES "${NEEDLE}")
    message(WARNING "FAIL : ${NAME} — expected '${NEEDLE}', got: ${_report}")
    math(EXPR _next "${_fail_count} + 1")
    set(_fail_count ${_next} PARENT_SCOPE)
  else()
    message(STATUS "ok   : ${NAME} rejected")
  endif()
endfunction()

# Failure matrix first. A checker never seen rejecting its target is not trusted.
string(JSON _row0 GET "${_manifest}" operations 0)
string(JSON _bad SET "${_manifest}" operations 1 "${_row0}")
expect_invalid("duplicate method/path" "${_bad}" "duplicate operation")

string(JSON _bad REMOVE "${_manifest}" operations 0 family)
expect_invalid("missing required field" "${_bad}" "family is required")

string(JSON _bad SET "${_manifest}" operations 0 state "\"mystery\"")
expect_invalid("unknown state" "${_bad}" "state 'mystery' is unknown")

# Row 40 remains planned (`POST /responses`); row 0 is implemented.
# Pinning each failure to a row in the matching state keeps this matrix honest
# as family work advances the first operations in the manifest.
string(JSON _bad REMOVE "${_manifest}" operations 40 tracking_issue)
expect_invalid("planned operation without issue" "${_bad}" "tracking_issue is required")

string(JSON _bad SET "${_manifest}" operations 40 tracking_issue 26)
expect_invalid("planned operation linked only to a child ticket" "${_bad}" "not a family issue under #36")

string(JSON _bad REMOVE "${_manifest}" operations 0 public_api)
expect_invalid("implemented operation without public API" "${_bad}" "public_api is required")

string(JSON _bad SET "${_manifest}" source sha256 "\"not-a-digest\"")
expect_invalid("invalid source digest" "${_bad}" "64 lowercase hexadecimal")

validate_openapi_coverage_text("OpenAPI coverage: 27/49 operations implemented." "OpenAPI coverage: 47/49 operations implemented." _coverage_ok)
if(_coverage_ok)
  message(WARNING "FAIL : stale documentation coverage was accepted")
  math(EXPR _fail_count "${_fail_count} + 1")
else()
  message(STATUS "ok   : stale documentation coverage rejected")
endif()

# Happy path last: validate the real manifest and the two public status claims.
validate_openapi_manifest("${_manifest}" _ok _report _total _implemented)
if(NOT _ok OR NOT _total EQUAL 49 OR NOT _implemented EQUAL 47)
  message(WARNING "FAIL : checked-in manifest — ${_report}; total=${_total}, implemented=${_implemented}")
  math(EXPR _fail_count "${_fail_count} + 1")
else()
  set(_coverage "OpenAPI coverage: ${_implemented}/${_total} operations implemented.")
  foreach(_doc README.md STATUS.md)
    file(READ "${CMAKE_CURRENT_LIST_DIR}/../${_doc}" _text)
    validate_openapi_coverage_text("${_text}" "${_coverage}" _coverage_ok)
    if(NOT _coverage_ok)
      message(WARNING "FAIL : ${_doc} does not contain checked coverage '${_coverage}'")
      math(EXPR _fail_count "${_fail_count} + 1")
    else()
      message(STATUS "ok   : ${_doc} coverage agrees")
    endif()
  endforeach()
  message(STATUS "ok   : checked-in manifest — ${_report}")
endif()

if(_fail_count GREATER 0)
  message(FATAL_ERROR "OpenAPI manifest self-test: ${_fail_count} case(s) failed")
endif()
message(STATUS "OpenAPI manifest self-test: all cases passed")
