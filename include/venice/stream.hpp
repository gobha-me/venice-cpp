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

  // Feed a chunk of body bytes; fn is invoked once per "data:" payload, in
  // order. Returns false if the buffer cap was hit and the buffer was dropped.
  auto feed(std::string_view bytes, const std::function<void(std::string_view)>& fn) -> bool {
    m_buf.append(bytes);
    dispatch_complete(fn);
    if (m_buf.size() > kMaxEvent) {
      m_buf.clear();
      m_overflowed = true;
      return false;
    }
    return true;
  }

  // Flush whatever is left when the body ends. An unterminated final event is
  // still an event — dropping it is the defect this exists to fix — and the
  // blank-line separator is a *separator*, not a terminator.
  void finish(const std::function<void(std::string_view)>& fn) {
    if (m_buf.empty()) return;
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

  void dispatch_complete(const std::function<void(std::string_view)>& fn) {
    while (const auto found = next_event(0)) {
      const auto [len, sep] = *found;
      const std::string event = m_buf.substr(0, len);
      m_buf.erase(0, len + sep);
      emit_data_lines(event, fn);
    }
  }

  // Invoke fn for each "data:" payload in one event block. A trailing \r is
  // stripped: with CRLF endings every line inside the event carries one, and a
  // payload of `{"a":1}\r` is not parseable json.
  static void emit_data_lines(std::string_view event,
                              const std::function<void(std::string_view)>& fn) {
    std::size_t start = 0;
    while (start <= event.size()) {
      const auto nl = event.find('\n', start);
      auto line = nl == std::string_view::npos ? event.substr(start)
                                               : event.substr(start, nl - start);
      if (line.ends_with('\r')) line.remove_suffix(1);
      if (line.starts_with("data:")) {
        auto payload = line.substr(5);
        // Exactly one optional leading space, per the SSE spec. Not a trim:
        // whitespace beyond that first space belongs to the payload.
        if (!payload.empty() && payload.front() == ' ') payload.remove_prefix(1);
        fn(payload);
      }
      if (nl == std::string_view::npos) break;
      start = nl + 1;
    }
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
// One delta per SSE frame, not one per kind. Venice's final frame carries
// finish_reason and usage together, and a reasoning model can emit
// reasoning_content and content in the same chunk; a variant would force either
// fanning one frame into several callbacks or dropping a field.
struct StreamDelta {
  // The whole verbatim chunk. Never null for a delta the accumulator produced.
  const nlohmann::json* chunk{nullptr};

  std::optional<std::string_view> content{};
  std::optional<std::string_view> reasoning_content{};
  std::optional<std::string_view> role{};
  std::optional<std::string_view> finish_reason{};
  std::optional<std::string_view> refusal{};

  // Fragments exactly as received, unmerged — merging is the accumulator's job
  // and doing it here would mean every observer redoing it. Borrowed from
  // storage the caller of delta_from_chunk owns.
  std::span<const ToolCall> tool_calls{};

  const nlohmann::json* usage{nullptr};

  // True when the chunk carried nothing this struct models. Not an error: Venice
  // sends role-only openers and empty keep-alive frames.
  [[nodiscard]] auto empty() const noexcept -> bool {
    return !content && !reasoning_content && !role && !finish_reason && !refusal &&
           tool_calls.empty() && usage == nullptr;
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
// Only choices[0] is surfaced, matching every other read in this library. A
// multi-choice stream is not reachable today (`n` is not a modeled request
// field) and the whole chunk is retained regardless, so nothing is lost.
[[nodiscard]] inline auto delta_from_chunk(const nlohmann::json& chunk,
                                           std::vector<ToolCall>& frags) -> StreamDelta {
  StreamDelta d;
  d.chunk = &chunk;
  if (!chunk.is_object()) return d;

  if (const auto* u = detail::opt_object(chunk, "usage")) d.usage = u;

  const auto* choices = detail::opt_array(chunk, "choices");
  if (choices == nullptr || choices->empty()) return d;

  const auto& c0 = choices->at(0);
  if (!c0.is_object()) return d;

  d.finish_reason = detail::view_of(c0, "finish_reason");

  const auto* delta = detail::opt_object(c0, "delta");
  if (delta == nullptr) return d;

  d.content = detail::view_of(*delta, "content");
  d.reasoning_content = detail::view_of(*delta, "reasoning_content");
  d.role = detail::view_of(*delta, "role");
  d.refusal = detail::view_of(*delta, "refusal");

  if (const auto* tc = detail::opt_array(*delta, "tool_calls")) {
    const auto first = frags.size();
    for (const auto& e : *tc) frags.push_back(e.get<ToolCall>());
    d.tool_calls = std::span<const ToolCall>{frags.data() + first, frags.size() - first};
  }
  return d;
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
    if (d.chunk != nullptr && m_keep_chunks) m_chunks.push_back(*d.chunk);

    // First write wins for role: it arrives once, in the opening frame.
    if (d.role && m_role.empty()) m_role = std::string{*d.role};

    if (d.content) m_content.append(*d.content);
    if (d.reasoning_content) m_reasoning.append(*d.reasoning_content);
    if (d.refusal) m_refusal.append(*d.refusal);
    // Not `else if` and not gated on finish_reason: content and reasoning are
    // independent streams that may interleave in any order, and finish_reason
    // can arrive before the last tool-call argument fragment.
    if (d.finish_reason) m_finish = std::string{*d.finish_reason};
    if (d.usage != nullptr) m_usage = d.usage->get<Usage>();

    for (const auto& frag : d.tool_calls) merge_tool_call(frag);

    m_saw_anything = m_saw_anything || !d.empty();
  }

  // Also accepts a chunk directly, for a caller driving their own transport.
  void ingest(const nlohmann::json& chunk) {
    std::vector<ToolCall> frags;
    ingest(delta_from_chunk(chunk, frags));
  }

  // The assembled turn, ready to append to the next request's messages.
  [[nodiscard]] auto message() const -> Message {
    Message m;
    // "assistant" when the stream never said: it is the only role a completion
    // can reply with, and a roleless message is one no server accepts.
    m.role = m_role.empty() ? std::string{"assistant"} : m_role;
    // Absent rather than "" when nothing arrived: a tool-call-only turn has no
    // content, and emitting "" would differ from what the non-streamed parse
    // produces for the same reply.
    if (!m_content.empty()) m.content = m_content;
    if (!m_reasoning.empty()) m.reasoning_content = m_reasoning;
    if (!m_refusal.empty()) m.refusal = m_refusal;
    if (!m_calls.empty()) {
      std::vector<ToolCall> calls;
      calls.reserve(m_calls.size());
      // std::map iterates in key order, so emission is by index rather than by
      // arrival — fragments for call 1 can precede call 0's.
      for (const auto& [idx, call] : m_calls) calls.push_back(call);
      m.tool_calls = std::move(calls);
    }
    return m;
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
    r.finish_reason = m_finish;
    r.usage = m_usage;
    r.message = message();
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
  //
  // A fragment with no index at all is treated as index 0, which is what a
  // non-streamed reply and a single-call stream both look like.
  void merge_tool_call(const ToolCall& frag) {
    auto& slot = m_calls[frag.index.value_or(0)];
    if (slot.id.empty()) slot.id = frag.id;
    if (slot.type.empty()) slot.type = frag.type;
    if (slot.name.empty()) slot.name = frag.name;
    slot.arguments += frag.arguments;
    if (!slot.index) slot.index = frag.index;
    if (slot.raw.is_null()) slot.raw = frag.raw;
  }

  std::string m_role, m_content, m_reasoning, m_refusal, m_finish, m_id, m_model;
  std::optional<Usage> m_usage{};
  std::map<int, ToolCall> m_calls{};
  std::vector<nlohmann::json> m_chunks{};
  bool m_keep_chunks{true};
  bool m_saw_anything{false};
};

}  // namespace venice
