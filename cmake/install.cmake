# ── Install, export, and package config (VC-07, #8) ─────────────────────────
# This is the answer to the "TODO Install" the root CMakeLists carried since the
# fork, and the reason it matters: a project consuming venice-cpp should be able
# to write
#
#   find_package(venice-cpp CONFIG REQUIRED)
#   target_link_libraries(app PRIVATE venice-cpp::lib)
#
# and have it work against an installed prefix, with the *same* target spelling it
# would use via add_subdirectory(). One spelling, three acquisition modes. See
# example/consumer/, which exercises all three.
#
# Ported from the upstream template's cmake/install.cmake (ticket CT-04) with four
# divergences, each marked ⚠ below. If you re-sync this file from upstream, keep
# them: every one is a property of venice-cpp that upstream does not share.
#
# Included from the root CMakeLists behind ${PROJECT_NAME}_INSTALL, which defaults
# to PROJECT_IS_TOP_LEVEL: an embedded copy of this project must not inject rules
# into its consumer's `cmake --install`.

include(CMakePackageConfigHelpers)

set(_cfg_install_dir ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME})

# ── Which public dependencies are vendored into this build ──────────────────
# venice-cpp_lib links four packages at INTERFACE, i.e. publicly. Two of them come
# from cmake/deps/*.cmake, which prefers find_package and falls back to
# FetchContent — so each is EITHER an IMPORTED target from an installed package OR
# a real target built inside this build. That is exactly the distinction
# install(EXPORT) keys on: an IMPORTED dependency is written through verbatim, a
# non-IMPORTED one must itself be in some export set (which is what the
# HTTPLIB_INSTALL / JSON_Install lines in the recipes arrange).
#
# Detected by target + IMPORTED property rather than by <pkg>_FOUND, because
# _FOUND does not separate the two cases cleanly: on the installed path httplib
# arrives only as httplib::httplib and a bare `httplib` target never exists, while
# nlohmann_json's own config file *does* create a bare IMPORTED `nlohmann_json`
# for pre-3.2 compatibility.
set(_vendored_deps "")
foreach (_dep IN ITEMS httplib nlohmann_json)
  if (TARGET ${_dep})
    get_target_property(_dep_imported ${_dep} IMPORTED)
    if (NOT _dep_imported)
      list(APPEND _vendored_deps ${_dep})
    endif ()
  endif ()
endforeach ()

if (_vendored_deps)
  # STATUS, not WARNING. On any host without a packaged cpp-httplib — which is
  # every Ubuntu, including this project's CI — this is the normal path, and
  # refusing to install here would mean find_package(venice-cpp CONFIG) exists
  # only on hosts that already ship our dependencies. So state the cost rather
  # than pretend there is a choice.
  message(STATUS
    "${PROJECT_NAME}: ${_vendored_deps} were built from source (not found as "
    "installed packages), so `cmake --install` will place them in "
    "CMAKE_INSTALL_PREFIX too — install(EXPORT) cannot export an interface while "
    "omitting an edge of it. Install them as packages and reconfigure for a "
    "prefix containing only ${PROJECT_NAME}.")
endif ()

# ── The library ─────────────────────────────────────────────────────────────
# EXPORT_NAME is what makes the imported target read venice-cpp::lib rather than
# venice-cpp::venice-cpp_lib. Paired with NAMESPACE on the install(EXPORT) below,
# find_package yields a target spelled identically to the in-tree ALIAS in
# src/lib/CMakeLists.txt — so a consumer switches acquisition modes without
# touching a link line.
set_target_properties(${PROJECT_NAME}_lib PROPERTIES EXPORT_NAME lib)

# ⚠ Divergence 1: no ARCHIVE/LIBRARY/RUNTIME destinations. Upstream carries them
# so one call serves both its library variants; venice-cpp is header-only by
# design and not by configuration (AGENTS.md), so they would be permanently dead.
install(TARGETS ${PROJECT_NAME}_lib
  EXPORT ${PROJECT_NAME}Targets
)

install(EXPORT ${PROJECT_NAME}Targets
  FILE      ${PROJECT_NAME}Targets.cmake
  NAMESPACE ${PROJECT_NAME}::
  DESTINATION ${_cfg_install_dir}
)

# ⚠ Divergence 2: no build-tree export(EXPORT ...).
#
# Upstream emits one so a consumer can point CMAKE_PREFIX_PATH at a build
# directory without installing. That cannot work here. The build-tree export
# enforces the same export-set rule as the install one, but against *build*
# export sets — and cpp-httplib registers none: it has install(EXPORT
# httplibTargets) and no export() call anywhere. Observed:
#
#   export called with target "venice-cpp_lib" which requires target "httplib"
#   that is not in any export set.
#
# Second, independent reason: the build-tree copy of venice-cppConfig.cmake would
# carry a PACKAGE_PREFIX_DIR computed for lib/cmake/venice-cpp, i.e. three
# directories above the build dir. Harmless for upstream's config, which only
# includes its Targets file; wrong for ours, which resolves dependencies relative
# to it. Side-by-side development is served by add_subdirectory().

# ── Headers ─────────────────────────────────────────────────────────────────
# ⚠ Divergence 3: include/venice/ only, not all of include/.
#
# Upstream installs *.hpp from include/ and documents the hazard itself: the
# generated version.hpp declares unprefixed globals (PROGRAM_NAME, VERSION_MAJOR,
# ...) and would land at <prefix>/include/version.hpp, on every consumer's include
# path, where it can collide with theirs. Upstream has to ship it because its
# header-only variant reads VERSION_MAJOR from it.
#
# Ours does not: nothing under include/venice/ includes version.hpp — only
# test/01client/test.cpp does, and tests are not installed. So the installed
# surface is exactly the headers under include/venice/, and that collision cannot
# happen here. This is upstream's own advice ("move headers under
# include/<project>/") taken rather than inherited.
install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/venice
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILES_MATCHING PATTERN "*.hpp"
)

# ── The application ─────────────────────────────────────────────────────────
# src/bin is deliberately NOT installed. Upstream installs its demo app because
# demonstrating the build is the point there; ours is a live-API smoke tool that
# needs $VENICE_API_KEY and talks to the network. That is a development aid, not
# a product, and an install prefix is not where it belongs.

# ── Package config ──────────────────────────────────────────────────────────
# venice-cppConfig.cmake is what find_package(venice-cpp CONFIG) loads; it pulls
# in the Targets file and re-finds the public dependencies those targets name.
# See cmake/project-config.cmake.in.
#
# Consumed by that template. Deliberately unhyphenated, so configure_file's @VAR@
# scanner and the hyphen in this project's name never have to meet.
set(CONFIG_VENDORED_DEPS "${_vendored_deps}")

configure_package_config_file(
  ${CMAKE_CURRENT_LIST_DIR}/project-config.cmake.in
  ${PROJECT_BINARY_DIR}/${PROJECT_NAME}Config.cmake
  INSTALL_DESTINATION ${_cfg_install_dir}
)

# ARCH_INDEPENDENT unconditionally: this package ships no compiled artifact, so it
# is usable from a build of any word size, and saying so keeps it from being
# rejected on a 32/64-bit mismatch that cannot apply. Upstream detects the target
# type because its library has two variants; ours has one.
#
# ⚠ Divergence 4: SameMinorVersion, not upstream's SameMajorVersion. At 0.x,
# SameMajorVersion would promise that 0.1.0 satisfies a request for 0.9.0 — false
# for an API still finding its shape. Flip this to SameMajorVersion at 1.0.0,
# when semver's major rule starts carrying that weight.
#
# ⚠ PROJECT_VERSION comes from `git describe` at configure time
# (cmake/version.cmake). A build with no reachable tags reports 0.0.0, and a
# consumer asking for a real version then gets a documented refusal from this file
# rather than a mystery. In CI that is almost always a shallow clone — keep
# fetch-depth: 0.
write_basic_package_version_file(
  ${PROJECT_BINARY_DIR}/${PROJECT_NAME}ConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMinorVersion
  ARCH_INDEPENDENT
)

install(FILES
    ${PROJECT_BINARY_DIR}/${PROJECT_NAME}Config.cmake
    ${PROJECT_BINARY_DIR}/${PROJECT_NAME}ConfigVersion.cmake
  DESTINATION ${_cfg_install_dir}
)
