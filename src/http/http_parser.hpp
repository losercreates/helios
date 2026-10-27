#pragma once

#include "buffer/buffer.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace helios {

// Standard HTTP methods
enum class HttpMethod : uint8_t {
    UNKNOWN = 0,
    GET,
    POST,
    PUT,
    DELETE_,
    HEAD,
    OPTIONS,
    PATCH,
    CONNECT,
    TRACE
};

[[nodiscard]] std::string_view ToString(HttpMethod method) noexcept;
[[nodiscard]] HttpMethod ParseMethod(std::string_view str) noexcept;

// Status returned by HttpParser::Parse()
enum class ParseStatus : uint8_t {
    Incomplete = 0,      // Need more bytes to complete current stage
    HeadersComplete,     // Headers fully parsed, ready for body streaming or dispatch
    BodyChunk,           // A chunk of body data was consumed (streaming)
    Complete,            // Entire HTTP message (headers + full body) complete
    ErrorBadRequest,     // HTTP 400: Malformed syntax, RFC 9112 violation, or smuggling attempt
    ErrorHeaderTooLarge, // HTTP 431: Request Header Fields Too Large
    ErrorPayloadTooLarge // HTTP 413: Payload Too Large
};

[[nodiscard]] std::string_view ToString(ParseStatus status) noexcept;

// Zero-copy representation of an HTTP header field
struct HttpHeader {
    std::string_view name;
    std::string_view value;
};

// Maximum number of headers in deterministic fixed table
inline constexpr size_t kDefaultMaxHeaders = 64;
// Maximum header block size (8 KB per Helios spec)
inline constexpr size_t kDefaultMaxHeaderSize = 8192;

// HTTP request representation with zero-copy views
struct HttpRequest {
    HttpMethod method{HttpMethod::UNKNOWN};
    std::string_view method_str;
    std::string_view uri;
    std::string_view path;
    std::string_view query;
    int http_major{1};
    int http_minor{1};

    std::array<HttpHeader, kDefaultMaxHeaders> headers{};
    size_t num_headers{0};

    std::string_view body{}; // Direct view into buffer when body was contiguous
    size_t content_length{0};
    bool has_content_length{false};
    bool is_chunked{false};
    bool keep_alive{true};
    bool headers_complete{false};
    bool is_complete{false};
    bool is_upgrade{false};
    std::string_view upgrade_protocol{};

    [[nodiscard]] std::span<const HttpHeader> Headers() const noexcept {
        return std::span<const HttpHeader>(headers.data(), num_headers);
    }

    [[nodiscard]] std::optional<std::string_view> GetHeader(std::string_view name) const noexcept;
    [[nodiscard]] bool HasHeader(std::string_view name) const noexcept;
    [[nodiscard]] std::optional<std::string_view> GetQueryParam(std::string_view key) const noexcept;
    [[nodiscard]] bool IsUpgrade() const noexcept { return is_upgrade; }
    [[nodiscard]] bool IsWebSocketUpgrade() const noexcept;
    void Reset() noexcept;
};

// Result returned by HttpParser::Parse()
struct ParseResult {
    ParseStatus status{ParseStatus::Incomplete};
    size_t bytes_consumed{0};
    std::string_view body_chunk{};
    int error_code{0}; // e.g. 400, 431, 413
    std::string_view error_message{};

    [[nodiscard]] bool IsComplete() const noexcept { return status == ParseStatus::Complete; }
    [[nodiscard]] bool IsHeadersComplete() const noexcept {
        return status == ParseStatus::HeadersComplete || status == ParseStatus::Complete;
    }
    [[nodiscard]] bool IsError() const noexcept {
        return status == ParseStatus::ErrorBadRequest ||
               status == ParseStatus::ErrorHeaderTooLarge ||
               status == ParseStatus::ErrorPayloadTooLarge;
    }
    [[nodiscard]] bool IsIncomplete() const noexcept { return status == ParseStatus::Incomplete; }
};

// Zero-copy streaming HTTP/1.1 parser & RFC 9112 request smuggling validator
class HttpParser {
public:
    using BodyChunkCallback = std::function<void(std::string_view)>;

    explicit HttpParser(size_t max_header_size = kDefaultMaxHeaderSize) noexcept;
    ~HttpParser() = default;

    // Movable with pointer rebasing
    HttpParser(HttpParser&& other) noexcept;
    HttpParser& operator=(HttpParser&& other) noexcept;

    // Disallow copies
    HttpParser(const HttpParser&) = delete;
    HttpParser& operator=(const HttpParser&) = delete;

    // Parse incoming data slice (zero-copy on hot path)
    ParseResult Parse(std::string_view data);

    // Convenience overload for Helios Buffer
    ParseResult Parse(Buffer& buffer);

    // Reset parser state for next pipelined/keep-alive request
    void Reset() noexcept;

    // Query parsed state
    [[nodiscard]] const HttpRequest& GetRequest() const noexcept { return request_; }
    [[nodiscard]] HttpRequest& GetRequest() noexcept { return request_; }
    [[nodiscard]] std::string_view GetBody() const noexcept;
    [[nodiscard]] const std::string& GetDecodedBody() const noexcept { return decoded_body_; }

    void SetBodyCallback(BodyChunkCallback cb) { body_callback_ = std::move(cb); }

    // Case-insensitive ASCII equality helper
    static bool CaseInsensitiveEqual(std::string_view a, std::string_view b) noexcept;

    // Checks if a comma-delimited header contains a specific token (case-insensitive, trims OWS)
    [[nodiscard]] static bool ContainsToken(std::string_view header_val, std::string_view token) noexcept;

private:
    enum class ParserState : uint8_t {
        RequestLineAndHeaders,
        BodyIdentity,
        ChunkSize,
        ChunkData,
        ChunkCrlf,
        ChunkTrailers,
        Complete,
        Error
    };

    ParseResult ParseHeaders(std::string_view data);
    ParseResult ParseBodyIdentity(std::string_view data);
    ParseResult ParseChunked(std::string_view data);
    bool ValidateHeadersRfc9112(ParseResult& result);
    void RebaseViews(const char* old_base, const char* new_base) noexcept;

    size_t max_header_size_{kDefaultMaxHeaderSize};
    ParserState state_{ParserState::RequestLineAndHeaders};
    HttpRequest request_{};

    // Reassembly storage for headers fragmented across reads
    std::array<char, kDefaultMaxHeaderSize> header_storage_{};
    size_t header_storage_len_{0};
    bool using_header_storage_{false};

    // Streaming body tracking
    size_t body_bytes_remaining_{0};
    size_t current_chunk_size_{0};
    size_t current_chunk_remaining_{0};
    uint8_t crlf_bytes_read_{0};

    // Scratch line buffer for chunk framing split across reads
    std::array<char, 256> chunk_line_buf_{};
    size_t chunk_line_buf_len_{0};

    // Accumulated decoded body (for tests & small requests) and streaming callback
    std::string decoded_body_;
    BodyChunkCallback body_callback_{};

    int error_code_{0};
    std::string_view error_message_{};
};

} // namespace helios
