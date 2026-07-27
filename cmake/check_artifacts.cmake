# ── Template-artifact check ─────────────────────────────────────────────────
# Reports leftover template artifacts and internal inconsistencies. It is
# read-only: it never edits, moves or deletes anything. Two modes:
#
#     cmake -P cmake/check_artifacts.cmake                    # enforce  (a fork)
#     cmake -DMODE=selftest -P cmake/check_artifacts.cmake    # selftest (this repo)
#
# (CMake requires -D *before* -P.)
#
# venice-cpp is a fork, never the template, so it always runs ENFORCE: every rule
# must report zero hits. The SELFTEST mode is kept only so this file stays a
# small diff against cpp-template — it inverts Class A there, where each of those
# rules must match at LEAST ONE thing because the template legitimately contains
# every artifact they look for, which is how a rule is stopped from rotting into
# one that matches nothing and waves every fork through. Nothing here runs it.
#
# test/CMakeLists.txt registers enforce unconditionally rather than keying the
# mode on NEW_PROJECT.md the way upstream does. In a fork that conditional is a
# footgun: anyone adding a file by that plausible name would silently invert all
# eleven Class-A rules and turn the suite red for a reason unrelated to the code.
#
# Class A rules are about template leftovers, and this repo is already clean of
# them — they are cheap insurance against a bad re-sync from upstream. Class B is
# the part that earns its keep long-term: B2 is what this repo failed (VC-01, #2),
# fetching fmt and argparse that nothing linked.
#
# Exit status is the contract, same as cmake/version_selftest.cmake: any failed
# rule -> message(FATAL_ERROR) -> non-zero exit -> ctest fails.

cmake_minimum_required(VERSION 3.28)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

if(NOT DEFINED MODE)
  set(MODE "enforce")
endif()
if(NOT MODE STREQUAL "enforce" AND NOT MODE STREQUAL "selftest")
  message(FATAL_ERROR "check_artifacts: MODE must be 'enforce' or 'selftest', got '${MODE}'")
endif()

set(_fail_count 0)
set(_rule_count 0)

# ── The scanned file set ────────────────────────────────────────────────────
# Prefer `git ls-files`: the tracked set excludes build*/, FetchContent's
# _deps/, the generated-and-gitignored include/version.hpp and any stray
# worktrees for free. All four are false-positive factories — a build tree
# really does contain the project's own symbols.
find_package(Git QUIET)

set(REPO_FILES "")
if(GIT_FOUND)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} ls-files
    WORKING_DIRECTORY ${REPO_ROOT}
    OUTPUT_VARIABLE _ls
    RESULT_VARIABLE _ls_rc
    ERROR_QUIET
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  if(_ls_rc EQUAL 0 AND NOT _ls STREQUAL "")
    string(REPLACE "\n" ";" REPO_FILES "${_ls}")
  endif()
endif()

if(REPO_FILES STREQUAL "")
  # Fallback for a tree that isn't a git repo yet (e.g. mid-bootstrap, between
  # `rm -rf .git` and `git init`). Same exclusions, done by hand.
  message(STATUS "note : no git file list; falling back to a filesystem walk")
  file(GLOB_RECURSE _walk RELATIVE ${REPO_ROOT} ${REPO_ROOT}/*)
  foreach(_f IN LISTS _walk)
    if(_f MATCHES "^(build|Testing|\\.git|\\.claude|\\.cache)" OR _f MATCHES "_deps/" OR _f STREQUAL "include/version.hpp")
      continue()
    endif()
    list(APPEND REPO_FILES "${_f}")
  endforeach()
endif()

# Two files talk *about* the artifacts rather than containing them, and both
# must stay out of the scan:
#
#   * this script necessarily contains every pattern it searches for. Leave it
#     in and the checker is permanently dirty in every fork, with a failure that
#     reads like a real finding.
#   * NEW_PROJECT.md quotes artifacts in order to tell you to remove them
#     ("rename `namespace template_lib`"). Leave it in and it becomes a phantom
#     hit source that keeps Class-A rules green in selftest mode no matter what
#     happened to the real artifact — which defeats the entire point of the
#     inversion. This was caught by deliberately renaming template_lib and
#     watching rule A2 pass anyway.
#
# Their *existence* is still checked (rule A11); only their contents are ignored.
list(REMOVE_ITEM REPO_FILES "cmake/check_artifacts.cmake" "NEW_PROJECT.md")

# Binary files would be read as garbage; nothing worth grepping lives in them.
set(_binary_re "\\.(png|jpe?g|gif|ico|pdf|zip|gz|tar|xz|so|a|o|bin|exe|woff2?|ttf|otf)$")

# ── Helpers ─────────────────────────────────────────────────────────────────

# Read a file into a list of lines. Semicolons are escaped first, or CMake
# would silently split file content on them and wreck the line numbering.
function(_read_lines PATH OUT)
  file(READ "${PATH}" _content)
  string(REPLACE ";" "\\;" _content "${_content}")
  string(REPLACE "\n" ";" _content "${_content}")
  set(${OUT} "${_content}" PARENT_SCOPE)
endfunction()

# _scan(<regex> <path-regex> <out-hits> <out-report>)
# Counts matching lines across the scanned set, reporting each as file:line.
function(_scan REGEX PATH_REGEX OUT_HITS OUT_REPORT)
  set(_hits 0)
  set(_report "")
  foreach(_rel IN LISTS REPO_FILES)
    if(NOT "${_rel}" MATCHES "${PATH_REGEX}")
      continue()
    endif()
    if("${_rel}" MATCHES "${_binary_re}")
      continue()
    endif()
    if(NOT EXISTS "${REPO_ROOT}/${_rel}")
      continue()   # tracked but deleted in the working tree
    endif()
    _read_lines("${REPO_ROOT}/${_rel}" _lines)
    set(_n 0)
    foreach(_line IN LISTS _lines)
      math(EXPR _n "${_n} + 1")
      if("${_line}" MATCHES "${REGEX}")
        math(EXPR _hits "${_hits} + 1")
        string(APPEND _report "\n           ${_rel}:${_n}")
      endif()
    endforeach()
  endforeach()
  set(${OUT_HITS} ${_hits} PARENT_SCOPE)
  set(${OUT_REPORT} "${_report}" PARENT_SCOPE)
endfunction()

# report_a(<id> <what> <hits-var> <report-var>)
# Class A — a template artifact. Must be absent in a fork, present in the
# template. Macros (not functions) so the counters mutate the caller's scope.
macro(report_a ID WHAT HITS_VAR REPORT_VAR)
  math(EXPR _rule_count "${_rule_count} + 1")
  if(MODE STREQUAL "selftest")
    if(${${HITS_VAR}} GREATER 0)
      if(NOT QUIET)
        message(STATUS "ok   : ${ID} ${WHAT} — ${${HITS_VAR}} hit(s), as expected in the template")
      endif()
    else()
      message(WARNING "FAIL : ${ID} ${WHAT} — matches nothing. The rule is stale: "
                      "the artifact was renamed or removed upstream, so this check "
                      "would pass every fork without looking at anything.")
      math(EXPR _fail_count "${_fail_count} + 1")
    endif()
  else()
    if(${${HITS_VAR}} EQUAL 0)
      if(NOT QUIET)
        message(STATUS "ok   : ${ID} ${WHAT}")
      endif()
    else()
      message(WARNING "FAIL : ${ID} ${WHAT} — still present:${${REPORT_VAR}}")
      math(EXPR _fail_count "${_fail_count} + 1")
    endif()
  endif()
endmacro()

# report_b(<id> <what> <violations-var> <report-var>)
# Class B — an internal inconsistency. Must be zero in the template AND in a
# fork, so it is never inverted.
macro(report_b ID WHAT COUNT_VAR REPORT_VAR)
  math(EXPR _rule_count "${_rule_count} + 1")
  if(${${COUNT_VAR}} EQUAL 0)
    if(NOT QUIET)
      message(STATUS "ok   : ${ID} ${WHAT}")
    endif()
  else()
    message(WARNING "FAIL : ${ID} ${WHAT}:${${REPORT_VAR}}")
    math(EXPR _fail_count "${_fail_count} + 1")
  endif()
endmacro()

message(STATUS "check_artifacts: mode=${MODE}, ${REPO_ROOT}")

# ── Class A: template artifacts ─────────────────────────────────────────────
# Each of these is something the template ships and a finished project must not.

_scan("gobha-me/cpp-template" ".*" _h _r)
report_a("A1" "upstream repo slug (README CI badge)" _h _r)

_scan("template_lib" ".*" _h _r)
report_a("A2" "demo library namespace \"template_lib\"" _h _r)

_scan("Placeholder translation unit" ".*" _h _r)
report_a("A3" "placeholder library source (src/lib/lib.cpp)" _h _r)

# Targets the demo main's signature line, not argparse itself — a fork that
# legitimately uses argparse must not be flagged for doing so.
_scan("ArgumentParser program\\(PROGRAM_NAME\\.data\\(\\), fmt::format\\(" "^src/" _h _r)
report_a("A4" "demo CLI in src/bin/main.cpp" _h _r)

set(_h 0)
set(_r "")
foreach(_demo 01example 02example 10example)
  if(EXISTS "${REPO_ROOT}/test/${_demo}")
    math(EXPR _h "${_h} + 1")
    string(APPEND _r "\n           test/${_demo}/")
  endif()
endforeach()
report_a("A5" "demo test dirs" _h _r)

set(_h 0)
set(_r "")
foreach(_rel IN LISTS REPO_FILES)
  if("${_rel}" MATCHES "\\.cpp$" AND EXISTS "${REPO_ROOT}/${_rel}")
    file(SIZE "${REPO_ROOT}/${_rel}" _sz)
    if(_sz EQUAL 0)
      math(EXPR _h "${_h} + 1")
      string(APPEND _r "\n           ${_rel}")
    endif()
  endif()
endforeach()
report_a("A6" "zero-byte .cpp stubs (glob demonstrators)" _h _r)

_scan("tie this template" ".*" _h _r)
report_a("A7" "LICENSE.md is the template's non-license" _h _r)

_scan("easy-button" ".*" _h _r)
report_a("A8" "template pitch prose in the docs" _h _r)

_scan("left as an exercise" ".*" _h _r)
report_a("A9" "template hand-wave prose in the docs" _h _r)

_scan("C\\+\\+ project template" ".*" _h _r)
report_a("A10" "AGENTS.md still self-identifies as the template" _h _r)

set(_h 0)
set(_r "")
if(EXISTS "${REPO_ROOT}/NEW_PROJECT.md")
  set(_h 1)
  set(_r "\n           NEW_PROJECT.md")
endif()
report_a("A11" "NEW_PROJECT.md (the bootstrap checklist / mode marker)" _h _r)

# ── Class B: internal consistency ───────────────────────────────────────────
# Green in the template and in a fork. These stay useful for the life of the
# project — they are not bootstrap leftovers, they are wiring that can drift.

# Parse the declarative deps list out of the root CMakeLists without
# configuring: find the `set(<something>_DEPS` line, then take names until the
# closing paren. Comments and blank lines are dropped.
set(_deps "")
set(_deps_lines "")
if(EXISTS "${REPO_ROOT}/CMakeLists.txt")
  _read_lines("${REPO_ROOT}/CMakeLists.txt" _root_lines)
  set(_n 0)
  set(_in_list FALSE)
  foreach(_line IN LISTS _root_lines)
    math(EXPR _n "${_n} + 1")
    set(_payload "")
    if(NOT _in_list AND "${_line}" MATCHES "^[ \t]*set[ \t]*\\([ \t]*[^ \t)]*_DEPS(.*)$")
      set(_in_list TRUE)
      set(_payload "${CMAKE_MATCH_1}")
    elseif(_in_list)
      set(_payload "${_line}")
    endif()
    if(_in_list)
      list(APPEND _deps_lines ${_n})
      if("${_payload}" MATCHES "^([^)]*)\\)")
        set(_payload "${CMAKE_MATCH_1}")
        set(_in_list FALSE)
      endif()
      string(REGEX REPLACE "#.*$" "" _payload "${_payload}")
      string(REGEX REPLACE "[ \t]+" ";" _payload "${_payload}")
      foreach(_tok IN LISTS _payload)
        if(NOT "${_tok}" STREQUAL "")
          list(APPEND _deps "${_tok}")
        endif()
      endforeach()
    endif()
  endforeach()
endif()

set(_b1 0)
set(_r "")
foreach(_dep IN LISTS _deps)
  if(NOT EXISTS "${REPO_ROOT}/cmake/deps/${_dep}.cmake")
    math(EXPR _b1 "${_b1} + 1")
    string(APPEND _r "\n           '${_dep}' is listed but cmake/deps/${_dep}.cmake is missing")
  endif()
endforeach()
report_b("B1" "every listed dependency has a recipe" _b1 _r)

# Listed-but-unused: this repo's own past failure (VC-01, #2) — recipes that
# FetchContent-pull a
# library nothing links. The usage token is derived from the recipe's own
# find_package() call, so this works for deps the template never heard of.
set(_b2 0)
set(_r "")
foreach(_dep IN LISTS _deps)
  set(_recipe "${REPO_ROOT}/cmake/deps/${_dep}.cmake")
  if(NOT EXISTS "${_recipe}")
    continue()   # already reported by B1
  endif()
  set(_pkg "")
  _read_lines("${_recipe}" _recipe_lines)
  foreach(_line IN LISTS _recipe_lines)
    if("${_line}" MATCHES "^[ \t]*#")
      continue()   # the recipe template's `find_package(<Name> ...)` example
    endif()
    if("${_line}" MATCHES "find_package[ \t]*\\([ \t]*([A-Za-z0-9_.+-]+)")
      set(_pkg "${CMAKE_MATCH_1}")
      break()
    endif()
  endforeach()
  if(_pkg STREQUAL "")
    continue()   # recipe doesn't use find_package; nothing to correlate
  endif()
  set(_used FALSE)
  foreach(_rel IN LISTS REPO_FILES)
    if(NOT "${_rel}" MATCHES "(CMakeLists\\.txt|\\.cmake)$")
      continue()
    endif()
    if("${_rel}" STREQUAL "cmake/deps/${_dep}.cmake" OR NOT EXISTS "${REPO_ROOT}/${_rel}")
      continue()
    endif()
    _read_lines("${REPO_ROOT}/${_rel}" _lines)
    set(_n 0)
    foreach(_line IN LISTS _lines)
      math(EXPR _n "${_n} + 1")
      # Skip the deps list itself: naming a dep there is the declaration, not a use.
      if("${_rel}" STREQUAL "CMakeLists.txt")
        list(FIND _deps_lines ${_n} _idx)
        if(NOT _idx EQUAL -1)
          continue()
        endif()
      endif()
      # Skip comments: naming a package in prose is not linking it. Without this,
      # a comment explaining *why* a dependency was dropped ("we no longer pull
      # fmt") reads as evidence that it is still used, and the rule goes blind to
      # exactly the dependency it was written to catch. Found the hard way — the
      # comment in cmake/dependencies.cmake did this.
      if("${_line}" MATCHES "^[ \t]*#")
        continue()
      endif()
      if("${_line}" MATCHES "${_pkg}")
        set(_used TRUE)
        break()
      endif()
    endforeach()
    if(_used)
      break()
    endif()
  endforeach()
  if(NOT _used)
    math(EXPR _b2 "${_b2} + 1")
    string(APPEND _r "\n           '${_dep}' is fetched but '${_pkg}' is linked by nothing — drop it from the deps list")
  endif()
endforeach()
report_b("B2" "no dependency is fetched but unused" _b2 _r)

# The UBSan define is a coupled pair across two files. Rename it on one side
# only and the UBSan smoke case compiles out on GCC <= 13 while the binary
# still exits 0 — sanitizer verification lost, CI green.
set(_b3 0)
set(_r "")
set(_ubsan_toolchain "")
if(EXISTS "${REPO_ROOT}/cmake/toolchain/undefined.cmake")
  _read_lines("${REPO_ROOT}/cmake/toolchain/undefined.cmake" _u_lines)
  foreach(_line IN LISTS _u_lines)
    if("${_line}" MATCHES "-D([A-Za-z_][A-Za-z0-9_]*UBSAN)([^A-Za-z0-9_]|$)")
      list(APPEND _ubsan_toolchain "${CMAKE_MATCH_1}")
    endif()
  endforeach()
endif()
list(REMOVE_DUPLICATES _ubsan_toolchain)

# Tokens the test sources test for but do not define themselves.
set(_ubsan_tests "")
set(_ubsan_selfdef "")
foreach(_rel IN LISTS REPO_FILES)
  if(NOT "${_rel}" MATCHES "^test/.*\\.(cpp|hpp|h)$" OR NOT EXISTS "${REPO_ROOT}/${_rel}")
    continue()
  endif()
  _read_lines("${REPO_ROOT}/${_rel}" _lines)
  foreach(_line IN LISTS _lines)
    if("${_line}" MATCHES "defined[ \t]*\\([ \t]*([A-Za-z_][A-Za-z0-9_]*UBSAN)([^A-Za-z0-9_]|$)")
      list(APPEND _ubsan_tests "${CMAKE_MATCH_1}")
    endif()
    if("${_line}" MATCHES "^[ \t]*#[ \t]*define[ \t]+([A-Za-z_][A-Za-z0-9_]*UBSAN)([^A-Za-z0-9_]|$)")
      list(APPEND _ubsan_selfdef "${CMAKE_MATCH_1}")
    endif()
  endforeach()
endforeach()
if(_ubsan_tests)
  list(REMOVE_DUPLICATES _ubsan_tests)
  if(_ubsan_selfdef)
    list(REMOVE_ITEM _ubsan_tests ${_ubsan_selfdef})
  endif()
endif()

foreach(_tok IN LISTS _ubsan_toolchain)
  if(NOT "${_tok}" IN_LIST _ubsan_tests)
    math(EXPR _b3 "${_b3} + 1")
    string(APPEND _r "\n           undefined.cmake defines -D${_tok} but no test checks defined(${_tok})")
  endif()
endforeach()
foreach(_tok IN LISTS _ubsan_tests)
  if(NOT "${_tok}" IN_LIST _ubsan_toolchain)
    math(EXPR _b3 "${_b3} + 1")
    string(APPEND _r "\n           tests check defined(${_tok}) but undefined.cmake never defines it")
  endif()
endforeach()
report_b("B3" "the UBSan define matches on both sides" _b3 _r)

# test/CMakeLists.txt names the sanitizer smoke dir inside `if (TARGET ...)`.
# That guard is silent when the dir is renamed or deleted: green CI, no proof
# the sanitizer is engaged.
set(_b4 0)
set(_r "")
if(EXISTS "${REPO_ROOT}/test/CMakeLists.txt")
  _read_lines("${REPO_ROOT}/test/CMakeLists.txt" _t_lines)
  set(_n 0)
  foreach(_line IN LISTS _t_lines)
    math(EXPR _n "${_n} + 1")
    if("${_line}" MATCHES "if[ \t]*\\([ \t]*TARGET[ \t]+([A-Za-z0-9_.-]+)-test[ \t]*\\)")
      set(_dir "${CMAKE_MATCH_1}")
      if(NOT EXISTS "${REPO_ROOT}/test/${_dir}")
        math(EXPR _b4 "${_b4} + 1")
        string(APPEND _r "\n           test/CMakeLists.txt:${_n} guards on ${_dir}-test but test/${_dir}/ does not exist")
      endif()
    endif()
  endforeach()
endif()
report_b("B4" "target-guarded test dirs still exist" _b4 _r)

# ctest invokes fixture scripts by path, with no interpreter. A lost exec bit
# fails every test run with a confusing permission error, and `cp` without -p
# (or a zip download) is enough to lose it.
set(_b5 0)
set(_r "")
if(GIT_FOUND)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} ls-files -s
    WORKING_DIRECTORY ${REPO_ROOT}
    OUTPUT_VARIABLE _ls_s
    RESULT_VARIABLE _ls_s_rc
    ERROR_QUIET
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  if(_ls_s_rc EQUAL 0)
    string(REPLACE "\n" ";" _ls_s "${_ls_s}")
    foreach(_entry IN LISTS _ls_s)
      if("${_entry}" MATCHES "^([0-7]+)[ \t]+[0-9a-f]+[ \t]+[0-9]+[ \t]+(.*\\.sh)$")
        if(NOT CMAKE_MATCH_1 STREQUAL "100755")
          math(EXPR _b5 "${_b5} + 1")
          string(APPEND _r "\n           ${CMAKE_MATCH_2} is mode ${CMAKE_MATCH_1}, not 100755 — ctest runs it directly")
        endif()
      endif()
    endforeach()
  endif()
endif()
report_b("B5" "shell scripts keep their exec bit" _b5 _r)

# ── Verdict ─────────────────────────────────────────────────────────────────
if(_fail_count GREATER 0)
  message(FATAL_ERROR
    "check_artifacts (${MODE}): ${_fail_count} of ${_rule_count} rule(s) failed. "
    "See the FAIL lines above.")
endif()
message(STATUS "CLEAN — check_artifacts (${MODE}): all ${_rule_count} rules passed")
