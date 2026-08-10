# Pure helpers for the README FetchContent release-pin check (VC-12, #17).
#
# This file does not invoke git. check_artifacts.cmake owns repository discovery
# and passes the newest reachable release tag in; readme_tag_selftest.cmake can
# therefore exercise every comparison and malformed-input path without a real
# repository or tags.

include(${CMAKE_CURRENT_LIST_DIR}/version_parse.cmake)

# release_tag_is_newer(<candidate> <current> <out>)
# Both inputs must be exact [rv]?MAJOR.MINOR.PATCH release tags. The empty
# current value is the seed used while finding the maximum tag.
function(release_tag_is_newer CANDIDATE CURRENT OUT)
  parse_git_describe("${CANDIDATE}" _candidate)
  if(NOT _candidate_MATCHED OR NOT "${CANDIDATE}" MATCHES "^[rv]?[0-9]+\\.[0-9]+\\.[0-9]+$")
    set(${OUT} FALSE PARENT_SCOPE)
    return()
  endif()

  if("${CURRENT}" STREQUAL "")
    set(${OUT} TRUE PARENT_SCOPE)
    return()
  endif()

  parse_git_describe("${CURRENT}" _current)
  if(NOT _current_MATCHED OR NOT "${CURRENT}" MATCHES "^[rv]?[0-9]+\\.[0-9]+\\.[0-9]+$")
    set(${OUT} FALSE PARENT_SCOPE)
    return()
  endif()

  set(_newer FALSE)
  if(_candidate_MAJOR GREATER _current_MAJOR)
    set(_newer TRUE)
  elseif(_candidate_MAJOR EQUAL _current_MAJOR)
    if(_candidate_MINOR GREATER _current_MINOR)
      set(_newer TRUE)
    elseif(_candidate_MINOR EQUAL _current_MINOR AND _candidate_PATCH GREATER _current_PATCH)
      set(_newer TRUE)
    endif()
  endif()
  set(${OUT} ${_newer} PARENT_SCOPE)
endfunction()

# check_readme_fetchcontent_tag(<README path> <latest tag> <out-count>
#                               <out-report> <out-tag> <out-comparison-skipped>)
#
# The README pin may equal or be newer than the latest existing tag. The latter
# is the normal release-PR window: documentation names the tag which is created
# only after the PR merges. With no latest tag, syntax/uniqueness is still
# checked and only the age comparison is skipped.
function(check_readme_fetchcontent_tag README_PATH LATEST_TAG OUT_COUNT OUT_REPORT OUT_TAG OUT_SKIPPED)
  set(_violations 0)
  set(_report "")
  set(_readme_tag "")
  set(_skip_compare FALSE)

  if(NOT EXISTS "${README_PATH}")
    set(_violations 1)
    set(_report "\n           README.md is missing")
  else()
    file(READ "${README_PATH}" _content)
    string(REPLACE ";" "\\;" _content "${_content}")
    string(REPLACE "\n" ";" _lines "${_content}")

    set(_in_declaration FALSE)
    set(_declarations 0)
    set(_tag_count 0)
    foreach(_line IN LISTS _lines)
      if(NOT _in_declaration AND
         "${_line}" MATCHES "^[ \t]*FetchContent_Declare[ \t]*\\([ \t]*venice-cpp([ \t]|$)")
        set(_in_declaration TRUE)
        math(EXPR _declarations "${_declarations} + 1")
      endif()

      if(_in_declaration)
        if("${_line}" MATCHES "^[ \t]*GIT_TAG[ \t]+([^ \t\\)]+)")
          math(EXPR _tag_count "${_tag_count} + 1")
          set(_readme_tag "${CMAKE_MATCH_1}")
        endif()
        if("${_line}" MATCHES "\\)")
          set(_in_declaration FALSE)
        endif()
      endif()
    endforeach()

    if(NOT _declarations EQUAL 1)
      math(EXPR _violations "${_violations} + 1")
      string(APPEND _report "\n           README.md has ${_declarations} FetchContent_Declare(venice-cpp ...) blocks; expected exactly one")
    endif()
    if(NOT _tag_count EQUAL 1)
      math(EXPR _violations "${_violations} + 1")
      string(APPEND _report "\n           README.md's venice-cpp FetchContent block has ${_tag_count} GIT_TAG values; expected exactly one")
    endif()

    if(_tag_count EQUAL 1)
      parse_git_describe("${_readme_tag}" _readme)
      if(NOT _readme_MATCHED OR NOT "${_readme_tag}" MATCHES "^[rv]?[0-9]+\\.[0-9]+\\.[0-9]+$")
        math(EXPR _violations "${_violations} + 1")
        string(APPEND _report "\n           README.md pins malformed release tag '${_readme_tag}'; expected [rv]?MAJOR.MINOR.PATCH")
      elseif("${LATEST_TAG}" STREQUAL "")
        set(_skip_compare TRUE)
      else()
        release_tag_is_newer("${LATEST_TAG}" "${_readme_tag}" _latest_is_newer)
        if(_latest_is_newer)
          math(EXPR _violations "${_violations} + 1")
          string(APPEND _report "\n           README.md pins '${_readme_tag}', older than newest reachable release tag '${LATEST_TAG}'")
        endif()
      endif()
    elseif("${LATEST_TAG}" STREQUAL "")
      set(_skip_compare TRUE)
    endif()
  endif()

  set(${OUT_COUNT} "${_violations}" PARENT_SCOPE)
  set(${OUT_REPORT} "${_report}" PARENT_SCOPE)
  set(${OUT_TAG} "${_readme_tag}" PARENT_SCOPE)
  set(${OUT_SKIPPED} "${_skip_compare}" PARENT_SCOPE)
endfunction()
