// venice-cpp smoke binary — exercises the client against the live API when a
// key is present, else prints the library shape. Real usage comes later; for
// now this proves the header-only client links and runs.

#include <cstdlib>
#include <iostream>
#include <string_view>

#include <venice/venice.hpp>

namespace {

// `--models [type]`: the live check for Model's typed metadata and, since
// VC-13, for the type filter. Prints the fields a picker would actually branch
// on, so a field that parses in a fixture but not on the wire shows up as a
// blank column rather than passing quietly.
//
// `type` is passed through verbatim — text, image, video, tts, embedding,
// inpaint, music, asr, upscale, all — and an empty one sends no filter, which
// Venice answers with text models only. The three runs worth doing:
//
//   --models          -> 106   (the pre-VC-13 behaviour, unchanged)
//   --models image    ->  35
//   --models all      -> 287
//
// /models answers 200 for any bearer token, so VENICE_API_KEY=unused is enough.
auto list_models(const venice::Client& client, std::string_view type) -> int {
  const auto res = client.models(type);
  if (!res) {
    std::cerr << "models failed [" << venice::to_string(res.error().kind) << "] "
              << res.error().message << '\n';
    return EXIT_FAILURE;
  }

  std::cout << res->size() << " models";
  if (!type.empty()) std::cout << " (type=" << type << ')';
  std::cout << '\n';

  for (const auto& m : *res) {
    // The type column earns its place once non-text models are listable: for
    // those, every column after it is a `?`, and without it the output reads
    // like a parse failure rather than a different model_spec shape.
    std::cout << m.id << "  [" << m.type << "]  ctx=";
    if (m.context_length) std::cout << *m.context_length;
    else std::cout << '?';

    std::cout << "  tools=";
    if (m.capabilities && m.capabilities->supports_function_calling)
      std::cout << (*m.capabilities->supports_function_calling ? "yes" : "no");
    else std::cout << '?';

    std::cout << "  in=$";
    if (m.pricing && m.pricing->base.input && m.pricing->base.input->usd)
      std::cout << *m.pricing->base.input->usd;
    else std::cout << '?';

    if (m.name) std::cout << "  (" << *m.name << ')';
    std::cout << '\n';
  }
  return EXIT_SUCCESS;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const char* key = std::getenv("VENICE_API_KEY");
  if (key == nullptr || *key == '\0') {
    std::cerr << "VENICE_API_KEY not set; nothing to call (library links fine).\n";
    return EXIT_SUCCESS;
  }

  const venice::Client client{key};
  if (argc > 1 && std::string_view{argv[1]} == "--models")
    return list_models(client, argc > 2 ? argv[2] : "");

  const std::string prompt = argc > 1 ? argv[1] : "Say hello in one short sentence.";

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
