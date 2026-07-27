# Self-test for parse_git_describe() — pure string logic, so it needs no repo,
# no build tree and no tags. Run it directly or via ctest:
#
#     cmake -P cmake/version_selftest.cmake
#
# Exit status is the contract: any failed row -> message(FATAL_ERROR) -> non-zero
# exit -> ctest fails (target `version-parse-selftest`). This is what makes the
# version parsing enforceable in CI.
#
# Following this repo's testing philosophy (AGENTS.md): the failure matrix comes
# first — malformed / non-tag / boundary inputs — and the plain happy-path tag is
# the last, least-interesting row.

cmake_minimum_required(VERSION 3.28)

include(${CMAKE_CURRENT_LIST_DIR}/version_parse.cmake)

set(_fail_count 0)

# expect(<describe> <major> <minor> <patch> <tweak> <dirty> <matched>)
function(expect DESCRIBE E_MAJOR E_MINOR E_PATCH E_TWEAK E_DIRTY E_MATCHED)
  parse_git_describe("${DESCRIBE}" T)

  set(_problems "")
  if(NOT T_MATCHED STREQUAL "${E_MATCHED}")
    string(APPEND _problems " matched=${T_MATCHED}(want ${E_MATCHED})")
  endif()
  if(NOT T_MAJOR STREQUAL "${E_MAJOR}")
    string(APPEND _problems " major=${T_MAJOR}(want ${E_MAJOR})")
  endif()
  if(NOT T_MINOR STREQUAL "${E_MINOR}")
    string(APPEND _problems " minor=${T_MINOR}(want ${E_MINOR})")
  endif()
  if(NOT T_PATCH STREQUAL "${E_PATCH}")
    string(APPEND _problems " patch=${T_PATCH}(want ${E_PATCH})")
  endif()
  if(NOT T_TWEAK STREQUAL "${E_TWEAK}")
    string(APPEND _problems " tweak=${T_TWEAK}(want ${E_TWEAK})")
  endif()
  if(NOT T_DIRTY STREQUAL "${E_DIRTY}")
    string(APPEND _problems " dirty=${T_DIRTY}(want ${E_DIRTY})")
  endif()

  if(_problems STREQUAL "")
    message(STATUS "ok   : '${DESCRIBE}' -> ${T_MAJOR}.${T_MINOR}.${T_PATCH} tweak=${T_TWEAK} dirty=${T_DIRTY} matched=${T_MATCHED}")
  else()
    message(WARNING "FAIL : '${DESCRIBE}' ->${_problems}")
    math(EXPR _fail_count "${_fail_count} + 1")
    set(_fail_count "${_fail_count}" PARENT_SCOPE)
  endif()
endfunction()

#         describe input               maj min pat twk dty mch
# --- failure matrix: nothing that isn't a clean MAJOR.MINOR.PATCH may parse ---
expect(""                              0   0   0   0   0   0)   # untagged clone (empty describe)
expect("abc1234"                       0   0   0   0   0   0)   # bare hash, no tag
expect("v1.2"                          0   0   0   0   0   0)   # too few components
expect("v1.2.3.4"                      0   0   0   0   0   0)   # too many components
expect("not-a-version"                 0   0   0   0   0   0)   # non-numeric junk
expect("v1.2.3-rc1"                    0   0   0   0   0   0)   # pre-release suffix (unsupported by design)
expect("v1x2y3"                        0   0   0   0   0   0)   # non-dot separators (old regex accepted these)

# --- semantics: tweak (commits since tag) and dirty are extracted, not dropped ---
expect("v1.2.3-5-gabc1234-dirty"       1   2   3   5   1   1)   # ahead AND dirty
expect("v1.2.3-5-gabc1234"             1   2   3   5   0   1)   # 5 commits past the tag
expect("v1.2.3-dirty"                  1   2   3   0   1   1)   # on the tag, dirty tree
expect("v01.02.03"                     1   2   3   0   0   1)   # leading zeros normalized (no C++ octal)
expect("r2.4.6"                        2   4   6   0   0   1)   # 'r' prefix variant
expect("2.4.6"                         2   4   6   0   0   1)   # no prefix

# --- happy path, last: a clean tag is tweak=0 dirty=0 (guards PARENT_SCOPE leaks) ---
expect("v1.2.3"                        1   2   3   0   0   1)

if(_fail_count GREATER 0)
  message(FATAL_ERROR "version_parse self-test: ${_fail_count} case(s) failed")
endif()
message(STATUS "version_parse self-test: all cases passed")
