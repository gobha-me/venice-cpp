# ── Dependency activation (declarative opt-in) ──────────────────────────────
# Includes exactly the recipes named in ${PROJECT_NAME}_DEPS (root CMakeLists),
# each mapping to cmake/deps/<name>.cmake. Nothing is fetched unless it's on the
# list, so what gets pulled is a deliberate declaration rather than whatever
# happens to sit in cmake/deps/ — which is how this repo ended up FetchContent-
# pulling two libraries that nothing linked (VC-01, #2).
#   Add a dep:    drop cmake/deps/<name>.cmake AND add <name> to the list.
#   Remove a dep: delete <name> from the list, and delete the recipe too — this
#                 is a library, not a template, so an unreferenced recipe is dead
#                 weight that no check_artifacts rule watches.

foreach(DEP IN LISTS ${PROJECT_NAME}_DEPS)
  set(_dep_recipe ${CMAKE_CURRENT_LIST_DIR}/deps/${DEP}.cmake)
  if (NOT EXISTS ${_dep_recipe})
    message(FATAL_ERROR
      "Dependency '${DEP}' is in ${PROJECT_NAME}_DEPS but recipe ${_dep_recipe} "
      "is missing. Add cmake/deps/${DEP}.cmake or drop '${DEP}' from the list.")
  endif()
  message(STATUS "Including dependency ${DEP} (${_dep_recipe})")
  include(${_dep_recipe})
endforeach()
