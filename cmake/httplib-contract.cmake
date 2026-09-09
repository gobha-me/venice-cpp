# Shared by source acquisition and the installed Venice package. Check actual
# usage requirements/header APIs, not only a package's version declaration.
function(venice_cpp_check_httplib out_valid out_reason contract_include)
  set(${out_valid} FALSE PARENT_SCOPE)
  set(${out_reason} "cpp-httplib must provide the canonical header-only OpenSSL3 0.51 target" PARENT_SCOPE)
  if (NOT TARGET httplib::httplib)
    return()
  endif ()
  get_target_property(_type httplib::httplib TYPE)
  if (NOT _type STREQUAL "INTERFACE_LIBRARY")
    return()
  endif ()

  # try_compile exports imported targets, but cannot directly export a fetched
  # interface target. Copy its usage requirements unchanged into a private probe
  # target; generator expressions still select the real build/include profile.
  if (NOT TARGET venice_cpp_httplib_contract_probe)
    add_library(venice_cpp_httplib_contract_probe INTERFACE IMPORTED)
  endif ()
  foreach (_property IN ITEMS
      INTERFACE_INCLUDE_DIRECTORIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES
      INTERFACE_COMPILE_DEFINITIONS INTERFACE_COMPILE_OPTIONS
      INTERFACE_COMPILE_FEATURES INTERFACE_LINK_LIBRARIES INTERFACE_LINK_OPTIONS)
    get_target_property(_value httplib::httplib ${_property})
    if (_value STREQUAL "_value-NOTFOUND")
      set(_value "")
    endif ()
    # CMake 3.28's try_compile export does not recreate aliases nested inside
    # link generator expressions. Resolve visible aliases without changing
    # whether that dependency is enabled or adding missing link requirements.
    if (_property STREQUAL "INTERFACE_LINK_LIBRARIES")
      string(REGEX MATCHALL "[A-Za-z0-9_.+-]+::[A-Za-z0-9_.+-]+" _references "${_value}")
      foreach (_reference IN LISTS _references)
        if (TARGET ${_reference})
          get_target_property(_aliased ${_reference} ALIASED_TARGET)
          if (_aliased)
            string(REPLACE "${_reference}" "${_aliased}" _value "${_value}")
          endif ()
        endif ()
      endforeach ()
    endif ()
    set_property(TARGET venice_cpp_httplib_contract_probe PROPERTY ${_property} "${_value}")
  endforeach ()
  include(CheckCXXSourceCompiles)
  include(CMakePushCheckState)
  cmake_push_check_state(RESET)
  set(CMAKE_REQUIRED_LIBRARIES venice_cpp_httplib_contract_probe)
  set(CMAKE_REQUIRED_INCLUDES "${contract_include}")
  set(CMAKE_CXX_STANDARD 23)
  # Recheck on every configure; a changed header/target must not reuse old proof.
  unset(_venice_cpp_httplib_contract_works CACHE)
  check_cxx_source_compiles([=[
#include <venice/detail/httplib_contract.hpp>
#include <chrono>
int main() {
  httplib::SSLClient client("fixture.invalid", 443);
  client.enable_system_ca(false);
  client.enable_server_certificate_verification(true);
  client.enable_server_hostname_verification(true);
  client.set_follow_location(false);
  client.set_path_encode(false);
  client.set_payload_max_length(1024);
  client.set_max_timeout(std::chrono::milliseconds{100});
  client.set_ca_cert_store(nullptr);
  auto* context = client.tls_context();
  (void)context;
  httplib::UploadFormDataItems items;
  (void)items;
  httplib::Request request;
  request.response_handler = [](const httplib::Response&) { return true; };
  request.content_receiver = [](const char*, std::size_t, std::uint64_t,
                                std::uint64_t) { return true; };
  httplib::Response response;
  httplib::Error error;
  // Compile/link the real public send path. This program is never executed.
  return client.send(request, response, error) ? 0 : 1;
}
]=] _venice_cpp_httplib_contract_works)
  cmake_pop_check_state()
  if (_venice_cpp_httplib_contract_works)
    set(${out_valid} TRUE PARENT_SCOPE)
    set(${out_reason} "" PARENT_SCOPE)
  else ()
    set(${out_reason} "cpp-httplib selected header or usage requirements do not satisfy the OpenSSL3 0.51 contract" PARENT_SCOPE)
  endif ()
endfunction()
