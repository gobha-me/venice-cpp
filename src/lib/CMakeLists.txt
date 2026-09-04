# ── venice-cpp: header-only (INTERFACE) library ────────────────────────────
add_library(${PROJECT_NAME}_lib INTERFACE)
add_library(${PROJECT_NAME}::lib ALIAS ${PROJECT_NAME}_lib)

# The two generator expressions are not decoration. Three independent reasons,
# any one of which is sufficient:
#   * CMAKE_SOURCE_DIR is the *consumer's* root the moment someone
#     add_subdirectory()s us, silently pointing the include path at the wrong
#     tree. PROJECT_SOURCE_DIR is ours.
#   * an installed package must describe where its headers landed, not where
#     they were built from.
#   * a bare absolute source path makes install(TARGETS ... EXPORT ...) fail
#     outright — observed, before this change:
#       Target "venice-cpp_lib" INTERFACE_INCLUDE_DIRECTORIES property contains
#       path: ".../include" which is prefixed in the source directory.
target_include_directories(${PROJECT_NAME}_lib
  INTERFACE
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

# The C++ standard is a property of this library's public headers, not of the
# build that happens to be compiling them: every fallible entry point in
# include/venice/client.hpp returns std::expected. cmake/toolchain/default.cmake
# sets CMAKE_CXX_STANDARD for *our* builds, but a consumer uses their own
# toolchain — without this they compile our headers at whatever their default is.
target_compile_features(${PROJECT_NAME}_lib INTERFACE cxx_std_23)

target_link_libraries(${PROJECT_NAME}_lib
  INTERFACE c-ares::cares
  INTERFACE httplib::httplib
  INTERFACE nlohmann_json::nlohmann_json
)

# OpenSSL is a public requirement: include/venice/client.hpp includes <httplib.h>,
# which pulls <openssl/ssl.h> once CPPHTTPLIB_OPENSSL_SUPPORT is defined. Linked
# directly rather than leaning on httplib's transitive dependency, because a
# *system* cpp-httplib may have been built without TLS — that failure belongs
# here, at our configure step, not in a consumer's link line.
find_package(OpenSSL REQUIRED)
target_link_libraries(${PROJECT_NAME}_lib
  INTERFACE OpenSSL::SSL
  INTERFACE OpenSSL::Crypto
)

# Threads is a public requirement for the same reason OpenSSL is, and became one
# with VC-06: include/venice/options.hpp spawns a std::thread of our own to
# watch a CancelToken. cpp-httplib links Threads too, so this happens to work
# transitively today — which is exactly the accident worth not depending on. A
# consumer who satisfies httplib through a system find_package inherits whatever
# that package recorded, and our own requirement should not be riding on it.
#
# Not a cmake/deps/ recipe and not in ${PROJECT_NAME}_DEPS: like OpenSSL this is
# a platform facility located by a CMake find module, not a library we fetch.
find_package(Threads REQUIRED)
target_link_libraries(${PROJECT_NAME}_lib
  INTERFACE Threads::Threads
)
