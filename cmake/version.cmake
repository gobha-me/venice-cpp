# Derive the project version from git tags — no commits needed just to roll a
# version. This runs BEFORE project() in the root CMakeLists, so the VERSION_*
# vars it sets are in scope for both project(VERSION ...) and the configure_file
# that generates include/version.hpp.
#
# The actual string parsing lives in version_parse.cmake (pure, no git) so it can
# be unit-tested via `cmake -P cmake/version_selftest.cmake`.
# Ported from the cpp-template upstream ticket CT-05.

include(${CMAKE_CURRENT_LIST_DIR}/version_parse.cmake)  # absolute: no CMAKE_MODULE_PATH here

find_package(Git)

set(GIT_TAG_STR "")
set(GIT_RESULT 0)

if(GIT_FOUND)
  # Nearest tag, plus git's -<N>-g<hash> (commits since tag) and -dirty markers.
  execute_process(
    COMMAND ${GIT_EXECUTABLE} describe --tags --dirty
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    RESULT_VARIABLE GIT_RESULT
    OUTPUT_VARIABLE GIT_TAG_STR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
endif()

parse_git_describe("${GIT_TAG_STR}" GITVER)

if(GITVER_MATCHED)
  set(VERSION "${GITVER_MAJOR}.${GITVER_MINOR}.${GITVER_PATCH}")
  set(VERSION_MAJOR ${GITVER_MAJOR})
  set(VERSION_MINOR ${GITVER_MINOR})
  set(VERSION_PATCH ${GITVER_PATCH})
  set(VERSION_TWEAK ${GITVER_TWEAK})
  set(VERSION_DIRTY ${GITVER_DIRTY})
else()
  # Fallback: three clean components (not the old 0.0.0.1 sentinel), with every
  # var set concretely so the generated header never gets an empty substitution.
  # 0.0.0 is also a legal tag, so the reason lives in the STATUS message below.
  set(VERSION 0.0.0)
  set(VERSION_MAJOR 0)
  set(VERSION_MINOR 0)
  set(VERSION_PATCH 0)
  set(VERSION_TWEAK 0)
  set(VERSION_DIRTY 0)

  if(NOT GIT_FOUND)
    message(STATUS "version: git not found; using ${VERSION}")
  elseif(NOT GIT_RESULT EQUAL 0)
    message(STATUS "version: no git tags reachable; using ${VERSION}")
  else()
    message(STATUS "version: tag '${GIT_TAG_STR}' is not MAJOR.MINOR.PATCH; using ${VERSION}")
  endif()
endif()
