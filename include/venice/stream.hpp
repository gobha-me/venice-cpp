#pragma once

// venice-cpp — SSE framing, stream deltas, and assembly (VC-05, #6).
//
// Three things live here, in dependency order:
//
//   detail::SseFramer   bytes  -> "data:" payloads
//   StreamDelta         chunk  -> a view of what one chunk carried
//   StreamAccumulator   deltas -> the assembled Message / ChatResponse
//
// None of them touches a socket, and that is the point. Before this header the
// framing lived inside Client::chat_stream's content_receiver lambda and
// for_each_data_line was a private static, so #6's own acceptance criteria —
// partial frames across chunk boundaries, [DONE] — were reachable only through
// a live connection. This is the third time this repo has made that move:
// VC-03 took models_from_json_body out of Client, VC-13 put percent_encode and
// with_query at namespace scope, and the reasoning is identical each time. A
// private helper is testable only behind a socket; a free function is testable.
//
// Include order: types.hpp <- stream.hpp <- client.hpp. One direction, no cycle.

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "venice/types.hpp"

namespace venice {

namespace detail {

// ── SSE framing ───────────────────────────────────────────────────────────
//
// Server-Sent Events, reduced to what a chat completion needs: events are
// separated by a blank line, and within an event the "data:" fields are the
// payload. Everything else an SSE stream may carry (event:, id:, retry:,
// comments) is ignored rather than modeled, because Venice sends none of it and
// inventing handling for unseen input is how you get untested code.
//
// Three defects in the pre-VC-05 framing are fixed here, all of them measured
// against a verbatim copy of the old loop before anything was rewritten:
//
//   * CRLF frames never dispatched. The old split was on "\n\n", and "\r\n\r\n"
//     contains no such substring, so a spec-legal CRLF peer buffered forever and
//     delivered *nothing at all*. Measured: 1 payload for LF, 0 for CRLF, with
//     the LF case as the control proving the harness worked. Venice sends LF
//     today, so this was latent — it surfaces behind a proxy that rewrites line
//     endings, not in any test.
//   * The trailing frame was dropped. `leftover` was a local of the receiver
//     lambda and was simply discarded when send() returned, so a final event not
//     terminated by a blank line never dispatched. Measured: 1 of 2 payloads
//     delivered. That last frame is frequently the *usage* frame, so the loss is
//     a billing bug. finish() is the fix.
//   * `leftover` was unbounded. A peer that never sends a blank line buffered
//     its entire response in memory. kMaxEvent caps it.
//
// The framer keeps no callback and no state beyond the buffer, so a test can
// drive it with byte strings — test/07stream/ §7.
class SseFramer {
 public:
  // A single event may legitimately be large (a whole tool-call argument blob),
  // but not unbounded. Past this the buffer is dropped rather than grown; a
  // peer sending 8 MiB with no blank line is malfunctioning, and holding it is
  // the worse failure of the two.
  static constexpr std::size_t kMaxEvent = 8U * 1024U * 1024U;

  // Feed a chunk of body bytes; fn is invoked once per SSE event carrying one
  // or more data fields, in order. Multiple data fields are joined with '\n',
  // as required by the SSE event-stream algorithm. Returns false if the event
  // cap was hit. Overflow is terminal until reset(): once an event has been
  // dropped, later bytes cannot safely be interpreted as a new event boundary.
  auto feed(std::string_view bytes, const std::function<void(std::string_view)>& fn) -> bool {
    if (m_overflowed) return false;
    m_buf.append(bytes);
    if (!dispatch_complete(fn)) return false;
    if (m_buf.size() > kMaxEvent) {
      mark_overflowed();
      return false;
    }
    return true;
  }

  // Flush whatever is left when the body ends. An unterminated final event is
  // still an event — dropping it is the defect this exists to fix — and the
  // blank-line separator is a *separator*, not a terminator.
  void finish(const std::function<void(std::string_view)>& fn) {
    if (m_overflowed || m_buf.empty()) return;
    std::string tail;
    tail.swap(m_buf);
    emit_data_lines(tail, fn);
  }

  [[nodiscard]] auto overflowed() const noexcept -> bool { return m_overflowed; }

  void reset() {
    m_buf.clear();
    m_overflowed = false;
  }

 private:
  // Find the end of the next complete event: a blank line, in either line
  // ending. Returns {event_length, separator_length}, or nullopt.
  //
  // Both endings are searched on every pass rather than sniffing one and
  // committing, because a proxy may rewrite mid-stream and a peer may mix them.
  // The earlier match wins; on a tie CRLF wins, since "\r\n\r\n" starting at i
  // means "\n\n" is at i+1 and the CRLF reading is the one that consumes the
  // stray \r rather than leaving it to head the next event.
  [[nodiscard]] auto next_event(std::size_t from) const
      -> std::optional<std::pair<std::size_t, std::size_t>> {
    const auto lf = m_buf.find("\n\n", from);
    const auto crlf = m_buf.find("\r\n\r\n", from);
    if (crlf != std::string::npos && (lf == std::string::npos || crlf <= lf))
      return std::pair{crlf, std::size_t{4}};
    if (lf != std::string::npos) return std::pair{lf, std::size_t{2}};
    return std::nullopt;
  }

  auto dispatch_complete(const std::function<void(std::string_view)>& fn) -> bool {
    while (const auto found = next_event(0)) {
      const auto [len, sep] = *found;
      if (len > kMaxEvent) {
        mark_overflowed();
        return false;
      }
      const std::string event = m_buf.substr(0, len);
      m_buf.erase(0, len + sep);
      emit_data_lines(event, fn);
    }
    return true;
  }

  void mark_overflowed() {
    m_buf.clear();
    m_overflowed = true;
  }

  // Join every "data:" field in one event block, then invoke fn once. A
  // trailing \r is stripped: with CRLF endings every line inside the event
  // carries one, and a payload of `{"a":1}\r` is not parseable json.
  static void emit_data_lines(std::string_view event,
                              const std::function<void(std::string_view)>& fn) {
    std::string joined;
    bool saw_data = false;
    std::size_t start = 0;
    while (start <= event.size()) {
      const auto nl = event.find('\n', start);
      auto line = nl == std::string_view::npos ? event.substr(start)
                                               : event.substr(start, nl - start);
      if (line.ends_with('\r')) line.remove_suffix(1);
      if (line.starts_with("data:")) {
        auto line_payload = line.substr(5);
        // Exactly one optional leading space, per the SSE spec. Not a trim:
        // whitespace beyond that first space belongs to the payload.
        if (!line_payload.empty() && line_payload.front() == ' ')
          line_payload.remove_prefix(1);
        if (saw_data) joined.push_back('\n');
        joined.append(line_payload.data(), line_payload.size());
        saw_data = true;
      }
      if (nl == std::string_view::npos) break;
      start = nl + 1;
    }
    if (saw_data) fn(joined);
  }

  std::string m_buf;
  bool m_overflowed = false;
};

}  // namespace detail

// ── one chunk, as it arrived ──────────────────────────────────────────────
//
// A **view**, valid only for the duration of the callback that receives it. The
// string_views and pointers borrow from the parsed chunk, which the transport
// owns and destroys as soon as the callback returns. Anything worth keeping is
// already in the accumulator; that asymmetry is deliberate, and it is why the
// callback and the storage are separate objects rather than one.
//
// A struct of optionals rather than a variant, and this is the ticket's own
// stated requirement rather than a style preference: "room for tool-call deltas
// without breaking the ABI of the callback signature again". Adding an
// alternative to a variant changes the variant's type, which changes the
// callback's type and breaks every exhaustive visit. Adding a member here is
// additive and existing callers do not notice. A `kind` enum is the same trap
// one level down — VC-06 already recorded that a new ErrorKind enumerator is a
// break for an exhaustive switch.
//
// One delta per choice in an SSE frame, not one per kind. The common n==1 case
// remains one callback per frame; n>1 fans out only at the choice boundary.
// Venice's final frame carries finish_reason and usage together, and a reasoning
// model can emit reasoning_content and content in the same choice; a variant
// would force either fanning one choice into several callbacks or dropping a
// field.
struct StreamDelta {
  // The whole verbatim chunk. Never null for a delta the accumulator produced.
  const nlohmann::json* chunk{nullptr};
  // Internal envelope ownership marker: a multi-choice SSE frame fans out to
  // one StreamDelta per choice, but chunks() must retain the frame only once.
  bool first_choice_in_chunk{true};

  // The server's join key for n>1. Disengaged retains the historical
  // single-choice behavior and is treated as choice zero by the accumulator.
  std::optional<int> choice_index{};

  std::optional<std::string_view> content{};
  std::optional<std::string_view> reasoning_content{};
  const nlohmann::json* reasoning_details{nullptr};
  std::optional<std::string_view> thought_signature{};
  std::optional<std::string_view> role{};
  std::optional<std::string_view> finish_reason{};
  std::optional<std::string_view> stop_reason{};
  std::optional<std::string_view> refusal{};
  // Provider-shaped choice logprobs for this chunk. Borrowed rather than
  // accumulated: the published shape does not define a cross-frame merge rule,
  // and wrapping fragments in a made-up array would no longer match the wire.
  const nlohmann::json* logprobs{nullptr};

  // Fragments exactly as received, unmerged — merging is the accumulator's job
  // and doing it here would mean every observer redoing it. Borrowed from
  // storage the caller of delta_from_chunk owns.
  std::span<const ToolCall> tool_calls{};

  const nlohmann::json* usage{nullptr};
  // What the call charged, when the frame carries it — see ChatResponse::cost.
  // Borrowed exactly as `usage` is, for the same reason and with the same
  // lifetime: this is a view, and both die with `chunk`.
  const nlohmann::json* cost{nullptr};
  const nlohmann::json* prompt_logprobs{nullptr};

  // True when the chunk carried nothing this struct models. Not an error: Venice
  // sends role-only openers and empty keep-alive frames.
  [[nodiscard]] auto empty() const noexcept -> bool {
    return !choice_index && !content && !reasoning_content && reasoning_details == nullptr &&
           !thought_signature && !role && !finish_reason && !stop_reason && !refusal &&
           logprobs == nullptr && tool_calls.empty() && usage == nullptr && cost == nullptr &&
           prompt_logprobs == nullptr;
  }
};

namespace detail {

// Borrowed from `chunk`, so it dies with it — hence the out-parameter for the
// tool-call fragments rather than a vector inside StreamDelta.
[[nodiscard]] inline auto view_of(const nlohmann::json& j, const char* key)
    -> std::optional<std::string_view> {
  const auto it = j.find(key);
  if (it == j.end()) return std::nullopt;
  const auto* s = it->get_ptr<const nlohmann::json::string_t*>();
  if (s == nullptr) return std::nullopt;  // null or wrong-typed reads as absent
  return std::string_view{*s};
}

}  // namespace detail

// Read one SSE chunk into a delta. Total — never throws, whatever the chunk
// turns out to be. `frags` owns the tool-call fragments the returned span points
// at, so it must outlive the delta.
//
namespace detail {

[[nodiscard]] inline auto delta_from_choice(const nlohmann::json& chunk,
                                            const nlohmann::json* choice,
                                            std::vector<ToolCall>& frags,
                                            bool include_envelope) -> StreamDelta {
  StreamDelta d;
  d.chunk = &chunk;
  d.first_choice_in_chunk = include_envelope;
  if (!chunk.is_object()) return d;

  if (include_envelope) {
    if (const auto* u = opt_object(chunk, "usage")) d.usage = u;
    if (const auto* c = opt_object(chunk, "cost")) d.cost = c;
    if (const auto it = chunk.find("prompt_logprobs"); it != chunk.end())
      d.prompt_logprobs = &*it;
  }
  if (choice == nullptr || !choice->is_object()) return d;

  d.choice_index = opt_int(*choice, "index");
  d.finish_reason = view_of(*choice, "finish_reason");
  d.stop_reason = view_of(*choice, "stop_reason");
  if (const auto it = choice->find("logprobs"); it != choice->end()) d.logprobs = &*it;

  const auto* delta = opt_object(*choice, "delta");
  if (delta == nullptr) return d;
  d.content = view_of(*delta, "content");
  d.reasoning_content = view_of(*delta, "reasoning_content");
  if (const auto* details = opt_array(*delta, "reasoning_details"))
    d.reasoning_details = details;
  d.thought_signature = view_of(*delta, "thought_signature");
  d.role = view_of(*delta, "role");
  d.refusal = view_of(*delta, "refusal");

  if (const auto* tc = opt_array(*delta, "tool_calls")) {
    const auto first = frags.size();
    for (const auto& e : *tc) frags.push_back(e.get<ToolCall>());
    d.tool_calls = std::span<const ToolCall>{frags.data() + first, frags.size() - first};
  }
  return d;
}

}  // namespace detail

// Compatibility helper: returns the first choice, as it always has. The
// transport and StreamAccumulator's chunk overload use
// for_each_delta_from_chunk below to retain every choice.
[[nodiscard]] inline auto delta_from_chunk(const nlohmann::json& chunk,
                                           std::vector<ToolCall>& frags) -> StreamDelta {
  const auto* choices = detail::opt_array(chunk, "choices");
  const auto* first = choices != nullptr && !choices->empty() ? &choices->front() : nullptr;
  return detail::delta_from_choice(chunk, first, frags, /*include_envelope=*/true);
}

template <typename Fn>
inline void for_each_delta_from_chunk(const nlohmann::json& chunk, Fn&& fn) {
  const auto* choices = detail::opt_array(chunk, "choices");
  if (choices == nullptr || choices->empty()) {
    std::vector<ToolCall> frags;
    fn(detail::delta_from_choice(chunk, nullptr, frags, /*include_envelope=*/true));
    return;
  }
  bool first = true;
  for (const auto& choice : *choices) {
    std::vector<ToolCall> frags;
    fn(detail::delta_from_choice(chunk, &choice, frags, first));
    first = false;
  }
}

// ── assembly ──────────────────────────────────────────────────────────────
//
// The "store and fetch on stream" half of #6. It is a caller-owned object rather
// than a hidden local inside Client::chat_stream, and that is the design's
// central promise made structural:
//
//   * A **cancelled** stream returns ErrorKind::Cancelled and no response —
//     VC-06 settled that, and it is right, because a caller who has stopped
//     waiting should not have to tell "the text so far" from "the answer". But
//     with the storage in the caller's hands, cancelling no longer *destroys*
//     what arrived. The return value says "you abandoned this"; the accumulator
//     still holds every token, every thought, and every chunk.
//   * The rich delta stream is unavailable without one, so there is no way to
//     ask for deltas and then lose them.
//
// Threading: ingest runs on the thread inside chat_stream, since httplib invokes
// the content receiver from send(). Do not read an accumulator from another
// thread while a call is in flight; the cancel watcher never touches it.
class StreamAccumulator {
 public:
  StreamAccumulator() = default;

  // Retention of the verbatim chunks is on by default — it is the literal
  // reading of "nothing received is discarded", and a long reply is well under
  // a megabyte. Turn it off for an unbounded or memory-tight stream; everything
  // modeled is still assembled either way.
  explicit StreamAccumulator(bool keep_chunks) : m_keep_chunks(keep_chunks) {}

  // Deliberately NOT operator(). A call operator would make this convertible to
  // std::function<bool(std::string_view)> and silently reintroduce the overload
  // ambiguity the three-overload set is shaped to avoid — test/07stream/ pins
  // the non-convertibility with a static_assert so this cannot regress.
  void ingest(const StreamDelta& d) {
    if (d.chunk != nullptr && d.first_choice_in_chunk && m_keep_chunks)
      m_chunks.push_back(*d.chunk);

    const bool has_choice_data = d.choice_index || d.role || d.content || d.reasoning_content ||
                                 d.reasoning_details != nullptr || d.thought_signature ||
                                 d.refusal || d.finish_reason || d.stop_reason ||
                                 !d.tool_calls.empty();
    if (has_choice_data) {
      auto& choice = m_choices[d.choice_index.value_or(0)];
      // First write wins for role/signature; text-bearing fields append because
      // SSE fragments are not complete values on their own.
      if (d.role && choice.role.empty()) choice.role = std::string{*d.role};
      if (d.content) choice.content.append(*d.content);
      if (d.reasoning_content) choice.reasoning.append(*d.reasoning_content);
      if (d.reasoning_details != nullptr)
        for (const auto& item : *d.reasoning_details) choice.reasoning_details.push_back(item);
      if (d.thought_signature && choice.thought_signature.empty())
        choice.thought_signature = std::string{*d.thought_signature};
      if (d.refusal) choice.refusal.append(*d.refusal);
      if (d.finish_reason) choice.finish = std::string{*d.finish_reason};
      if (d.stop_reason) choice.stop_reason = std::string{*d.stop_reason};
      for (const auto& frag : d.tool_calls) merge_tool_call(choice, frag);
    }
    // Cost BEFORE usage, and the order is load bearing. Cost rides on the usage
    // frame — one object, both keys — and `get<Usage>()` below is loud, so a
    // wrong-typed token count throws out of this function. chat_stream turns
    // that into a terminal Parse result while leaving accepted state in the
    // caller-owned accumulator. Read the other way round, a corrupt `usage`
    // would take the billing figure with it, which is exactly what
    // ChatResponse::cost's tolerant parse exists to prevent, undone one file
    // over. test/07stream/ §9 pins it.
    //
    // Last wins, mirroring usage. Every stream swept for VC-20 carried exactly
    // one cost frame, so no capture discriminates last-wins from first-wins or
    // from summing; `venice-cpp --usage` fails the run if a second cost frame
    // ever arrives, which is what keeps this rule falsifiable rather than merely
    // asserted. This line cannot throw — both members go through
    // detail::opt_double, a predicate — so nothing after it is at risk from it.
    if (d.cost != nullptr) m_cost = d.cost->get<Price>();
    if (d.usage != nullptr) m_usage = d.usage->get<Usage>();
    if (d.prompt_logprobs != nullptr) m_prompt_logprobs = *d.prompt_logprobs;

    m_saw_anything = m_saw_anything || !d.empty();
  }

  // Also accepts a chunk directly, for a caller driving their own transport.
  // Takes the envelope too, so this overload is complete on its own — the
  // delta view deliberately does not model id/model, and a caller who only had
  // this entry point would otherwise silently lose them.
  void ingest(const nlohmann::json& chunk) {
    note_envelope(chunk);
    for_each_delta_from_chunk(chunk, [&](const StreamDelta& delta) { ingest(delta); });
  }

  // The assembled turn, ready to append to the next request's messages.
  [[nodiscard]] auto message() const -> Message {
    if (!m_choices.empty()) return message_from(m_choices.begin()->second);
    Message message;
    message.role = "assistant";
    return message;
  }

  // The same shape Client::chat returns for the same reply. `raw` is
  // deliberately NOT the concatenation of chunks — it is a synthesised body, and
  // a streamed response can never be byte-equal to a non-streamed one
  // ("chat.completion.chunk" vs "chat.completion"). chunks() is where the
  // verbatim record lives.
  [[nodiscard]] auto response() const -> ChatResponse {
    ChatResponse r;
    r.id = m_id;
    r.model = m_model;
    r.usage = m_usage;
    r.cost = m_cost;
    r.prompt_logprobs = m_prompt_logprobs;
    for (const auto& [index, state] : m_choices) {
      ChatChoice choice;
      choice.index = index;
      choice.message = message_from(state);
      if (!state.finish.empty()) choice.finish_reason = state.finish;
      if (!state.stop_reason.empty()) choice.stop_reason = state.stop_reason;
      r.choices.push_back(std::move(choice));
    }
    if (!r.choices.empty()) {
      r.message = r.choices.front().message;
      if (r.choices.front().finish_reason)
        r.finish_reason = *r.choices.front().finish_reason;
    } else {
      // Preserve the historical accumulator behavior for an envelope-only
      // partial result.
      r.message = message();
    }
    r.content = r.message->text();
    return r;
  }

  [[nodiscard]] auto chunks() const noexcept -> const std::vector<nlohmann::json>& {
    return m_chunks;
  }

  // Whether any modeled field arrived at all. chat_stream uses this to decide
  // whether a parse error was fatal: before VC-05 that test was
  // "content is empty", which reported ErrorKind::Parse on a reasoning-only
  // stream that had in fact arrived perfectly.
  [[nodiscard]] auto empty() const noexcept -> bool { return !m_saw_anything; }

  void reset() {
    *this = StreamAccumulator{m_keep_chunks};
  }

  // Set from the chunk envelope by chat_stream, which sees fields the delta
  // view does not model.
  void note_envelope(const nlohmann::json& chunk) {
    if (const auto* s = detail::opt_string_at(chunk, "id")) m_id = *s;
    if (const auto* s = detail::opt_string_at(chunk, "model")) m_model = *s;
  }

 private:
  // The merge that cannot be deferred to VC-08, and every rule in it is a
  // failure mode rather than a preference:
  //
  //   * The join key is the fragment's own `index`, never its position in the
  //     array. A chunk carrying only the second call sends a one-element array
  //     with "index": 1, so position-merging concatenates two calls' arguments
  //     into one and produces something that looks plausible.
  //   * std::map, not vector[index]: indices are neither guaranteed contiguous
  //     nor monotonic, and emission must be by index rather than arrival.
  //   * First *non-empty* write wins for the scalars. id, type and name arrive
  //     in the opening fragment only, and some gateways send "name": "" on
  //     continuations — an unguarded assignment clobbers the id on fragment two.
  //   * arguments is appended verbatim, never trimmed. Whitespace inside a JSON
  //     string literal is significant, and a fragment is not valid JSON on its
  //     own so there is nothing to validate against.
  //   * thought_signature obeys the same first-non-empty rule, and it *must*:
  //     unlike id/type/name it is not tied to the opening fragment. Measured
  //     2026-08-09 — gemini-3-6-flash sends the whole call in one fragment with
  //     the signature attached, gemini-3-5-flash splits the same call across
  //     two. So it cannot be read off `raw` below, which is fragment one only.
  //
  // A fragment with no index at all is treated as index 0, which is what a
  // non-streamed reply and a single-call stream both look like.
  //
  // `raw` here is the FIRST fragment, not a record of the call: on a
  // multi-fragment call there is no single verbatim server object to hold, and
  // its `function.arguments` is only the opening chunk. `chunks()` is where the
  // complete record lives. Overlaying the fragments into one object would be a
  // synthesised value wearing a "verbatim" label, which is the same thing
  // response() refuses to do for ChatResponse::raw above.
  struct ChoiceState {
    std::string role{};
    std::string content{};
    std::string reasoning{};
    std::vector<nlohmann::json> reasoning_details{};
    std::string thought_signature{};
    std::string refusal{};
    std::string finish{};
    std::string stop_reason{};
    std::map<int, ToolCall> calls{};
  };

  [[nodiscard]] static auto message_from(const ChoiceState& state) -> Message {
    Message message;
    message.role = state.role.empty() ? std::string{"assistant"} : state.role;
    if (!state.content.empty()) message.content = state.content;
    if (!state.reasoning.empty()) message.reasoning_content = state.reasoning;
    if (!state.reasoning_details.empty()) message.reasoning_details = state.reasoning_details;
    if (!state.thought_signature.empty()) message.thought_signature = state.thought_signature;
    if (!state.refusal.empty()) message.refusal = state.refusal;
    if (!state.calls.empty()) {
      std::vector<ToolCall> calls;
      calls.reserve(state.calls.size());
      for (const auto& entry : state.calls) calls.push_back(entry.second);
      message.tool_calls = std::move(calls);
    }
    return message;
  }

  static void merge_tool_call(ChoiceState& choice, const ToolCall& frag) {
    auto& slot = choice.calls[frag.index.value_or(0)];
    if (slot.id.empty()) slot.id = frag.id;
    if (slot.type.empty()) slot.type = frag.type;
    if (slot.name.empty()) slot.name = frag.name;
    slot.arguments += frag.arguments;
    if (!slot.index) slot.index = frag.index;
    // The empty-signature half of this guard is analogical, not measured: no
    // capture has shown a blank signature on a continuation. It mirrors the
    // `"name": ""` behaviour documented three bullets up, on this same object
    // with this same lifecycle — and an empty signature is not a signature,
    // since emitting "" is the same 400 as emitting none with a lie attached.
    if ((!slot.thought_signature || slot.thought_signature->empty()) &&
        frag.thought_signature && !frag.thought_signature->empty())
      slot.thought_signature = frag.thought_signature;
    if (slot.raw.is_null()) slot.raw = frag.raw;
  }

  std::string m_id, m_model;
  std::optional<Usage> m_usage{};
  std::optional<Price> m_cost{};
  std::optional<nlohmann::json> m_prompt_logprobs{};
  std::map<int, ChoiceState> m_choices{};
  std::vector<nlohmann::json> m_chunks{};
  bool m_keep_chunks{true};
  bool m_saw_anything{false};
};

}  // namespace venice
