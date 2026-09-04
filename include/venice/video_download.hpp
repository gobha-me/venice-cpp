#pragma once

// Internal implementation for Client::download_video. Consumers should use
// <venice/venice.hpp> and the public Client method.

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#endif

#include <ares.h>
#include <httplib.h>

#include "venice/error.hpp"
#include "venice/options.hpp"
#include "venice/types.hpp"

namespace venice {

// Process-wide lifetime ownership for c-ares. The first owner must be
// initialized on the application's startup thread before the application
// creates any thread, not merely before it creates video-download workers. Keep
// at least one owner alive until every application-created thread has joined,
// then destroy the final owner on the startup thread. Every c-ares consumer in
// the process must participate in this same VideoDownloadRuntime ownership
// coordinator and lifetime; none may perform an independent
// ares_library_init/ares_library_cleanup pair.
//
// Multiple owners are reference-counted within that one process-wide lifetime,
// and final destruction drains active resolver calls before cleanup. The
// library can fail closed when no owner exists, but it cannot discover
// arbitrary application threads: violating the startup/shutdown ordering is an
// unsupported caller error, not a condition initialize() can diagnose.
class VideoDownloadRuntime final {
public:
  [[nodiscard]] static auto initialize()
      -> std::expected<VideoDownloadRuntime, Error>;

  ~VideoDownloadRuntime() noexcept;
  VideoDownloadRuntime(const VideoDownloadRuntime &) = delete;
  auto operator=(const VideoDownloadRuntime &)
      -> VideoDownloadRuntime & = delete;
  VideoDownloadRuntime(VideoDownloadRuntime &&other) noexcept
      : m_owner{std::exchange(other.m_owner, false)} {}
  auto operator=(VideoDownloadRuntime &&) -> VideoDownloadRuntime & = delete;

private:
  struct ActiveTag {};
  explicit VideoDownloadRuntime(ActiveTag) noexcept : m_owner{true} {}

  bool m_owner{};
};

} // namespace venice

namespace venice::detail {

inline constexpr std::size_t kMaximumVideoDownloadUrlBytes = 8192;
inline constexpr std::size_t kMaximumVideoDownloadRedirects = 5;

struct VideoDownloadUrl {
  std::string host{};
  std::uint16_t port{443};
  std::string target{"/"};

  [[nodiscard]] auto origin() const -> std::string {
    std::string value{"https://"};
    const bool ipv6 = host.find(':') != std::string::npos;
    if (ipv6)
      value.push_back('[');
    value += host;
    if (ipv6)
      value.push_back(']');
    if (port != 443)
      value += ':' + std::to_string(port);
    return value;
  }

  [[nodiscard]] auto canonical() const -> std::string {
    auto value = origin();
    value += target;
    return value;
  }
};

struct VideoDownloadResponse {
  int status{};
  std::string content_type{};
  std::string body{};
  std::optional<std::string> location{};
};

class VideoDownloadBodyLimit final {
public:
  explicit VideoDownloadBodyLimit(std::size_t maximum) : m_maximum{maximum} {}

  [[nodiscard]] auto accept(std::size_t bytes) noexcept -> bool {
    if (bytes > m_maximum - m_received) {
      m_exceeded = true;
      return false;
    }
    m_received += bytes;
    return true;
  }

  [[nodiscard]] auto accept_declared(std::string_view value) noexcept -> bool {
    if (value.empty())
      return false;
    std::size_t parsed{};
    const auto [end, status] =
        std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (status == std::errc::result_out_of_range) {
      m_exceeded = true;
      return false;
    }
    if (status != std::errc{} || end != value.data() + value.size())
      return false;
    if (parsed <= m_maximum)
      return true;
    m_exceeded = true;
    return false;
  }

  [[nodiscard]] auto exceeded() const noexcept -> bool { return m_exceeded; }

private:
  std::size_t m_maximum{};
  std::size_t m_received{};
  bool m_exceeded{};
};

[[nodiscard]] inline auto
normalize_video_download_http_token(std::string_view value) -> std::string {
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.front())))
    value.remove_prefix(1);
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.back())))
    value.remove_suffix(1);
  std::string normalized;
  normalized.reserve(value.size());
  for (const char value_byte : value) {
    const auto byte = static_cast<unsigned char>(value_byte);
    normalized.push_back(
        byte >= 'A' && byte <= 'Z'
            ? static_cast<char>(byte + static_cast<unsigned char>('a' - 'A'))
            : value_byte);
  }
  return normalized;
}

[[nodiscard]] inline auto
normalize_video_download_media_type(std::string_view value) -> std::string {
  return normalize_video_download_http_token(value.substr(0, value.find(';')));
}

[[nodiscard]] inline auto
video_download_headers_within_limit(VideoDownloadBodyLimit &limit,
                                    const httplib::Response &response) -> bool {
  const auto transfer_encoding_count =
      response.get_header_value_count("Transfer-Encoding");
  if (transfer_encoding_count != 0)
    return transfer_encoding_count == 1 &&
           normalize_video_download_http_token(
               response.get_header_value("Transfer-Encoding")) == "chunked";
  if (response.has_header("Content-Encoding"))
    return true;
  const auto content_length_count =
      response.get_header_value_count("Content-Length");
  if (content_length_count == 0)
    return true;
  if (content_length_count != 1)
    return false;
  return limit.accept_declared(response.get_header_value("Content-Length"));
}

[[nodiscard]] inline auto
video_download_content_encoding_supported(const httplib::Response &response)
    -> bool {
  const auto count = response.get_header_value_count("Content-Encoding");
  if (count == 0)
    return true;
  if (count != 1)
    return false;
  const auto raw_encoding = response.get_header_value("Content-Encoding");
  const auto encoding = normalize_video_download_http_token(raw_encoding);
  if (encoding == "identity")
    return true;
#ifdef CPPHTTPLIB_ZLIB_SUPPORT
  if (raw_encoding == "gzip" || raw_encoding == "deflate")
    return true;
#endif
#ifdef CPPHTTPLIB_BROTLI_SUPPORT
  if (raw_encoding == "br")
    return true;
#endif
  return false;
}

[[nodiscard]] inline auto video_download_error(ErrorKind kind,
                                               std::string message,
                                               int status = 0) -> Error {
  return Error{kind, status, std::move(message), {}, {}};
}

[[nodiscard]] inline auto video_download_cancelled() -> Error {
  return video_download_error(ErrorKind::Cancelled,
                              "video download cancelled by caller");
}

[[nodiscard]] inline auto video_download_timed_out() -> Error {
  return video_download_error(ErrorKind::Network,
                              "video download deadline exceeded");
}

[[nodiscard]] inline auto ascii_lower(char value) noexcept -> char {
  const auto byte = static_cast<unsigned char>(value);
  if (byte >= 'A' && byte <= 'Z')
    return static_cast<char>(byte + ('a' - 'A'));
  return value;
}

[[nodiscard]] inline auto ascii_iequal(std::string_view lhs,
                                       std::string_view rhs) noexcept -> bool {
  if (lhs.size() != rhs.size())
    return false;
  for (std::size_t i = 0; i < lhs.size(); ++i)
    if (ascii_lower(lhs[i]) != ascii_lower(rhs[i]))
      return false;
  return true;
}

[[nodiscard]] inline auto valid_url_bytes(std::string_view value) noexcept
    -> bool {
  if (value.empty() || value.size() > kMaximumVideoDownloadUrlBytes)
    return false;
  for (const unsigned char byte : value)
    if (byte <= 0x20 || byte == 0x7f || byte >= 0x80 || byte == '\\')
      return false;
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (value[i] != '%')
      continue;
    const auto hex = [](const unsigned char byte) {
      return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f') ||
             (byte >= 'A' && byte <= 'F');
    };
    if (i + 2 >= value.size() ||
        !hex(static_cast<unsigned char>(value[i + 1])) ||
        !hex(static_cast<unsigned char>(value[i + 2])))
      return false;
    i += 2;
  }
  return true;
}

[[nodiscard]] inline auto valid_dns_name(std::string_view host) noexcept
    -> bool {
  if (host.empty() || host.size() > 253 || host.front() == '.' ||
      host.back() == '.')
    return false;
  std::size_t label = 0;
  bool label_starts_with_hyphen = false;
  char previous{};
  for (const unsigned char byte : host) {
    if (byte == '.') {
      if (label == 0 || label > 63 || label_starts_with_hyphen ||
          previous == '-')
        return false;
      label = 0;
      label_starts_with_hyphen = false;
      previous = '.';
      continue;
    }
    const bool allowed = (byte >= 'a' && byte <= 'z') ||
                         (byte >= 'A' && byte <= 'Z') ||
                         (byte >= '0' && byte <= '9') || byte == '-';
    if (!allowed)
      return false;
    if (label == 0)
      label_starts_with_hyphen = byte == '-';
    ++label;
    previous = static_cast<char>(byte);
  }
  return label != 0 && label <= 63 && !label_starts_with_hyphen &&
         previous != '-';
}

template <std::size_t Size>
[[nodiscard]] inline auto
parse_video_download_address(int family, std::string_view text,
                             std::array<unsigned char, Size> &output) -> bool {
  const std::string owned{text};
#ifdef _WIN32
  return InetPtonA(family, owned.c_str(), output.data()) == 1;
#else
  return ::inet_pton(family, owned.c_str(), output.data()) == 1;
#endif
}

[[nodiscard]] inline auto parse_video_download_url(std::string_view value)
    -> std::expected<VideoDownloadUrl, Error> {
  const auto invalid = [] {
    return std::unexpected{video_download_error(ErrorKind::InvalidArg,
                                                "invalid video download URL")};
  };
  if (!valid_url_bytes(value) || value.find('#') != std::string_view::npos)
    return invalid();

  constexpr std::string_view kScheme{"https://"};
  if (value.size() < kScheme.size() ||
      !ascii_iequal(value.substr(0, kScheme.size()), kScheme))
    return invalid();
  value.remove_prefix(kScheme.size());

  const auto authority_end = value.find_first_of("/?");
  const auto authority = value.substr(0, authority_end);
  if (authority.empty() || authority.find('@') != std::string_view::npos ||
      authority.find('%') != std::string_view::npos)
    return invalid();

  std::string_view host;
  std::string_view port_text;
  if (authority.front() == '[') {
    const auto close = authority.find(']');
    if (close == std::string_view::npos || close == 1)
      return invalid();
    host = authority.substr(1, close - 1);
    const auto suffix = authority.substr(close + 1);
    if (!suffix.empty()) {
      if (suffix.front() != ':' || suffix.size() == 1)
        return invalid();
      port_text = suffix.substr(1);
    }
    std::array<unsigned char, 16> address{};
    if (!parse_video_download_address(AF_INET6, host, address))
      return invalid();
  } else {
    const auto colon = authority.rfind(':');
    if (colon != std::string_view::npos) {
      if (authority.find(':') != colon || colon == 0 ||
          colon + 1 == authority.size())
        return invalid();
      host = authority.substr(0, colon);
      port_text = authority.substr(colon + 1);
    } else {
      host = authority;
    }
    std::array<unsigned char, 4> address{};
    if (!parse_video_download_address(AF_INET, host, address) &&
        !valid_dns_name(host))
      return invalid();
  }

  unsigned int port = 443;
  if (!port_text.empty()) {
    const auto *begin = port_text.data();
    const auto *end = begin + port_text.size();
    const auto [parsed_end, result] = std::from_chars(begin, end, port);
    if (result != std::errc{} || parsed_end != end || port == 0 ||
        port > std::numeric_limits<std::uint16_t>::max())
      return invalid();
  }

  std::string normalized_host;
  normalized_host.reserve(host.size());
  for (const char byte : host)
    normalized_host.push_back(ascii_lower(byte));

  std::string target;
  if (authority_end == std::string_view::npos) {
    target = "/";
  } else {
    target = std::string{value.substr(authority_end)};
    if (target.front() == '?')
      target.insert(target.begin(), '/');
  }
  return VideoDownloadUrl{std::move(normalized_host),
                          static_cast<std::uint16_t>(port), std::move(target)};
}

[[nodiscard]] inline auto remove_dot_segments(std::string_view target)
    -> std::string {
  const auto query = target.find('?');
  std::string input{target.substr(0, query)};
  std::string normalized;
  const auto remove_last = [&] {
    if (normalized.empty())
      return;
    const auto slash = normalized.rfind('/');
    normalized.erase(slash == std::string::npos ? 0 : slash);
  };
  while (!input.empty()) {
    if (input.starts_with("../")) {
      input.erase(0, 3);
    } else if (input.starts_with("./")) {
      input.erase(0, 2);
    } else if (input.starts_with("/./")) {
      input.erase(0, 2);
    } else if (input == "/.") {
      input = "/";
    } else if (input.starts_with("/../")) {
      input.erase(0, 3);
      remove_last();
    } else if (input == "/..") {
      input = "/";
      remove_last();
    } else if (input == "." || input == "..") {
      input.clear();
    } else {
      const auto next =
          input.front() == '/' ? input.find('/', 1) : input.find('/');
      const auto length = next == std::string::npos ? input.size() : next;
      normalized.append(input, 0, length);
      input.erase(0, length);
    }
  }
  if (normalized.empty())
    normalized = "/";
  if (query != std::string_view::npos)
    normalized += target.substr(query);
  return normalized;
}

[[nodiscard]] inline auto
resolve_video_download_redirect(const VideoDownloadUrl &current,
                                std::string_view location)
    -> std::expected<VideoDownloadUrl, Error> {
  if (!valid_url_bytes(location) ||
      location.find('#') != std::string_view::npos)
    return std::unexpected{video_download_error(
        ErrorKind::InvalidArg, "invalid video download redirect")};

  if (location.starts_with("//") ||
      location.find("://") != std::string_view::npos) {
    auto absolute = location.starts_with("//")
                        ? parse_video_download_url(std::string{"https:"} +
                                                   std::string{location})
                        : parse_video_download_url(location);
    if (absolute)
      absolute->target = remove_dot_segments(absolute->target);
    return absolute;
  }
  const auto first_separator = location.find_first_of("/?");
  const auto colon = location.find(':');
  if (colon != std::string_view::npos &&
      (first_separator == std::string_view::npos || colon < first_separator))
    return std::unexpected{video_download_error(
        ErrorKind::InvalidArg, "invalid video download redirect")};

  auto next = current;
  if (location.front() == '/') {
    next.target = remove_dot_segments(location);
  } else if (location.front() == '?') {
    const auto query = next.target.find('?');
    if (query != std::string::npos)
      next.target.erase(query);
    next.target += location;
  } else {
    const auto query = next.target.find('?');
    if (query != std::string::npos)
      next.target.erase(query);
    const auto slash = next.target.rfind('/');
    next.target.erase(slash == std::string::npos ? 0 : slash + 1);
    next.target += location;
    next.target = remove_dot_segments(next.target);
  }
  return next;
}

[[nodiscard]] inline auto
ipv4_is_public(const std::array<unsigned char, 4> &bytes) noexcept -> bool {
  if (bytes[0] == 0 || bytes[0] == 10 || bytes[0] == 127 || bytes[0] >= 224)
    return false;
  if ((bytes[0] == 100 && (bytes[1] & 0xc0U) == 64) ||
      (bytes[0] == 169 && bytes[1] == 254) ||
      (bytes[0] == 172 && (bytes[1] & 0xf0U) == 16) ||
      (bytes[0] == 192 && bytes[1] == 0 && (bytes[2] == 0 || bytes[2] == 2)) ||
      (bytes[0] == 192 && bytes[1] == 88 && bytes[2] == 99) ||
      (bytes[0] == 192 && bytes[1] == 168) ||
      (bytes[0] == 198 && (bytes[1] == 18 || bytes[1] == 19)) ||
      (bytes[0] == 198 && bytes[1] == 51 && bytes[2] == 100) ||
      (bytes[0] == 203 && bytes[1] == 0 && bytes[2] == 113))
    return false;
  return true;
}

[[nodiscard]] inline auto
video_download_address_is_public(std::string_view address) -> bool {
  std::array<unsigned char, 4> ipv4{};
  if (parse_video_download_address(AF_INET, address, ipv4))
    return ipv4_is_public(ipv4);

  std::array<unsigned char, 16> ipv6{};
  if (!parse_video_download_address(AF_INET6, address, ipv6))
    return false;
  const auto all_zero = [](auto begin, auto end) {
    return std::all_of(begin, end,
                       [](unsigned char value) { return value == 0; });
  };
  if (all_zero(ipv6.begin(), ipv6.begin() + 10) && ipv6[10] == 0xff &&
      ipv6[11] == 0xff) {
    const std::array<unsigned char, 4> mapped{ipv6[12], ipv6[13], ipv6[14],
                                              ipv6[15]};
    return ipv4_is_public(mapped);
  }
  if (all_zero(ipv6.begin(), ipv6.begin() + 12))
    return false;

  // Fail closed against IANA's IPv6 Global Unicast Address Assignments
  // registry (2025-10-10), rather than assuming all of 2000::/3 is public.
  // Unlisted space in that architectural range is reserved for future IANA
  // allocation; that includes the returned 6bone block 3ffe::/16. When IANA
  // allocates another range this allowlist and its boundary matrix must move
  // together.
  const auto first = static_cast<std::uint16_t>(
      static_cast<std::uint16_t>(ipv6[0]) << 8U | ipv6[1]);
  const auto second = static_cast<std::uint16_t>(
      static_cast<std::uint16_t>(ipv6[2]) << 8U | ipv6[3]);
  if (first == 0x2001U) {
    const bool allocated = (second >= 0x0200U && second <= 0x0fffU) ||
                           (second >= 0x1200U && second <= 0x3fffU) ||
                           (second >= 0x4000U && second <= 0x4dffU) ||
                           (second >= 0x5000U && second <= 0x5fffU) ||
                           (second >= 0x8000U && second <= 0xbfffU);
    return allocated && second != 0x0db8U; // documentation
  }
  if (first == 0x2003U)
    return second <= 0x3fffU; // 2003::/18
  if (first >= 0x2400U && first <= 0x241fU)
    return true; // 2400::/12 and 2410::/12
  if (first >= 0x2600U && first <= 0x260fU)
    return true; // 2600::/12
  if ((first == 0x2610U || first == 0x2620U) && second <= 0x01ffU)
    return true; // 2610::/23 and 2620::/23
  if (first >= 0x2630U && first <= 0x263fU)
    return true; // 2630::/12
  if (first >= 0x2800U && first <= 0x280fU)
    return true; // 2800::/12
  if (first >= 0x2a00U && first <= 0x2a1fU)
    return true;                               // 2a00::/12 and 2a10::/12
  return first >= 0x2c00U && first <= 0x2c0fU; // 2c00::/12
}

[[nodiscard]] inline auto
validate_video_download_addresses(const std::vector<std::string> &addresses)
    -> std::expected<void, Error> {
  if (addresses.empty())
    return std::unexpected{video_download_error(
        ErrorKind::Network, "video download DNS resolution failed")};
  for (const auto &address : addresses)
    if (!video_download_address_is_public(address))
      return std::unexpected{video_download_error(
          ErrorKind::InvalidArg, "video download destination is not public")};
  return {};
}

struct CaresSocketState {
  struct Socket {
    ares_socket_t descriptor{ARES_SOCKET_BAD};
    bool readable{};
    bool writable{};
  };
  std::vector<Socket> sockets{};
  bool allocation_failure{};
};

inline void cares_socket_state(void *data, ares_socket_t descriptor,
                               int readable, int writable) noexcept {
  auto &state = *static_cast<CaresSocketState *>(data);
  try {
    const auto found = std::find_if(
        state.sockets.begin(), state.sockets.end(),
        [&](const auto &socket) { return socket.descriptor == descriptor; });
    if (readable == 0 && writable == 0) {
      if (found != state.sockets.end())
        state.sockets.erase(found);
      return;
    }
    if (found != state.sockets.end()) {
      found->readable = readable != 0;
      found->writable = writable != 0;
      return;
    }
    state.sockets.push_back({descriptor, readable != 0, writable != 0});
  } catch (...) {
    state.allocation_failure = true;
  }
}

struct CaresResult {
  bool complete{};
  int status{ARES_EDESTRUCTION};
  std::vector<std::string> addresses{};
};

inline void cares_address_result(void *data, int status, int,
                                 ares_addrinfo *result) noexcept {
  auto &output = *static_cast<CaresResult *>(data);
  output.status = status;
  try {
    if (status == ARES_SUCCESS && result != nullptr) {
      for (auto *node = result->nodes; node != nullptr; node = node->ai_next) {
        // 45 characters plus NUL is the maximum IPv6 presentation form.
        std::array<char, 46> text{};
        const void *source = nullptr;
        if (node->ai_addr == nullptr)
          continue;
        if (node->ai_family == AF_INET) {
          source =
              &reinterpret_cast<const sockaddr_in *>(node->ai_addr)->sin_addr;
        } else if (node->ai_family == AF_INET6) {
          source =
              &reinterpret_cast<const sockaddr_in6 *>(node->ai_addr)->sin6_addr;
        }
        if (source != nullptr &&
            ares_inet_ntop(node->ai_family, source, text.data(),
                           static_cast<ares_socklen_t>(text.size())) != nullptr)
          output.addresses.emplace_back(text.data());
      }
    }
  } catch (...) {
    output.status = ARES_ENOMEM;
    output.addresses.clear();
  }
  if (result != nullptr)
    ares_freeaddrinfo(result);
  output.complete = true;
}

class CaresChannel final {
public:
  CaresChannel() = default;
  ~CaresChannel() {
    if (m_channel != nullptr)
      ares_destroy(m_channel);
  }
  CaresChannel(const CaresChannel &) = delete;
  auto operator=(const CaresChannel &) -> CaresChannel & = delete;

  [[nodiscard]] auto pointer() noexcept -> ares_channel_t ** {
    return &m_channel;
  }
  [[nodiscard]] auto get() const noexcept -> ares_channel_t * {
    return m_channel;
  }

private:
  ares_channel_t *m_channel{};
};

class CaresRuntimeCoordinator final {
public:
  template <typename Initialize>
  [[nodiscard]] auto initialize(Initialize initialize_library) -> bool {
    const std::lock_guard lock{m_mutex};
    if (m_stopping || m_owners == std::numeric_limits<std::size_t>::max())
      return false;
    if (m_owners == 0) {
      if (initialize_library() != ARES_SUCCESS)
        return false;
      m_initialized = true;
    }
    ++m_owners;
    return true;
  }

  template <typename Cleanup> void release(Cleanup cleanup_library) noexcept {
    try {
      std::unique_lock lock{m_mutex};
      if (m_owners == 0)
        return;
      --m_owners;
      if (m_owners != 0)
        return;
      m_stopping = true;
      m_condition.wait(lock, [this] { return m_users == 0; });
      if (m_initialized)
        cleanup_library();
      m_initialized = false;
      m_stopping = false;
      lock.unlock();
      m_condition.notify_all();
    } catch (...) {
      // Cleanup racing live use would be unsafe. On an exceptional mutex or
      // condition-variable failure, leak the c-ares process reference and keep
      // future acquisitions closed instead of risking use-after-cleanup.
    }
  }

  [[nodiscard]] auto acquire() -> bool {
    const std::lock_guard lock{m_mutex};
    if (!m_initialized || m_stopping ||
        m_users == std::numeric_limits<std::size_t>::max())
      return false;
    ++m_users;
    return true;
  }

  void release_use() noexcept {
    try {
      {
        const std::lock_guard lock{m_mutex};
        if (m_users == 0)
          return;
        --m_users;
      }
      m_condition.notify_all();
    } catch (...) {
      // As above, retaining the process reference is the safe failure mode.
    }
  }

  CaresRuntimeCoordinator() = default;
  ~CaresRuntimeCoordinator() = default;
  CaresRuntimeCoordinator(const CaresRuntimeCoordinator &) = delete;
  auto operator=(const CaresRuntimeCoordinator &)
      -> CaresRuntimeCoordinator & = delete;

private:
  friend class CaresRuntimeUse;
  friend auto cares_runtime_coordinator() noexcept -> CaresRuntimeCoordinator &;

  std::mutex m_mutex;
  std::condition_variable m_condition;
  std::size_t m_owners{};
  std::size_t m_users{};
  bool m_initialized{};
  bool m_stopping{};
};

class CaresRuntimeUse final {
public:
  explicit CaresRuntimeUse(CaresRuntimeCoordinator &coordinator) noexcept
      : m_coordinator{&coordinator} {}
  ~CaresRuntimeUse() {
    if (m_coordinator != nullptr)
      m_coordinator->release_use();
  }
  CaresRuntimeUse(const CaresRuntimeUse &) = delete;
  auto operator=(const CaresRuntimeUse &) -> CaresRuntimeUse & = delete;
  CaresRuntimeUse(CaresRuntimeUse &&other) noexcept
      : m_coordinator{std::exchange(other.m_coordinator, nullptr)} {}
  auto operator=(CaresRuntimeUse &&) -> CaresRuntimeUse & = delete;

private:
  CaresRuntimeCoordinator *m_coordinator{};
};

[[nodiscard]] inline auto cares_runtime_coordinator() noexcept
    -> CaresRuntimeCoordinator & {
  static CaresRuntimeCoordinator coordinator;
  return coordinator;
}

[[nodiscard]] inline auto acquire_cares_runtime()
    -> std::optional<CaresRuntimeUse> {
  auto &coordinator = cares_runtime_coordinator();
  if (!coordinator.acquire())
    return std::nullopt;
  return CaresRuntimeUse{coordinator};
}

} // namespace venice::detail

namespace venice {

inline auto VideoDownloadRuntime::initialize()
    -> std::expected<VideoDownloadRuntime, Error> {
  try {
    if (!detail::cares_runtime_coordinator().initialize(
            [] { return ares_library_init(ARES_LIB_INIT_ALL); }))
      return std::unexpected{detail::video_download_error(
          ErrorKind::Network, "video download runtime initialization failed")};
    return VideoDownloadRuntime{ActiveTag{}};
  } catch (...) {
    return std::unexpected{detail::video_download_error(
        ErrorKind::Network, "video download runtime initialization failed")};
  }
}

inline VideoDownloadRuntime::~VideoDownloadRuntime() noexcept {
  if (m_owner)
    detail::cares_runtime_coordinator().release([] { ares_library_cleanup(); });
}

} // namespace venice

namespace venice::detail {

class CaresResolverDriver {
public:
  virtual ~CaresResolverDriver() = default;
  [[nodiscard]] virtual auto start(std::string_view host) -> bool = 0;
  [[nodiscard]] virtual auto complete() const noexcept -> bool = 0;
  [[nodiscard]] virtual auto allocation_failed() const noexcept -> bool = 0;
  [[nodiscard]] virtual auto sockets() const noexcept
      -> const std::vector<CaresSocketState::Socket> & = 0;
  [[nodiscard]] virtual auto process(const ares_fd_events_t *events,
                                     std::size_t event_count) -> bool = 0;
  virtual void cancel() noexcept = 0;
  [[nodiscard]] virtual auto status() const noexcept -> int = 0;
  [[nodiscard]] virtual auto take_addresses() -> std::vector<std::string> = 0;
};

class SystemCaresResolverDriver final : public CaresResolverDriver {
public:
  [[nodiscard]] auto start(std::string_view host) -> bool override {
    ares_options options{};
    options.sock_state_cb = cares_socket_state;
    options.sock_state_cb_data = &m_socket_state;
    if (ares_init_options(m_channel.pointer(), &options,
                          ARES_OPT_SOCK_STATE_CB) != ARES_SUCCESS)
      return false;

    ares_addrinfo_hints hints{};
    hints.ai_flags = ARES_AI_NOSORT | ARES_AI_NUMERICSERV;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    m_host = host;
    ares_getaddrinfo(m_channel.get(), m_host.c_str(), "443", &hints,
                     cares_address_result, &m_result);
    return true;
  }

  [[nodiscard]] auto complete() const noexcept -> bool override {
    return m_result.complete;
  }
  [[nodiscard]] auto allocation_failed() const noexcept -> bool override {
    return m_socket_state.allocation_failure;
  }
  [[nodiscard]] auto sockets() const noexcept
      -> const std::vector<CaresSocketState::Socket> & override {
    return m_socket_state.sockets;
  }
  [[nodiscard]] auto process(const ares_fd_events_t *events,
                             std::size_t event_count) -> bool override {
    return ares_process_fds(m_channel.get(), events, event_count,
                            ARES_PROCESS_FLAG_NONE) == ARES_SUCCESS;
  }
  void cancel() noexcept override { ares_cancel(m_channel.get()); }
  [[nodiscard]] auto status() const noexcept -> int override {
    return m_result.status;
  }
  [[nodiscard]] auto take_addresses() -> std::vector<std::string> override {
    return std::move(m_result.addresses);
  }

private:
  CaresSocketState m_socket_state;
  CaresResult m_result;
  CaresChannel m_channel;
  std::string m_host;
};

class CaresVideoDownloadResolver final {
public:
  using Clock = std::chrono::steady_clock;

  CaresVideoDownloadResolver() = default;
  explicit CaresVideoDownloadResolver(CaresResolverDriver &driver) noexcept
      : m_driver{&driver} {}

  [[nodiscard]] auto resolve(std::string_view host, Clock::time_point deadline,
                             CancelToken *cancel)
      -> std::expected<std::vector<std::string>, Error> {
    auto runtime = acquire_cares_runtime();
    if (!runtime)
      return std::unexpected{video_download_error(
          ErrorKind::InvalidArg, "video download runtime is not initialized")};
    const std::string owned_host{host};
    std::array<unsigned char, 16> numeric{};
    if (parse_video_download_address(AF_INET, host, numeric) ||
        parse_video_download_address(AF_INET6, host, numeric))
      return std::vector<std::string>{owned_host};
    SystemCaresResolverDriver system_driver;
    auto &driver = m_driver == nullptr
                       ? static_cast<CaresResolverDriver &>(system_driver)
                       : *m_driver;
    if (!driver.start(owned_host))
      return std::unexpected{video_download_error(
          ErrorKind::Network, "video download DNS initialization failed")};

    while (!driver.complete()) {
      if (cancel != nullptr && cancel->cancelled()) {
        driver.cancel();
        return std::unexpected{video_download_cancelled()};
      }
      const auto now = Clock::now();
      if (now >= deadline) {
        driver.cancel();
        return std::unexpected{video_download_timed_out()};
      }
      if (driver.allocation_failed()) {
        driver.cancel();
        return std::unexpected{video_download_error(
            ErrorKind::Network, "video download DNS resolution failed")};
      }

      const auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
      const int wait_ms =
          static_cast<int>(std::clamp<std::int64_t>(remaining.count(), 1, 10));

#ifdef _WIN32
      std::vector<WSAPOLLFD> polled;
      polled.reserve(driver.sockets().size());
      for (const auto &socket : driver.sockets())
        polled.push_back(
            {socket.descriptor,
             static_cast<short>((socket.readable ? POLLRDNORM : 0) |
                                (socket.writable ? POLLWRNORM : 0)),
             0});
      const int ready =
          polled.empty() ? (std::this_thread::sleep_for(
                                std::chrono::milliseconds{wait_ms}),
                            0)
                         : WSAPoll(polled.data(),
                                   static_cast<ULONG>(polled.size()), wait_ms);
#else
      std::vector<pollfd> polled;
      polled.reserve(driver.sockets().size());
      for (const auto &socket : driver.sockets())
        polled.push_back({socket.descriptor,
                          static_cast<short>((socket.readable ? POLLIN : 0) |
                                             (socket.writable ? POLLOUT : 0)),
                          0});
      const int ready =
          ::poll(polled.data(), static_cast<nfds_t>(polled.size()), wait_ms);
#endif
      if (ready < 0) {
#ifndef _WIN32
        if (errno == EINTR)
          continue;
#endif
        driver.cancel();
        return std::unexpected{video_download_error(
            ErrorKind::Network, "video download DNS resolution failed")};
      }

      std::vector<ares_fd_events_t> events;
      if (ready > 0) {
        events.reserve(polled.size());
        for (const auto &socket : polled) {
          unsigned int event = ARES_FD_EVENT_NONE;
#ifdef _WIN32
          if ((socket.revents & (POLLRDNORM | POLLERR | POLLHUP)) != 0)
            event |= ARES_FD_EVENT_READ;
          if ((socket.revents & (POLLWRNORM | POLLERR | POLLHUP)) != 0)
            event |= ARES_FD_EVENT_WRITE;
#else
          if ((socket.revents & (POLLIN | POLLERR | POLLHUP)) != 0)
            event |= ARES_FD_EVENT_READ;
          if ((socket.revents & (POLLOUT | POLLERR | POLLHUP)) != 0)
            event |= ARES_FD_EVENT_WRITE;
#endif
          if (event != ARES_FD_EVENT_NONE)
            events.push_back({socket.fd, event});
        }
      }
      const auto *event_data = events.empty() ? nullptr : events.data();
      if (!driver.process(event_data, events.size())) {
        driver.cancel();
        return std::unexpected{video_download_error(
            ErrorKind::Network, "video download DNS resolution failed")};
      }
    }

    if (driver.status() != ARES_SUCCESS)
      return std::unexpected{video_download_error(
          ErrorKind::Network, "video download DNS resolution failed")};
    auto addresses = driver.take_addresses();
    std::sort(addresses.begin(), addresses.end());
    addresses.erase(std::unique(addresses.begin(), addresses.end()),
                    addresses.end());
    return addresses;
  }

private:
  CaresResolverDriver *m_driver{};
};

class VideoDownloadDeadlineGuard final {
public:
  using Clock = std::chrono::steady_clock;

  VideoDownloadDeadlineGuard(httplib::Client &client,
                             Clock::time_point deadline)
      : m_sigpipe{true},
        m_thread{[this, &client, deadline] { watch(client, deadline); }} {}

  ~VideoDownloadDeadlineGuard() { release(); }
  VideoDownloadDeadlineGuard(const VideoDownloadDeadlineGuard &) = delete;
  auto operator=(const VideoDownloadDeadlineGuard &)
      -> VideoDownloadDeadlineGuard & = delete;

  [[nodiscard]] auto expired() const noexcept -> bool {
    return m_expired.load(std::memory_order_acquire);
  }

private:
  void watch(httplib::Client &client, Clock::time_point deadline) {
    {
      std::unique_lock lock{m_mutex};
      if (m_condition.wait_until(lock, deadline, [this] { return m_released; }))
        return;
      m_expired.store(true, std::memory_order_release);
    }
    for (;;) {
      client.stop();
      std::unique_lock lock{m_mutex};
      if (m_condition.wait_for(lock, std::chrono::milliseconds{2},
                               [this] { return m_released; }))
        return;
    }
  }

  void release() {
    {
      const std::lock_guard lock{m_mutex};
      m_released = true;
    }
    m_condition.notify_all();
    if (m_thread.joinable())
      m_thread.join();
  }

  SigPipeBlock m_sigpipe;
  mutable std::mutex m_mutex;
  std::condition_variable m_condition;
  std::atomic<bool> m_expired{};
  bool m_released{};
  std::thread m_thread;
};

[[nodiscard]] inline auto
capped_timeout(std::optional<std::chrono::milliseconds> requested,
               std::chrono::milliseconds fallback,
               std::chrono::steady_clock::time_point deadline)
    -> std::chrono::milliseconds {
  const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
      deadline - std::chrono::steady_clock::now());
  const auto positive_remaining =
      std::max(remaining, std::chrono::milliseconds{1});
  return std::min(requested.value_or(fallback), positive_remaining);
}

class HttplibVideoDownloadFetcher final {
public:
  using Clock = std::chrono::steady_clock;

  explicit HttplibVideoDownloadFetcher(std::string trusted_ca = {})
      : m_trusted_ca{std::move(trusted_ca)} {}

  [[nodiscard]] auto
  fetch(const VideoDownloadUrl &url, const std::string &address,
        std::size_t maximum_bytes, Clock::time_point deadline,
        const RequestOptions &options)
      -> std::expected<VideoDownloadResponse, Error> {
    if (Clock::now() >= deadline)
      return std::unexpected{video_download_timed_out()};
    try {
      // Use cpp-httplib's scheme-aware wrapper: CancelGuard intentionally
      // targets this stable public client surface, while the concrete
      // SSLClient is its private HTTPS implementation.
      httplib::Client client{url.origin()};
      if (!m_trusted_ca.empty())
        client.load_ca_cert_store(m_trusted_ca.data(), m_trusted_ca.size());
      client.set_hostname_addr_map({{url.host, address}});
      client.enable_server_certificate_verification(true);
      client.enable_server_hostname_verification(true);
      client.set_follow_location(false);
      client.set_url_encode(false);
      client.set_connection_timeout(capped_timeout(
          options.connect_timeout, std::chrono::seconds{30}, deadline));
      client.set_read_timeout(capped_timeout(
          options.read_timeout, std::chrono::seconds{300}, deadline));
      client.set_write_timeout(capped_timeout(
          options.write_timeout, std::chrono::seconds{300}, deadline));

      const CancelGuard cancellation{options.cancel, client};
      VideoDownloadDeadlineGuard time_limit{client, deadline};
      if (cancellation.cancelled())
        return std::unexpected{video_download_cancelled()};

      httplib::Request request;
      request.method = "GET";
      request.path = url.target;
      request.set_header("Accept", "video/mp4");

      httplib::Response response;
      httplib::Error transport_status = httplib::Error::Success;
      VideoDownloadBodyLimit body_limit{maximum_bytes};
      bool unsupported_encoding = false;
      request.response_handler = [&](const httplib::Response &headers) {
        if (!video_download_content_encoding_supported(headers)) {
          unsupported_encoding = true;
          return false;
        }
        return video_download_headers_within_limit(body_limit, headers);
      };
      request.content_receiver = [&](const char *data, std::size_t length,
                                     std::size_t, std::uint64_t) {
        if (!body_limit.accept(length))
          return false;
        response.body.append(data, length);
        return true;
      };
      const bool sent = client.send(request, response, transport_status);

      if (cancellation.cancelled())
        return std::unexpected{video_download_cancelled()};
      if (time_limit.expired() || Clock::now() >= deadline)
        return std::unexpected{video_download_timed_out()};
      if (body_limit.exceeded())
        return std::unexpected{video_download_error(
            ErrorKind::ResponseTooLarge,
            "video download exceeds its byte limit", response.status)};
      if (unsupported_encoding)
        return std::unexpected{video_download_error(
            ErrorKind::Parse, "video download response encoding is unsupported",
            response.status)};
      if (!sent)
        return std::unexpected{video_download_error(
            ErrorKind::Network, "video download transport failed")};

      std::optional<std::string> location;
      if (response.get_header_value_count("Location") == 1)
        location = response.get_header_value("Location");
      return VideoDownloadResponse{
          response.status,
          response.get_header_value_count("Content-Type") == 1
              ? normalize_video_download_media_type(
                    response.get_header_value("Content-Type"))
              : std::string{},
          std::move(response.body), std::move(location)};
    } catch (...) {
      return std::unexpected{video_download_error(
          ErrorKind::Network, "video download transport failed")};
    }
  }

private:
  std::string m_trusted_ca;
};

struct SteadyNow {
  [[nodiscard]] auto operator()() const noexcept
      -> std::chrono::steady_clock::time_point {
    return std::chrono::steady_clock::now();
  }
};

template <typename Resolver, typename Fetcher, typename Now = SteadyNow>
[[nodiscard]] auto download_video_with(const VideoDownloadRequest &request,
                                       const RequestOptions &options,
                                       Resolver &resolver, Fetcher &fetcher,
                                       Now now = {})
    -> std::expected<VideoMedia, Error> {
  using Clock = std::chrono::steady_clock;
  if (request.maximum_bytes == 0)
    return std::unexpected{video_download_error(
        ErrorKind::InvalidArg, "video download byte limit must be nonzero")};
  if (request.maximum_elapsed.count() <= 0)
    return std::unexpected{video_download_error(
        ErrorKind::InvalidArg, "video download deadline must be nonzero")};
  for (const auto timeout :
       {options.connect_timeout, options.read_timeout, options.write_timeout})
    if (timeout && timeout->count() <= 0)
      return std::unexpected{video_download_error(
          ErrorKind::InvalidArg, "video download timeouts must be positive")};
  if (options.maximum_response_bytes && *options.maximum_response_bytes == 0)
    return std::unexpected{video_download_error(
        ErrorKind::InvalidArg, "video download byte limit must be nonzero")};

  const auto started = now();
  const auto available = Clock::time_point::max() - started;
  if (request.maximum_elapsed >
      std::chrono::duration_cast<std::chrono::milliseconds>(available))
    return std::unexpected{video_download_error(
        ErrorKind::InvalidArg, "video download deadline is out of range")};
  const auto deadline = started + request.maximum_elapsed;
  auto url = parse_video_download_url(request.url);
  if (!url)
    return std::unexpected{std::move(url.error())};

  std::vector<std::string> visited;
  visited.reserve(kMaximumVideoDownloadRedirects + 1);
  for (std::size_t redirects = 0;;) {
    if (options.cancel != nullptr && options.cancel->cancelled())
      return std::unexpected{video_download_cancelled()};
    if (now() >= deadline)
      return std::unexpected{video_download_timed_out()};
    const auto canonical = url->canonical();
    if (std::find(visited.begin(), visited.end(), canonical) != visited.end())
      return std::unexpected{video_download_error(
          ErrorKind::Http, "video download redirect loop")};
    visited.push_back(canonical);

    auto addresses = resolver.resolve(url->host, deadline, options.cancel);
    if (!addresses)
      return std::unexpected{std::move(addresses.error())};
    if (options.cancel != nullptr && options.cancel->cancelled())
      return std::unexpected{video_download_cancelled()};
    if (now() >= deadline)
      return std::unexpected{video_download_timed_out()};
    if (auto valid = validate_video_download_addresses(*addresses); !valid)
      return std::unexpected{std::move(valid.error())};

    const auto byte_limit =
        options.maximum_response_bytes
            ? std::min(request.maximum_bytes, *options.maximum_response_bytes)
            : request.maximum_bytes;
    const RequestOptions transport_options{
        .connect_timeout = options.connect_timeout,
        .read_timeout = options.read_timeout,
        .write_timeout = options.write_timeout,
        .cancel = options.cancel,
        .maximum_response_bytes = byte_limit,
    };
    auto response = fetcher.fetch(*url, addresses->front(), byte_limit,
                                  deadline, transport_options);
    if (!response)
      return std::unexpected{std::move(response.error())};
    if (options.cancel != nullptr && options.cancel->cancelled())
      return std::unexpected{video_download_cancelled()};
    if (now() >= deadline)
      return std::unexpected{video_download_timed_out()};
    if (response->body.size() > byte_limit)
      return std::unexpected{video_download_error(
          ErrorKind::ResponseTooLarge, "video download exceeds its byte limit",
          response->status)};

    const bool redirect = response->status == 301 || response->status == 302 ||
                          response->status == 303 || response->status == 307 ||
                          response->status == 308;
    if (redirect) {
      if (!response->location || redirects >= kMaximumVideoDownloadRedirects)
        return std::unexpected{video_download_error(
            ErrorKind::Http, "video download redirect was not usable",
            response->status)};
      auto next = resolve_video_download_redirect(*url, *response->location);
      if (!next)
        return std::unexpected{std::move(next.error())};
      url = std::move(next);
      ++redirects;
      continue;
    }
    if (response->status < 200 || response->status >= 300)
      return std::unexpected{video_download_error(
          kind_for_status(response->status),
          "video download returned a non-success status", response->status)};
    if (normalize_video_download_media_type(response->content_type) !=
        "video/mp4")
      return std::unexpected{video_download_error(
          ErrorKind::Parse, "video download response is not video/mp4",
          response->status)};
    if (response->body.empty())
      return std::unexpected{video_download_error(
          ErrorKind::Parse, "video download response is empty",
          response->status)};
    return VideoMedia{.bytes = std::move(response->body),
                      .media_type = "video/mp4"};
  }
}

[[nodiscard]] inline auto download_video(const VideoDownloadRequest &request,
                                         const RequestOptions &options)
    -> std::expected<VideoMedia, Error> {
  try {
    CaresVideoDownloadResolver resolver;
    HttplibVideoDownloadFetcher fetcher;
    return download_video_with(request, options, resolver, fetcher);
  } catch (...) {
    return std::unexpected{
        video_download_error(ErrorKind::Network, "video download failed")};
  }
}

} // namespace venice::detail
