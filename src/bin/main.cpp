// venice-cpp smoke binary — exercises the client against the live API when a
// key is present, else prints the library shape. Real usage comes later; for
// now this proves the header-only client links and runs.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <venice/venice.hpp>

namespace {

// ── choosing a model from the live catalogue ──────────────────────────────
//
// A model chosen by capability, plus the runners-up it beat. Both halves are
// the point. AGENTS.md makes naming the alternates a rule after VC-18, where
// --tools auto-picked one family and nothing on screen suggested the next one
// would answer differently — it did, and the bug read as a library defect for
// as long as it did because of that.
//
// VC-17 (#28) is the second bug that rule would have caught, and this helper
// exists because writing the rule down in one leg did not put it in the others.
// #28 was filed as "Venice does not send Usage's nested detail objects" on the
// strength of a --stream run against `gemini-3-6-flash` — which is simply the
// first supportsReasoning entry in /models?type=text, and one of the two models
// in a seven-model sweep that reports neither field. Five of the other six send
// prompt_tokens_details.cached_tokens and two send
// completion_tokens_details.reasoning_tokens, at exactly the nesting the ticket
// called unobserved. One model settles nothing about a per-family shape.
struct ModelPick {
  std::string chosen{};
  std::vector<std::string> alternates{};
};

// nullopt after printing why: the list call failed, or nothing claimed the flag.
// Both are this leg's failure to report, not something a caller can fix.
//
// The flag is a pointer-to-member rather than a name, so a typo cannot compile
// — the same reason detail::kCapabilityBoolFields pairs the field with its wire
// key instead of looking one up by string.
auto pick_by_capability(const venice::Client& client,
                        std::optional<bool> venice::ModelCapabilities::*flag,
                        std::string_view flag_name,
                        std::size_t max_alternates = 4) -> std::optional<ModelPick> {
  const auto models = client.models("text");
  if (!models) {
    std::cerr << "models failed [" << venice::to_string(models.error().kind) << "] "
              << models.error().message << '\n';
    return std::nullopt;
  }

  ModelPick pick;
  for (const auto& m : *models) {
    if (!m.capabilities) continue;
    const auto& claimed = (*m.capabilities).*flag;
    if (!claimed || !*claimed) continue;
    if (pick.chosen.empty())
      pick.chosen = m.id;
    else if (pick.alternates.size() < max_alternates)
      pick.alternates.push_back(m.id);
    else
      break;
  }

  if (pick.chosen.empty()) {
    std::cerr << "no text model reported " << flag_name
              << " -- either none does, or that flag is not where Model expects it\n";
    return std::nullopt;
  }
  return pick;
}

// `rerun` names the command that runs this same leg on a named model, because
// a list of alternates nobody can act on is decoration. Prints nothing beyond
// the model when the caller named one — there are no runners-up to a choice
// that was not made here.
void report_pick(const ModelPick& pick, std::string_view flag_name, std::string_view rerun) {
  std::cerr << "model: " << pick.chosen << '\n';
  if (pick.alternates.empty()) return;
  std::cerr << "others claiming " << flag_name << ": ";
  for (std::size_t i = 0; i < pick.alternates.size(); ++i)
    std::cerr << (i != 0U ? ", " : "") << pick.alternates[i];
  std::cerr << "\n  (" << rerun
            << " runs this same leg on any of them; server behaviour that varies by\n"
               "   model family is invisible to a leg that only ever runs one)\n";
}

// `--models [type]`: the live check for Model's typed metadata and, since
// VC-13, for the type filter. Prints the fields a picker would actually branch
// on, so a field that parses in a fixture but not on the wire shows up as a
// blank column rather than passing quietly.
//
// `type` is passed through verbatim — text, image, video, tts, embedding,
// inpaint, music, asr, upscale, all — and an empty one sends no filter, which
// Venice answers with text models only. The runs worth doing:
//
//   --models          the pre-VC-13 behaviour, and it must still equal
//   --models text     this one exactly
//   --models all      every modality; roughly three times the bare count
//
// Deliberately no expected numbers here: Venice's catalogue moved by twelve
// models in the ten days between VC-03 and VC-13, so a count written into a
// comment is wrong within the month. The bare-equals-text relation is the part
// that holds.
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

// `--characters [search]`: the live check VC-04 could not run offline, and the
// reason its PR says "documented, not captured".
//
// test/08characters/ pins the shape Venice's OpenAPI document describes, but
// nothing there has been on a wire: unlike /models, this endpoint answers 402
// unauthenticated and 401 to a junk bearer, so no fixture could be captured
// without a real key. The columns below are exactly the fields a TUI picker
// would branch on, and an absent one prints '?' — so a key that parses in the
// fixture and not in reality shows up as a blank column rather than passing
// quietly. If one disagrees, the fixture is what needs correcting;
// Character::raw is why that is recoverable.
//
// Two things this leg is specifically watching for, both of them guesses until
// it runs:
//
//   * that `slug` really is always present, since the parse skips entries
//     without one — a listing that comes back short is that guess being wrong.
//   * that the page really is 50 by default. The count printed below against a
//     `--characters` with no search is the whole evidence for the pagination
//     paragraph in client.hpp.
//
// The optional argument is a search term, which is also the cheapest way to see
// that a query parameter survives encoding: `--characters "alan watts"`.
auto list_characters(const venice::Client& client, std::string_view search) -> int {
  venice::CharacterQuery q;
  if (!search.empty()) q.search = std::string{search};

  const auto res = client.characters(q);
  if (!res) {
    // The body is printed and not just the message because this endpoint's
    // documented failures are 402 and 401, and "HTTP 402" alone names nothing.
    // The 402 body is where "Authentication required" actually appears.
    std::cerr << "characters failed [" << venice::to_string(res.error().kind) << "] "
              << res.error().message << '\n';
    if (!res.error().body.empty()) std::cerr << res.error().body << '\n';
    return EXIT_FAILURE;
  }

  // Both counts, because one number cannot answer both questions this leg
  // exists to ask. `returned` is the server's page — if it is 50 with no limit
  // set, the default-page claim holds. `entries` is what survived the parse —
  // if it is lower, `slug` is not always present after all, which is the other
  // guess. Printing only the second would make the two indistinguishable.
  std::cout << res->entries.size() << " usable of " << res->returned << " returned";
  if (!search.empty()) std::cout << " (search=" << search << ')';
  std::cout << '\n';
  if (res->returned > res->entries.size()) {
    std::cout << "   <-- " << res->returned - res->entries.size()
              << " entry/entries skipped for want of a slug: the schema says "
                 "slug is required, so this is the schema being wrong\n";
  }
  if (res->returned == 0) {
    std::cout << "   <-- ZERO returned: the account sees no characters at all\n";
  }

  for (const auto& c : res->entries) {
    std::cout << c.slug << "  model=";
    if (c.model_id) std::cout << *c.model_id;
    else std::cout << '?';

    std::cout << "  web=";
    if (c.web_enabled) std::cout << (*c.web_enabled ? "yes" : "no");
    else std::cout << '?';

    std::cout << "  adult=";
    if (c.adult) std::cout << (*c.adult ? "yes" : "no");
    else std::cout << '?';

    std::cout << "  rating=";
    if (c.stats && c.stats->average_rating) std::cout << *c.stats->average_rating;
    else std::cout << '?';

    std::cout << "  tags=" << c.tags.size();
    if (c.name) std::cout << "  (" << *c.name << ')';
    std::cout << '\n';
  }
  return EXIT_SUCCESS;
}

// The live check VC-05 could not run offline, and the reason the PR says
// "documented, not measured".
//
// Everything in stream.hpp about *shape* — where reasoning_content sits, where
// cached_tokens is nested, how tool-call fragments are keyed — came from
// Venice's published docs rather than a capture, because /models answers for
// any bearer token but chat does not, and the implementing environment had no
// key. This is where a key-holder confirms it. Run:
//
//     VENICE_API_KEY=... venice-cpp --stream "think step by step: 2+2?"
//
// and read the report. Every line of it is a claim the offline suite asserts
// against a hand-written fixture; if one disagrees with the live API, the
// fixture in test/07stream/ is what needs correcting.
auto stream_report(const venice::Client& client, const std::string& prompt) -> int {
  // The model is chosen from the live list by capability, not hardcoded. This
  // leg used to name `deepseek-r1-671b` and by 2026-08-09 that model was gone
  // from the catalogue, so the one command that settles VC-05 answered HTTP 404
  // instead — a live check that cannot run is worse than no live check, because
  // it looks like a failure of the thing under test. It is the same hazard the
  // --models leg records about hardcoded *counts*, and a model id ages faster.
  // Picking on supports_reasoning also gives that VC-03 flag a live use, the
  // way tools_report does for supports_function_calling.
  //
  // It also names the runners-up, which it did not until VC-17. Picking the
  // first entry and printing only that is what let #28 be filed against the one
  // model family that answers this question differently from most; see
  // pick_by_capability.
  const auto pick = pick_by_capability(client, &venice::ModelCapabilities::supports_reasoning,
                                       "supportsReasoning");
  if (!pick) return EXIT_FAILURE;
  report_pick(*pick, "supportsReasoning", "`venice-cpp --usage <id>`");

  venice::ChatRequest req;
  req.model = pick->chosen;  // a reasoning model, so thinking has somewhere to come from
  req.messages = {venice::Message::user(prompt)};

  venice::StreamAccumulator acc;
  std::size_t content_frames = 0;
  std::size_t reasoning_frames = 0;

  const auto res = client.chat_stream(req, acc, [&](const venice::StreamDelta& d) {
    if (d.reasoning_content) ++reasoning_frames;
    if (d.content) {
      ++content_frames;
      std::cout << *d.content << std::flush;
    }
    return true;
  });
  std::cout << '\n';

  if (!res) {
    std::cerr << "stream failed [" << venice::to_string(res.error().kind) << "] "
              << res.error().message << '\n';
    return EXIT_FAILURE;
  }

  const auto msg = acc.message();
  std::cerr << "\n-- what the stream carried --\n"
            << "chunks retained    : " << acc.chunks().size() << '\n'
            << "content frames     : " << content_frames << '\n'
            << "reasoning frames   : " << reasoning_frames
            << (reasoning_frames == 0
                    ? "   <-- ZERO: either the model did not think, or"
                      " reasoning_content is not where we look\n"
                    : "\n")
            << "reasoning chars    : " << (msg.reasoning_content ? msg.reasoning_content->size() : 0)
            << '\n'
            << "tool calls         : " << (msg.tool_calls ? msg.tool_calls->size() : 0) << '\n'
            << "finish_reason      : " << res->finish_reason << '\n';

  // Absence here is a fact about the model, not a symptom. Both of these read
  // "(absent -- check the nesting)" until VC-17, which pointed the reader at a
  // parse bug that does not exist: the nestings are right, and whether they
  // arrive is per-family. `--usage` prints the verbatim object, which is the
  // only thing that can tell those two apart.
  if (res->usage) {
    std::cerr << "prompt/completion  : " << res->usage->prompt_tokens << '/'
              << res->usage->completion_tokens << '\n'
              << "reasoning_tokens   : ";
    if (res->usage->reasoning_tokens) std::cerr << *res->usage->reasoning_tokens << '\n';
    else std::cerr << "(absent -- this family does not report one; venice-cpp --usage)\n";
    std::cerr << "cached_tokens      : ";
    if (res->usage->cached_tokens) std::cerr << *res->usage->cached_tokens << '\n';
    else std::cerr << "(absent -- this family does not report one; venice-cpp --usage)\n";
  } else {
    std::cerr << "usage              : (no usage frame arrived at all)\n";
  }

  // The streamed cost, off the assembled response rather than the chunks — the
  // only place a reader looking at the reply's *shape* sees the field at all.
  // `--usage` is the check; this is the shape report, and a field missing from
  // it reads as a gap in the library rather than a gap in the leg.
  std::cerr << "cost               : ";
  if (res->cost && res->cost->diem) std::cerr << *res->cost->diem << " diem\n";
  else if (res->cost) std::cerr << "(object arrived, no diem in it)\n";
  else std::cerr << "(no cost frame arrived at all)\n";

  // The claim that matters most, and the one no fixture can settle: whether the
  // assembled turn is something Venice will accept back.
  std::cerr << "\n-- the turn, as it would be replayed --\n"
            << nlohmann::json(msg).dump(2) << '\n';
  return EXIT_SUCCESS;
}

// `--tools [model]`: the live check VC-08 could not run offline, for the same
// reason --stream exists. Every offline case in test/02request/ asserts what
// to_json_body *emits*; none of them can assert that Venice *accepts* it. The
// nesting under "function", the spelling of tool_choice, whether
// parallel_tool_calls is even honoured on this API — all of it is read off
// Venice's published docs.
//
//     VENICE_API_KEY=... venice-cpp --tools
//
// Two legs, and the second is the one that matters. Leg one offers a function
// and sees whether a tool_call comes back — that alone proves the `tools` shape
// parsed. Leg two answers the call and sends the whole history back, which is
// the only thing that proves the assistant turn plus a tool result is a
// conversation Venice will continue. A run that stops after leg one has checked
// half the round trip.
//
// If a field disagrees with the fixtures, the fixture is what needs correcting;
// ChatResponse::raw and Message::raw are why a wrong guess here is recoverable.
auto tools_report(const venice::Client& client, std::string_view model) -> int {
  // Picking by capability gives VC-03's supports_function_calling flag its
  // first live use — a flag nothing has ever branched on is a flag nobody knows
  // is parsed — and collecting the runners-up is the fix VC-18 paid for. That
  // logic lives in pick_by_capability now, because VC-17 found --stream had
  // never grown it and filed a wrong conclusion as a result.
  ModelPick pick;
  if (model.empty()) {
    auto picked = pick_by_capability(client, &venice::ModelCapabilities::supports_function_calling,
                                     "supportsFunctionCalling");
    if (!picked) return EXIT_FAILURE;
    pick = *std::move(picked);
  } else {
    pick.chosen = std::string{model};
  }
  const std::string& chosen = pick.chosen;
  const auto& alternates = pick.alternates;

  venice::ChatRequest req;
  req.model = chosen;
  req.messages = {venice::Message::user("What is the weather in San Francisco?")};
  req.tools = std::vector<nlohmann::json>{venice::tools::function(
      "get_weather", "Look up the current weather for a city",
      nlohmann::json::parse(R"({"type":"object",
                                "properties":{"city":{"type":"string"}},
                                "required":["city"]})"))};
  req.tool_choice = venice::tool_choice::automatic();
  req.parallel_tool_calls = false;

  report_pick(pick, "supportsFunctionCalling", "`venice-cpp --tools <id>`");
  std::cerr << "\n-- leg 1: the body actually sent --\n" << req.to_json_body(false).dump(2) << '\n';

  const auto first = client.chat(req);
  if (!first) {
    std::cerr << "\nchat failed [" << venice::to_string(first.error().kind) << "] "
              << first.error().message << "\nbody: " << first.error().body << '\n';
    return EXIT_FAILURE;
  }

  // Borrowed, not copied, and it outlives every use below — `first` is alive to
  // the end of the function. nullptr covers three different absences at once: no
  // message, no tool_calls, or an engaged-but-empty list, and that last one is
  // why the count rather than the optional decides. front() on an empty vector
  // would be UB, and a gateway echoing "tool_calls": [] is not far-fetched.
  const auto* calls = first->message && first->message->tool_calls
                          ? &*first->message->tool_calls
                          : nullptr;
  const auto count = calls != nullptr ? calls->size() : 0;

  std::cerr << "\n-- leg 1: what came back --\n"
            << "finish_reason      : " << first->finish_reason << '\n'
            << "tool calls         : " << count;
  if (count == 0) {
    std::cerr << "   <-- ZERO: the request was accepted but nothing was called."
                 " Either the model declined, or `tools` is not reaching it\n";
    return EXIT_SUCCESS;
  }
  std::cerr << '\n';

  const auto& call = calls->front();
  std::cerr << "id / name          : " << call.id << " / " << call.name << '\n'
            << "arguments          : " << call.arguments << '\n';
  if (const auto parsed = call.parsed_arguments(); !parsed)
    std::cerr << "arguments DID NOT PARSE: " << parsed.error().message << '\n';

  std::cerr << "thought_signature  : ";
  if (call.thought_signature)
    std::cerr << "present, " << call.thought_signature->size()
              << " chars  <-- this family requires it echoed on leg 2 (VC-18)\n";
  else
    std::cerr << "(none -- this family does not use one)\n";

  // Leg two. Replay the assistant turn verbatim, then answer the call.
  //
  // Read the echo off the SERIALIZED body, not off the struct. VC-18's defect
  // lived in ToolCall::to_json, downstream of the field — re-reading
  // call.thought_signature here would only restate what leg 1 just printed and
  // would have stayed quiet through the entire bug.
  auto assistant_turn = *first->message;
  const nlohmann::json replayed = assistant_turn;
  const bool echoed = replayed.contains("tool_calls") && !replayed.at("tool_calls").empty() &&
                      replayed.at("tool_calls").at(0).contains("thought_signature");
  const bool signature_lost = call.thought_signature.has_value() && !echoed;

  std::cerr << "\n-- the turn, as it would be replayed --\n" << replayed.dump(2) << '\n';
  if (signature_lost)
    std::cerr << "\nSIGNATURE SEEN BUT NOT ECHOED -- VC-18 has regressed; leg 2 will 400\n";

  req.messages.push_back(std::move(assistant_turn));
  req.messages.push_back(venice::Message::tool(call.id, R"({"temp_f":68,"sky":"fog"})"));

  const auto second = client.chat(req);
  if (!second) {
    std::cerr << "\n-- leg 2 REJECTED --\n[" << venice::to_string(second.error().kind) << "] "
              << second.error().message << "\nbody: " << second.error().body
              << "\n\nThis is the half no fixture can settle: the turn assembled here is not"
                 " something Venice will take back.\nRun the same two legs on another family"
                 " before reading this as a library bug:\n"
              // The runners-up are only on screen when this leg auto-picked. A
              // caller who named the model got no such line, so pointing at
              // "the list above" would be pointing at nothing.
              << (alternates.empty()
                      ? "`venice-cpp --tools` with no argument lists other models that claim it.\n"
                      : "`venice-cpp --tools <id>` with any id from the list above.\n");
    return EXIT_FAILURE;
  }

  std::cerr << "\n-- leg 2: the tool result was accepted --\n";
  std::cout << second->content << '\n';
  // A pass is not enough. A future model that tolerates a stripped signature
  // would let VC-18 regress silently behind a green leg 2, so a signature seen
  // and not carried back fails the run on its own.
  return signature_lost ? EXIT_FAILURE : EXIT_SUCCESS;
}

// ── `--usage [model]`: what Venice actually puts in a response ────────────
//
// The leg VC-17 (#28) needed and did not have, since VC-20 (#34) covering the
// response's billing metadata whole rather than only the `usage` object.
// `Usage` models two nested fields, `prompt_tokens_details.cached_tokens` and
// `completion_tokens_details.reasoning_tokens`, and no offline fixture can say
// whether the API sends them — #28 was filed claiming it does not, off a single
// model that does not.
//
// So this prints the **verbatim** objects from both the streaming and the
// non-streaming path, beside the typed structs parsed from them. Verbatim is
// the whole point: a typed field reading absent means either "the server did
// not send it" or "we are looking in the wrong place", and only the raw object
// distinguishes them. That ambiguity is what cost #28 a wrong premise.
//
// It is a check and not a printer. A modeled key present in the raw object but
// absent from the typed struct is a parse bug — case (c) in #28's own scope,
// the only one of the three that is — and fails the run.
//
// **And it reports the whole envelope, not only the sub-object it was written
// for.** That is #34's finding rather than its subject: `cost` is a sibling of
// `usage`, so the unmodeled-key report below could never have named it, and it
// rode untyped for three releases with nothing looking one level up. The
// envelope report found `service_tier` on four families and four more keys on
// llama-3.3-70b the first time it ran.

// Every key Usage reads, so anything else in the object can be named as
// unmodeled rather than disappearing silently into ChatResponse::raw.
constexpr std::array<std::string_view, 6> kModeledUsageKeys{
    "prompt_tokens",  "completion_tokens",        "total_tokens",
    "cached_tokens",  "prompt_tokens_details",    "completion_tokens_details"};

// True when the typed parse disagrees with the object it came from.
auto report_usage(const nlohmann::json& raw, const std::optional<venice::Usage>& typed) -> bool {
  std::cerr << "usage, verbatim    : " << raw.dump() << '\n';
  if (!typed) {
    std::cerr << "  TYPED USAGE ABSENT while a usage object arrived -- parse bug\n";
    return true;
  }

  // What the raw object says, independent of the parse. nullopt means the key
  // is not there at all; a present 0 is a real answer and not the same thing —
  // both deepseek-v4-pro and llama-3.3-70b report an explicit cached_tokens 0,
  // which is why Usage's fields are optional rather than defaulted.
  const auto nested = [&raw](const char* obj, const char* key) -> std::optional<int> {
    const auto o = raw.find(obj);
    if (o == raw.end() || !o->is_object()) return std::nullopt;
    const auto k = o->find(key);
    if (k == o->end() || !k->is_number_integer()) return std::nullopt;
    return k->get<int>();
  };
  const auto raw_cached = nested("prompt_tokens_details", "cached_tokens");
  const auto raw_reasoning = nested("completion_tokens_details", "reasoning_tokens");

  const auto show = [](const char* label, std::optional<int> from_raw, std::optional<int> from_typed) {
    std::cerr << label;
    if (from_typed) std::cerr << *from_typed;
    else std::cerr << "(absent)";
    if (from_raw != from_typed)
      std::cerr << "   <-- RAW SAYS " << (from_raw ? std::to_string(*from_raw) : "(absent)")
                << ": the wire moved and the parse did not";
    std::cerr << '\n';
  };
  show("typed cached_tokens: ", raw_cached, typed->cached_tokens);
  show("typed reasoning    : ", raw_reasoning, typed->reasoning_tokens);

  // Name what we do not model. cache_read_input_tokens turned up this way — a
  // flat sibling mirroring the nested cached_tokens, redundant in all 21
  // captures of the VC-17 sweep, which is why it stays untyped.
  std::vector<std::string> unmodeled;
  for (const auto& [k, v] : raw.items())
    if (std::find(kModeledUsageKeys.begin(), kModeledUsageKeys.end(), k) ==
        kModeledUsageKeys.end())
      unmodeled.push_back(k);
  if (!unmodeled.empty()) {
    std::cerr << "unmodeled usage keys: ";
    for (std::size_t i = 0; i < unmodeled.size(); ++i)
      std::cerr << (i != 0U ? ", " : "") << unmodeled[i];
    std::cerr << "   (reachable via ChatResponse::raw)\n";
  }

  return raw_cached != typed->cached_tokens || raw_reasoning != typed->reasoning_tokens;
}

// Every key `ChatResponse` reads off the response *envelope* — id, model,
// created, system_fingerprint, venice_parameters, choices, usage, cost — plus
// `object`, which it does NOT read. That one is here deliberately and is the
// only entry that is not a modeled field: it is the discriminator
// ("chat.completion" vs "chat.completion.chunk"), it is on every body, and
// naming it every run would bury the keys this report exists to surface. Any
// OTHER known-but-untyped key belongs in the report, not in here — the value of
// the list is that it is short and every entry is accounted for.
//
// The list above is the same idea one level down, and the gap between them is
// VC-20 (#34): `cost` is a sibling of `usage`, not a key inside it, so
// report_usage's set-difference can never see it at any value of
// kModeledUsageKeys. It rode untyped for three releases not because anyone
// deferred it but because no leg ever named the keys *beside* the sub-object it
// was written for.
//
// Note this is the NON-STREAMED key set. A chunk envelope models strictly less
// — note_envelope takes only id and model, delta_from_chunk adds choices, usage
// and cost — so created / system_fingerprint / venice_parameters on a chunk are
// filtered out here while the accumulator does in fact drop them. Under-reports
// the stream, and said out loud rather than left to be found.
constexpr std::array<std::string_view, 9> kModeledBodyKeys{
    "id",      "object", "model", "created", "system_fingerprint",
    "choices", "usage",  "cost",  "venice_parameters"};

// Name what an envelope carries that nothing models. Separate from report_cost
// because the discovery check must not be gated on the thing being discovered:
// if Venice renames `cost`, this still runs and still names the new key.
void report_envelope_keys(const nlohmann::json& envelope) {
  if (!envelope.is_object()) return;
  std::vector<std::string> unmodeled;
  for (const auto& [k, v] : envelope.items())
    if (std::find(kModeledBodyKeys.begin(), kModeledBodyKeys.end(), k) == kModeledBodyKeys.end())
      unmodeled.push_back(k);
  if (unmodeled.empty()) return;
  std::cerr << "unmodeled body keys: ";
  for (std::size_t i = 0; i < unmodeled.size(); ++i)
    std::cerr << (i != 0U ? ", " : "") << unmodeled[i];
  std::cerr << "   (reachable via ChatResponse::raw)\n";
}

// True when the typed parse disagrees with the envelope it came from.
//
// Takes the whole body or chunk, not a sub-object — that is the entire point.
auto report_cost(const nlohmann::json& envelope, const std::optional<venice::Price>& typed)
    -> bool {
  const auto c = envelope.find("cost");

  if (c == envelope.end()) {
    std::cerr << "cost, verbatim     : (no cost key in the body at all)\n";
    // Not a bug. Every family swept for VC-20 sent one, but reading an absence
    // as a parse error is exactly the mistake that filed #28 against a shape
    // that was merely per-family.
    return false;
  }
  if (!c->is_object()) {
    // A DIFFERENT answer from the one above, and collapsing the two would undo
    // this leg's reason for existing: the key is here and has changed shape, so
    // the typed field is disengaged because we are looking in the wrong place,
    // not because the server went quiet. Fails the run.
    std::cerr << "cost, verbatim     : " << c->dump()
              << "\n  COST IS PRESENT BUT NOT AN OBJECT -- the wire shape moved\n";
    return true;
  }
  std::cerr << "cost, verbatim     : " << c->dump() << '\n';
  if (!typed) {
    std::cerr << "  TYPED COST ABSENT while a cost object arrived -- parse bug\n";
    return true;
  }

  // What the envelope says, independent of the parse — and read through the
  // SAME predicate detail::opt_double uses. is_number(), not is_number_float():
  // the capture in #34 is `"usd":0`, a JSON *integer*, and a float-only reader
  // would call it absent and then report a mismatch against a parse that is
  // behaving exactly as documented.
  const auto wire = [&c](const char* key) -> std::optional<double> {
    const auto k = c->find(key);
    if (k == c->end() || !k->is_number()) return std::nullopt;
    return k->get<double>();
  };
  // The wire type is half the question the sweep exists to answer, so print it
  // rather than only the value.
  const auto kind = [&c](const char* key) -> const char* {
    const auto k = c->find(key);
    if (k == c->end()) return "missing";
    if (k->is_number_integer()) return "integer";
    if (k->is_number_float()) return "float";
    return "not a number";
  };
  std::cerr << "cost wire types    : usd " << kind("usd") << ", diem " << kind("diem") << '\n';

  // Read once each, so the numbers printed below and the numbers that decide
  // the exit status are the same reads rather than two a reader has to prove
  // equal. Matches report_usage's raw_cached / raw_reasoning above.
  const auto raw_usd = wire("usd");
  const auto raw_diem = wire("diem");

  // max_digits10 on both sides. Default ostream precision is 6 significant
  // digits and std::to_string is 6 *decimal places*, which on a tool whose one
  // job is reporting the verbatim wire value would print a diem of 1.7e-7 as
  // "0.000000" — a zero, in the leg whose entire finding is that a printed zero
  // must not be confused with a real one.
  const auto num = [](double v) {
    std::ostringstream os;
    os << std::setprecision(std::numeric_limits<double>::max_digits10) << v;
    return os.str();
  };
  const auto show = [&num](const char* label, std::optional<double> from_raw,
                           std::optional<double> from_typed) {
    std::cerr << label << (from_typed ? num(*from_typed) : "(absent)");
    if (from_raw != from_typed)
      std::cerr << "   <-- RAW SAYS " << (from_raw ? num(*from_raw) : "(absent)")
                << ": the wire moved and the parse did not";
    std::cerr << '\n';
  };
  show("typed usd          : ", raw_usd, typed->usd);
  show("typed diem         : ", raw_diem, typed->diem);

  // An engaged usd of 0 is NOT a failure, and this is the one line in the leg
  // that could most easily be written wrong. It has been 0 on every capture,
  // including a call whose rate-card value was $0.0645 — so 0 means "not
  // reported for this account", and a check that treated it as suspect would
  // fire on every run forever.
  return raw_usd != typed->usd || raw_diem != typed->diem;
}

auto usage_report(const venice::Client& client, std::string_view model) -> int {
  ModelPick pick;
  if (model.empty()) {
    auto picked =
        pick_by_capability(client, &venice::ModelCapabilities::supports_reasoning,
                           "supportsReasoning");
    if (!picked) return EXIT_FAILURE;
    pick = *std::move(picked);
  } else {
    pick.chosen = std::string{model};
  }
  report_pick(pick, "supportsReasoning", "`venice-cpp --usage <id>`");

  venice::ChatRequest req;
  req.model = pick.chosen;
  // Long enough to have something to cache: cached_tokens can only be non-zero
  // on a request that actually hit a prompt cache, and #28's original run had
  // no reason to. Re-running this leg back to back is the cache probe.
  req.messages = {venice::Message::user("Think step by step: what is 17 * 23?")};

  bool mismatch = false;

  std::cerr << "\n-- non-streaming --\n";
  const auto nonstream = client.chat(req);
  if (!nonstream) {
    std::cerr << "chat failed [" << venice::to_string(nonstream.error().kind) << "] "
              << nonstream.error().message << '\n';
    return EXIT_FAILURE;
  }
  if (const auto u = nonstream->raw.find("usage");
      u != nonstream->raw.end() && u->is_object())
    mismatch = report_usage(*u, nonstream->usage) || mismatch;
  else
    std::cerr << "usage, verbatim    : (no usage key in the body at all)\n";
  // The whole body, not the usage sub-object. `||` ordering is deliberate: both
  // reports run, so a cost mismatch does not hide a usage one or vice versa.
  report_envelope_keys(nonstream->raw);
  mismatch = report_cost(nonstream->raw, nonstream->cost) || mismatch;

  std::cerr << "\n-- streaming --\n";
  venice::StreamAccumulator acc;
  const auto streamed = client.chat_stream(req, acc, [](const venice::StreamDelta&) { return true; });
  if (!streamed) {
    std::cerr << "stream failed [" << venice::to_string(streamed.error().kind) << "] "
              << streamed.error().message << '\n';
    return EXIT_FAILURE;
  }

  // Off the retained chunks, not off the typed Usage — the accumulator parses
  // the frame and keeps the struct, so the struct is exactly what cannot answer
  // "is this key on the wire". chunks() is where the verbatim record lives, and
  // it needed no library change to reach.
  const nlohmann::json* frame = nullptr;
  std::size_t usage_frames = 0;
  // The cost frame is scanned separately and kept as the whole *chunk*, because
  // report_cost reads the envelope: `cost` is a sibling of `usage`, so handing
  // it the usage sub-object would be handing it the one object that cannot
  // contain the key. Counting them is not decoration either — a second cost
  // frame would mean last-wins is the wrong accumulation rule, and that is the
  // single thing the sweep can measure that changes a line of the library
  // rather than a line of prose.
  //
  // Envelope keys are reported over the UNION of every chunk, not just the
  // cost-bearing one: a new sibling could arrive on an opening or content frame,
  // and a discovery check that only looks where the last discovery happened is
  // how `cost` stayed invisible for three releases.
  const nlohmann::json* cost_chunk = nullptr;
  std::size_t cost_frames = 0;
  std::vector<nlohmann::json> seen_costs;
  nlohmann::json chunk_key_union = nlohmann::json::object();
  for (const auto& c : acc.chunks()) {
    if (!c.is_object()) continue;
    for (const auto& [k, v] : c.items()) chunk_key_union[k] = nullptr;
    if (const auto u = c.find("usage"); u != c.end() && u->is_object()) {
      ++usage_frames;
      frame = &*u;  // last wins, matching StreamAccumulator::ingest
    }
    if (const auto k = c.find("cost"); k != c.end()) {
      ++cost_frames;
      cost_chunk = &c;
      if (std::find(seen_costs.begin(), seen_costs.end(), *k) == seen_costs.end())
        seen_costs.push_back(*k);
    }
  }
  const std::size_t distinct_costs = seen_costs.size();
  report_envelope_keys(chunk_key_union);

  std::cerr << "usage frames       : " << usage_frames << '\n';
  if (frame != nullptr) mismatch = report_usage(*frame, streamed->usage) || mismatch;
  else std::cerr << "usage, verbatim    : (no chunk carried a usage object)\n";

  std::cerr << "cost frames        : " << cost_frames << '\n';
  if (cost_chunk != nullptr) mismatch = report_cost(*cost_chunk, streamed->cost) || mismatch;
  else std::cerr << "cost, verbatim     : (no chunk carried a cost object)\n";

  // The accumulation rule, checked rather than assumed. Every stream swept for
  // VC-20 carried exactly ONE cost frame, so StreamAccumulator's last-wins is
  // uncontradicted but also unproven.
  //
  // The trigger is a second frame, NOT a second *distinct value*, and that is
  // the whole point of the check. Requiring distinctness would pass the shape
  // most likely to mean "these are increments, sum them" — two frames each
  // carrying 0.0005 for a call that charged 0.001 — and last-wins would then
  // report half the true charge with the run exiting 0. Any repeat is news.
  if (cost_frames > 1) {
    std::cerr << "\nMULTIPLE COST FRAMES (" << cost_frames << ", " << distinct_costs
              << " distinct) -- StreamAccumulator takes the last and that may now be wrong.\n"
                 "VC-20 measured exactly one per stream. Equal values may be increments to\n"
                 "sum; differing ones may mean last-wins should be first. Either way the\n"
                 "accumulation rule needs re-opening against this capture.\n";
    mismatch = true;
  }

  if (mismatch)
    std::cerr << "\nRAW AND TYPED DISAGREE -- a from_json is reading the wrong place.\n"
                 "For usage this is case (c) in #28: a parse bug, not a per-family\n"
                 "absence. For cost see #34 -- it is a top-level sibling of usage.\n";
  return mismatch ? EXIT_FAILURE : EXIT_SUCCESS;
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
  if (argc > 1 && std::string_view{argv[1]} == "--characters")
    return list_characters(client, argc > 2 ? argv[2] : "");
  if (argc > 1 && std::string_view{argv[1]} == "--stream")
    return stream_report(client, argc > 2 ? argv[2]
                                          : "Think step by step: what is 17 * 23?");
  if (argc > 1 && std::string_view{argv[1]} == "--tools")
    return tools_report(client, argc > 2 ? argv[2] : "");
  if (argc > 1 && std::string_view{argv[1]} == "--usage")
    return usage_report(client, argc > 2 ? argv[2] : "");

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
  // What it actually cost, when the server says. Printed here and not only in
  // --usage so the field is visible without running the check leg. usd has been
  // 0 on every capture, so print whichever currency is engaged and let the
  // reader see which one answered.
  if (res->cost) {
    if (res->cost->usd || res->cost->diem) {
      std::cerr << "(cost";
      if (res->cost->usd) std::cerr << " usd " << *res->cost->usd;
      if (res->cost->diem) std::cerr << " diem " << *res->cost->diem;
      std::cerr << ")\n";
    } else {
      // A cost object with neither currency readable. Saying so is the point of
      // the tolerant parse — silence here would be the state it exists to avoid.
      std::cerr << "(cost object arrived but neither currency parsed)\n";
    }
  }
  return EXIT_SUCCESS;
}
