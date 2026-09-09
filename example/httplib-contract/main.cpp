#include <venice/detail/httplib_contract.hpp>
#include <iostream>

int main() {
  // No TLS context, resolver, socket or request is constructed by this consumer.
  std::cout << CPPHTTPLIB_VERSION << '\n';
}
