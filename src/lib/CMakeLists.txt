# ── venice-cpp: header-only (INTERFACE) library ────────────────────────────
add_library(${PROJECT_NAME}_lib INTERFACE)
add_library(${PROJECT_NAME}::lib ALIAS ${PROJECT_NAME}_lib)

target_include_directories(${PROJECT_NAME}_lib
  INTERFACE ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(${PROJECT_NAME}_lib
  INTERFACE httplib::httplib
  INTERFACE nlohmann_json::nlohmann_json
)

find_package(OpenSSL REQUIRED)
target_link_libraries(${PROJECT_NAME}_lib
  INTERFACE OpenSSL::SSL
  INTERFACE OpenSSL::Crypto
)
