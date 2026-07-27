# Pure, side-effect-free parsing of `git describe --tags --dirty` output.
#
# This file intentionally does NOT invoke git — it only turns a describe string
# into semantic version components. Keeping it pure is what lets the parser be
# unit-tested in isolation via `cmake -P cmake/version_selftest.cmake` (no real
# repo, no tags, no build tree required). `version.cmake` is the
# thin wrapper that runs git and feeds the result in here.
#
# parse_git_describe(<describe-string> <out-prefix>)
#   Parses inputs of the shape  <tag>[-<N>-g<hash>][-dirty]  where <tag> is
#   [rv]?MAJOR.MINOR.PATCH (git's default `describe` layout). Sets, in the
#   caller's scope, ALL of the following (defaults on every path — never leaks a
#   previous call's value through PARENT_SCOPE):
#     <out-prefix>_MAJOR    integer, leading zeros stripped
#     <out-prefix>_MINOR    integer, leading zeros stripped
#     <out-prefix>_PATCH    integer, leading zeros stripped
#     <out-prefix>_TWEAK    commits since the tag (git's -<N>-), 0 when on the tag
#     <out-prefix>_DIRTY    1 if the working tree was dirty (-dirty suffix), else 0
#     <out-prefix>_MATCHED  1 if the string parsed as MAJOR.MINOR.PATCH, else 0
#
# Deliberately strict: the core must be exactly three numeric components. Two
# components (v1.2), four (v1.2.3.4), pre-release tags (v1.2.3-rc1) and non-tag
# describe output (a bare hash) all yield MATCHED=0 so the caller can fall back
# loudly instead of guessing. The generated header's contract is fixed at
# MAJOR/MINOR/PATCH, so anything else is genuinely ambiguous here.
function(parse_git_describe DESCRIBE OUT)
  # Defaults — set on every code path so a no-match (or a field the input lacks)
  # can never inherit a prior call's PARENT_SCOPE value.
  set(_major 0)
  set(_minor 0)
  set(_patch 0)
  set(_tweak 0)
  set(_dirty 0)
  set(_matched 0)

  set(_core "${DESCRIBE}")

  # Peel the suffixes from the outside in: -dirty is always outermost, then the
  # -<N>-g<hash> "commits since tag" block (git omits it when N == 0).
  if(_core MATCHES "-dirty$")
    set(_dirty 1)
    string(REGEX REPLACE "-dirty$" "" _core "${_core}")
  endif()

  if(_core MATCHES "-([0-9]+)-g[0-9a-fA-F]+$")
    set(_tweak "${CMAKE_MATCH_1}")  # capture before the REPLACE below clobbers CMAKE_MATCH_*
    string(REGEX REPLACE "-([0-9]+)-g[0-9a-fA-F]+$" "" _core "${_core}")
  endif()

  # What remains must be exactly [rv]?MAJOR.MINOR.PATCH. Dots are escaped (\\.)
  # so they match a literal '.' and not any character.
  if(_core MATCHES "^[rv]?([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
    # Copy all three captures out FIRST — the leading-zero strips below are regex
    # ops that overwrite CMAKE_MATCH_* (it is a single global set).
    set(_major "${CMAKE_MATCH_1}")
    set(_minor "${CMAKE_MATCH_2}")
    set(_patch "${CMAKE_MATCH_3}")
    set(_matched 1)

    # Strip leading zeros: '08' would otherwise land in the header as an invalid
    # C++ octal literal. Only the leading run is touched; a lone '0' is preserved.
    string(REGEX REPLACE "^0+([0-9])" "\\1" _major "${_major}")
    string(REGEX REPLACE "^0+([0-9])" "\\1" _minor "${_minor}")
    string(REGEX REPLACE "^0+([0-9])" "\\1" _patch "${_patch}")
  endif()

  set(${OUT}_MAJOR   "${_major}"   PARENT_SCOPE)
  set(${OUT}_MINOR   "${_minor}"   PARENT_SCOPE)
  set(${OUT}_PATCH   "${_patch}"   PARENT_SCOPE)
  set(${OUT}_TWEAK   "${_tweak}"   PARENT_SCOPE)
  set(${OUT}_DIRTY   "${_dirty}"   PARENT_SCOPE)
  set(${OUT}_MATCHED "${_matched}" PARENT_SCOPE)
endfunction()
