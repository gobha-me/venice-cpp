// venice-cpp smoke binary — exercises the client against the live API when a
// key is present, else prints the library shape. Real usage comes later; for
// now this proves the header-only client links and runs.

#include <cstdlib>
#include <iostream>

#include <venice/venice.hpp>

auto main(int argc, char** argv) -> int {
  const char* key = std::getenv("VENICE_API_KEY");
  if (key == nullptr || *key == '\0') {
    std::cerr << "VENICE_API_KEY not set; nothing to call (library links fine).\n";
    return EXIT_SUCCESS;
  }

  const std::string prompt = argc > 1 ? argv[1] : "Say hello in one short sentence.";
  venice::Client client{key};

  venice::ChatRequest req;
  req.model = "llama-3.3-70b";
  req.messages = {venice::Message::user(prompt)};

  auto res = client.chat(req);
  if (!res) {
    std::cerr << "chat failed [" << venice::to_string(res.error().kind) << "] "
              << res.error().message << '\n';
    return EXIT_FAILURE;
  }

  std::cout << res->content << '\n';
  if (res->usage) {
    std::cerr << "(" << res->usage->prompt_tokens << " prompt / "
              << res->usage->completion_tokens << " completion tokens)\n";
  }
  return EXIT_SUCCESS;
}
