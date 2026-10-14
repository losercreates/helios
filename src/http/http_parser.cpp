#include "http_parser.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>

namespace helios {

namespace {

[[nodiscard]] constexpr bool IsTokenChar(char c) noexcept {
    auto uc = static_cast<unsigned char>(c);
    if ((uc >= 'a' && uc <= 'z') || (uc >= 'A' && uc <= 'Z') || (uc >= '0' && uc <= '9')) {
        return true;
    }
    switch (c) {
        case '!': case '#': case '$': case '%': case '&': case '\'':
        case '*': case '+': case '-': case '.': case '^': case '_':
        case '`': case '|': case '~':
            return true;
        default:
            return false;
    }
}

[[nodiscard]] constexpr int HexDigit(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

[[nodiscard]] std::string_view TrimOws(std::string_view sv) noexcept {
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t')) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t')) {
        sv.remove_suffix(1);
    }
    return sv;
}

} // namespace

std::string_view ToString(HttpMethod method) noexcept {
    switch (method) {
        case HttpMethod::GET: return "GET";
        case HttpMethod::POST: return "POST";
        case HttpMethod::PUT: return "PUT";
        case HttpMethod::DELETE_: return "DELETE";
        case HttpMethod::HEAD: return "HEAD";
        case HttpMethod::OPTIONS: return "OPTIONS";
        case HttpMethod::PATCH: return "PATCH";
        case HttpMethod::CONNECT: return "CONNECT";
        case HttpMethod::TRACE: return "TRACE";
        case HttpMethod::UNKNOWN:
        default:
            return "UNKNOWN";
    }
}

HttpMethod ParseMethod(std::string_view str) noexcept {
    if (str == "GET") return HttpMethod::GET;
    if (str == "POST") return HttpMethod::POST;
    if (str == "PUT") return HttpMethod::PUT;
    if (str == "DELETE") return HttpMethod::DELETE_;
    if (str == "HEAD") return HttpMethod::HEAD;
    if (str == "OPTIONS") return HttpMethod::OPTIONS;
    if (str == "PATCH") return HttpMethod::PATCH;
    if (str == "CONNECT") return HttpMethod::CONNECT;
    if (str == "TRACE") return HttpMethod::TRACE;
    return HttpMethod::UNKNOWN;
}

std::string_view ToString(ParseStatus status) noexcept {
    switch (status) {
        case ParseStatus::Incomplete: return "Incomplete";
        case ParseStatus::HeadersComplete: return "HeadersComplete";
        case ParseStatus::BodyChunk: return "BodyChunk";
        case ParseStatus::Complete: return "Complete";
        case ParseStatus::ErrorBadRequest: return "ErrorBadRequest";
        case ParseStatus::ErrorHeaderTooLarge: return "ErrorHeaderTooLarge";
        case ParseStatus::ErrorPayloadTooLarge: return "ErrorPayloadTooLarge";
    }
    return "Unknown";
}

bool HttpParser::CaseInsensitiveEqual(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::optional<std::string_view> HttpRequest::GetHeader(std::string_view name) const noexcept {
    for (size_t i = 0; i < num_headers; ++i) {
        if (HttpParser::CaseInsensitiveEqual(headers[i].name, name)) {
            return headers[i].value;
        }
    }
    return std::nullopt;
}

bool HttpRequest::HasHeader(std::string_view name) const noexcept {
    return GetHeader(name).has_value();
}

std::optional<std::string_view> HttpRequest::GetQueryParam(std::string_view key) const noexcept {
    if (query.empty() || key.empty()) return std::nullopt;

    std::string_view rem = query;
    while (!rem.empty()) {
        auto ampersand = rem.find('&');
        std::string_view pair = (ampersand != std::string_view::npos) ? rem.substr(0, ampersand) : rem;
        rem = (ampersand != std::string_view::npos) ? rem.substr(ampersand + 1) : std::string_view{};

        auto eq = pair.find('=');
        std::string_view k = (eq != std::string_view::npos) ? pair.substr(0, eq) : pair;
        std::string_view v = (eq != std::string_view::npos) ? pair.substr(eq + 1) : std::string_view{};

        if (k == key) {
            return v;
        }
    }
    return std::nullopt;
}

void HttpRequest::Reset() noexcept {
    method = HttpMethod::UNKNOWN;
    method_str = {};
    uri = {};
    path = {};
    query = {};
    http_major = 1;
    http_minor = 1;
    num_headers = 0;
    body = {};
    content_length = 0;
    has_content_length = false;
    is_chunked = false;
    keep_alive = true;
    headers_complete = false;
    is_complete = false;
}

HttpParser::HttpParser(size_t max_header_size) noexcept
    : max_header_size_(std::min(max_header_size, kDefaultMaxHeaderSize)) {
}

HttpParser::HttpParser(HttpParser&& other) noexcept
    : max_header_size_(other.max_header_size_),
      state_(other.state_),
      request_(other.request_),
      header_storage_(other.header_storage_),
      header_storage_len_(other.header_storage_len_),
      using_header_storage_(other.using_header_storage_),
      body_bytes_remaining_(other.body_bytes_remaining_),
      current_chunk_size_(other.current_chunk_size_),
      current_chunk_remaining_(other.current_chunk_remaining_),
      crlf_bytes_read_(other.crlf_bytes_read_),
      chunk_line_buf_(other.chunk_line_buf_),
      chunk_line_buf_len_(other.chunk_line_buf_len_),
      decoded_body_(std::move(other.decoded_body_)),
      body_callback_(std::move(other.body_callback_)),
      error_code_(other.error_code_),
      error_message_(other.error_message_) {
    if (using_header_storage_) {
        RebaseViews(other.header_storage_.data(), header_storage_.data());
    }
    other.Reset();
}

HttpParser& HttpParser::operator=(HttpParser&& other) noexcept {
    if (this != &other) {
        const char* other_old_base = other.header_storage_.data();
        max_header_size_ = other.max_header_size_;
        state_ = other.state_;
        request_ = other.request_;
        header_storage_ = other.header_storage_;
        header_storage_len_ = other.header_storage_len_;
        using_header_storage_ = other.using_header_storage_;
        body_bytes_remaining_ = other.body_bytes_remaining_;
        current_chunk_size_ = other.current_chunk_size_;
        current_chunk_remaining_ = other.current_chunk_remaining_;
        crlf_bytes_read_ = other.crlf_bytes_read_;
        chunk_line_buf_ = other.chunk_line_buf_;
        chunk_line_buf_len_ = other.chunk_line_buf_len_;
        decoded_body_ = std::move(other.decoded_body_);
        body_callback_ = std::move(other.body_callback_);
        error_code_ = other.error_code_;
        error_message_ = other.error_message_;

        if (using_header_storage_) {
            RebaseViews(other_old_base, header_storage_.data());
        }
        other.Reset();
    }
    return *this;
}

void HttpParser::RebaseViews(const char* old_base, const char* new_base) noexcept {
    auto rebase_sv = [old_base, new_base](std::string_view& sv) {
        if (!sv.empty() && sv.data() >= old_base && sv.data() < old_base + kDefaultMaxHeaderSize) {
            auto offset = static_cast<size_t>(sv.data() - old_base);
            sv = std::string_view(new_base + offset, sv.size());
        }
    };

    rebase_sv(request_.method_str);
    rebase_sv(request_.uri);
    rebase_sv(request_.path);
    rebase_sv(request_.query);
    for (size_t i = 0; i < request_.num_headers; ++i) {
        rebase_sv(request_.headers[i].name);
        rebase_sv(request_.headers[i].value);
    }
}

void HttpParser::Reset() noexcept {
    state_ = ParserState::RequestLineAndHeaders;
    request_.Reset();
    header_storage_len_ = 0;
    using_header_storage_ = false;
    body_bytes_remaining_ = 0;
    current_chunk_size_ = 0;
    current_chunk_remaining_ = 0;
    crlf_bytes_read_ = 0;
    chunk_line_buf_len_ = 0;
    decoded_body_.clear();
    error_code_ = 0;
    error_message_ = {};
}

std::string_view HttpParser::GetBody() const noexcept {
    if (!request_.body.empty()) {
        return request_.body;
    }
    return std::string_view(decoded_body_);
}

ParseResult HttpParser::Parse(Buffer& buffer) {
    auto slice = std::string_view(reinterpret_cast<const char*>(buffer.ReadPtr()), buffer.ReadableBytes());
    ParseResult result = Parse(slice);
    buffer.AdvanceRead(result.bytes_consumed);
    return result;
}

ParseResult HttpParser::Parse(std::string_view data) {
    if (state_ == ParserState::Error) {
        return ParseResult{
            .status = ParseStatus::ErrorBadRequest,
            .bytes_consumed = 0,
            .body_chunk = {},
            .error_code = error_code_,
            .error_message = error_message_
        };
    }

    if (state_ == ParserState::Complete) {
        return ParseResult{
            .status = ParseStatus::Complete,
            .bytes_consumed = 0,
            .body_chunk = {},
            .error_code = 0,
            .error_message = {}
        };
    }

    if (state_ == ParserState::RequestLineAndHeaders) {
        return ParseHeaders(data);
    }

    if (state_ == ParserState::BodyIdentity) {
        return ParseBodyIdentity(data);
    }

    if (state_ >= ParserState::ChunkSize && state_ <= ParserState::ChunkTrailers) {
        return ParseChunked(data);
    }

    return ParseResult{.status = ParseStatus::Incomplete, .bytes_consumed = 0};
}

ParseResult HttpParser::ParseHeaders(std::string_view data) {
    ParseResult result{};

    std::string_view header_source;
    size_t header_end_pos = std::string_view::npos;
    bool using_storage_this_round = using_header_storage_;

    if (!using_storage_this_round) {
        // Fast path: check if header block ends within incoming data slice
        auto crlf2 = data.find("\r\n\r\n");
        if (crlf2 != std::string_view::npos) {
            header_end_pos = crlf2 + 4;
            header_source = data.substr(0, header_end_pos);
        } else {
            auto lf2 = data.find("\n\n");
            if (lf2 != std::string_view::npos) {
                header_end_pos = lf2 + 2;
                header_source = data.substr(0, header_end_pos);
            }
        }
    }

    if (header_end_pos == std::string_view::npos) {
        // Header is split or fragmented across reads; accumulate into fixed reassembly buffer
        using_header_storage_ = true;
        if (header_storage_len_ + data.size() > max_header_size_) {
            state_ = ParserState::Error;
            error_code_ = 431;
            error_message_ = "Request Header Fields Too Large";
            return ParseResult{
                .status = ParseStatus::ErrorHeaderTooLarge,
                .bytes_consumed = 0,
                .error_code = 431,
                .error_message = error_message_
            };
        }

        std::memcpy(header_storage_.data() + header_storage_len_, data.data(), data.size());
        header_storage_len_ += data.size();

        std::string_view accumulated(header_storage_.data(), header_storage_len_);
        auto crlf2 = accumulated.find("\r\n\r\n");
        if (crlf2 != std::string_view::npos) {
            header_end_pos = crlf2 + 4;
            header_source = accumulated.substr(0, header_end_pos);
        } else {
            auto lf2 = accumulated.find("\n\n");
            if (lf2 != std::string_view::npos) {
                header_end_pos = lf2 + 2;
                header_source = accumulated.substr(0, header_end_pos);
            }
        }

        if (header_end_pos == std::string_view::npos) {
            // Still waiting for complete header section
            return ParseResult{
                .status = ParseStatus::Incomplete,
                .bytes_consumed = data.size()
            };
        }
    }

    // Parse Request Line
    auto first_nl = header_source.find('\n');
    if (first_nl == std::string_view::npos) {
        state_ = ParserState::Error;
        error_code_ = 400;
        error_message_ = "Malformed HTTP request line";
        return ParseResult{.status = ParseStatus::ErrorBadRequest, .error_code = 400, .error_message = error_message_};
    }

    std::string_view req_line = header_source.substr(0, first_nl);
    if (!req_line.empty() && req_line.back() == '\r') {
        req_line.remove_suffix(1);
    }

    // Disallow null bytes
    if (req_line.find('\0') != std::string_view::npos) {
        state_ = ParserState::Error;
        error_code_ = 400;
        error_message_ = "Null byte in request line";
        return ParseResult{.status = ParseStatus::ErrorBadRequest, .error_code = 400, .error_message = error_message_};
    }

    auto sp1 = req_line.find(' ');
    if (sp1 == std::string_view::npos) {
        state_ = ParserState::Error;
        error_code_ = 400;
        error_message_ = "Missing space after method";
        return ParseResult{.status = ParseStatus::ErrorBadRequest, .error_code = 400, .error_message = error_message_};
    }

    auto sp2 = req_line.find(' ', sp1 + 1);
    if (sp2 == std::string_view::npos) {
        state_ = ParserState::Error;
        error_code_ = 400;
        error_message_ = "Missing space after URI";
        return ParseResult{.status = ParseStatus::ErrorBadRequest, .error_code = 400, .error_message = error_message_};
    }

    // Verify no trailing extra tokens
    if (req_line.find(' ', sp2 + 1) != std::string_view::npos) {
        state_ = ParserState::Error;
        error_code_ = 400;
        error_message_ = "Extra spaces in request line";
        return ParseResult{.status = ParseStatus::ErrorBadRequest, .error_code = 400, .error_message = error_message_};
    }

    request_.method_str = req_line.substr(0, sp1);
    request_.uri = req_line.substr(sp1 + 1, sp2 - sp1 - 1);
    std::string_view version_str = req_line.substr(sp2 + 1);

    if (request_.method_str.empty()) {
        state_ = ParserState::Error;
        error_code_ = 400;
        error_message_ = "Empty HTTP method";
        return ParseResult{.status = ParseStatus::ErrorBadRequest, .error_code = 400, .error_message = error_message_};
    }

    for (char c : request_.method_str) {
        if (!IsTokenChar(c)) {
            state_ = ParserState::Error;
            error_code_ = 400;
            error_message_ = "Invalid token character in method";
            return ParseResult{.status = ParseStatus::ErrorBadRequest, .error_code = 400, .error_message = error_message_};
        }
    }
    request_.method = ParseMethod(request_.method_str);

    if (request_.uri.empty()) {
        state_ = ParserState::Error;
        error_code_ = 400;
        error_message_ = "Empty URI";
        return ParseResult{.status = ParseStatus::ErrorBadRequest, .error_code = 400, .error_message = error_message_};
    }

    // Validate URI characters (no spaces, control characters)
    for (char c : request_.uri) {
        auto uc = static_cast<unsigned char>(c);
        if (uc <= 0x20 || uc == 0x7F) {
            state_ = ParserState::Error;
            error_code_ = 400;
            error_message_ = "Invalid control character or space in URI";
            return ParseResult{.status = ParseStatus::ErrorBadRequest, .error_code = 400, .error_message = error_message_};
        }
    }

    auto qpos = request_.uri.find('?');
    if (qpos != std::string_view::npos) {
        request_.path = request_.uri.substr(0, qpos);
        request_.query = request_.uri.substr(qpos + 1);
    } else {
        request_.path = request_.uri;
        request_.query = {};
    }

    // Validate HTTP Version
    if (version_str == "HTTP/1.1") {
        request_.http_major = 1;
        request_.http_minor = 1;
        request_.keep_alive = true;
    } else if (version_str == "HTTP/1.0") {
        request_.http_major = 1;
        request_.http_minor = 0;
        request_.keep_alive = false;
    } else {
        state_ = ParserState::Error;
        error_code_ = 400;
        error_message_ = "Unsupported or invalid HTTP version";
        return ParseResult{.status = ParseStatus::ErrorBadRequest, .error_code = 400, .error_message = error_message_};
    }

    // Parse Headers
    size_t curr_pos = first_nl + 1;
    while (curr_pos < header_source.size()) {
        auto next_nl = header_source.find('\n', curr_pos);
        if (next_nl == std::string_view::npos) {
            break;
        }

        std::string_view line = header_source.substr(curr_pos, next_nl - curr_pos);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        curr_pos = next_nl + 1;

        if (line.empty()) {
            // End of headers block
            break;
        }

        if (line.find('\0') != std::string_view::npos) {
            state_ = ParserState::Error;
            error_code_ = 400;
            error_message_ = "Null byte in header field";
            return ParseResult{.status = ParseStatus::ErrorBadRequest, .error_code = 400, .error_message = error_message_};
        }

        // Multi-line header (obs-fold: line starts with SP or HTAB)
        if (line.front() == ' ' || line.front() == '\t') {
            if (request_.num_headers == 0) {
                state_ = ParserState::Error;
                error_code_ = 400;
                error_message_ = "Header continuation without preceding header (RFC 9112 §5.2)";
                return ParseResult{.status = ParseStatus::ErrorBadRequest, .error_code = 400, .error_message = error_message_};
            }

            // In RFC 9112 §5.2 obs-fold, fold into the previous header
            auto trimmed = TrimOws(line);
            auto& prev_hdr = request_.headers[request_.num_headers - 1];
            // Extend view to encompass continuation line
            const char* v_start = prev_hdr.value.data();
            const char* v_end = trimmed.data() + trimmed.size();
            if (v_end >= v_start) {
                prev_hdr.value = std::string_view(v_start, static_cast<size_t>(v_end - v_start));
            }
            continue;
        }

        auto colon = line.find(':');
        if (colon == std::string_view::npos) {
            state_ = ParserState::Error;
            error_code_ = 400;
            error_message_ = "Missing colon in header field line";
            return ParseResult{.status = ParseStatus::ErrorBadRequest, .error_code = 400, .error_message = error_message_};
        }

        // RFC 9112 §5.1: No whitespace is allowed between the header field-name and colon!
        std::string_view field_name = line.substr(0, colon);
        if (field_name.empty()) {
            state_ = ParserState::Error;
            error_code_ = 400;
            error_message_ = "Empty header field name";
            return ParseResult{.status = ParseStatus::ErrorBadRequest, .error_code = 400, .error_message = error_message_};
        }

        for (char c : field_name) {
            if (!IsTokenChar(c)) {
                state_ = ParserState::Error;
                error_code_ = 400;
                error_message_ = "Invalid whitespace or control character preceding header colon (RFC 9112 §5.1)";
                return ParseResult{.status = ParseStatus::ErrorBadRequest, .error_code = 400, .error_message = error_message_};
            }
        }

        std::string_view field_val = line.substr(colon + 1);
        field_val = TrimOws(field_val);

        if (request_.num_headers >= kDefaultMaxHeaders) {
            state_ = ParserState::Error;
            error_code_ = 431;
            error_message_ = "Header count exceeds maximum capacity";
            return ParseResult{.status = ParseStatus::ErrorHeaderTooLarge, .error_code = 431, .error_message = error_message_};
        }

        request_.headers[request_.num_headers++] = HttpHeader{field_name, field_val};
    }

    if (!ValidateHeadersRfc9112(result)) {
        return result;
    }

    // Determine bytes consumed from `data` for header block
    size_t consumed_from_data = 0;
    if (using_header_storage_) {
        // How many bytes from current `data` slice went into header block vs body?
        size_t prev_storage_len = header_storage_len_ - data.size();
        if (header_end_pos > prev_storage_len) {
            consumed_from_data = header_end_pos - prev_storage_len;
        } else {
            consumed_from_data = 0;
        }
    } else {
        consumed_from_data = header_end_pos;
    }

    result.bytes_consumed = consumed_from_data;

    // If more data remains in this slice, feed it to body parsing immediately
    if (consumed_from_data < data.size() && state_ != ParserState::Complete && state_ != ParserState::Error) {
        std::string_view body_rem = data.substr(consumed_from_data);
        ParseResult body_res{};
        if (state_ == ParserState::BodyIdentity) {
            body_res = ParseBodyIdentity(body_rem);
        } else if (state_ >= ParserState::ChunkSize && state_ <= ParserState::ChunkTrailers) {
            body_res = ParseChunked(body_rem);
        }
        result.bytes_consumed += body_res.bytes_consumed;
        result.body_chunk = body_res.body_chunk;
        result.status = body_res.status;
        result.error_code = body_res.error_code;
        result.error_message = body_res.error_message;
    } else {
        if (state_ == ParserState::Complete) {
            result.status = ParseStatus::Complete;
        } else {
            result.status = ParseStatus::HeadersComplete;
        }
    }

    return result;
}

bool HttpParser::ValidateHeadersRfc9112(ParseResult& result) {
    bool has_te = false;
    bool te_is_chunked = false;

    // 1. Validate Transfer-Encoding
    for (size_t i = 0; i < request_.num_headers; ++i) {
        if (CaseInsensitiveEqual(request_.headers[i].name, "Transfer-Encoding")) {
            has_te = true;
            std::string_view te_val = request_.headers[i].value;

            // Split comma-separated transfer codings
            std::string_view rem = te_val;
            std::string_view last_coding{};
            while (!rem.empty()) {
                auto comma = rem.find(',');
                std::string_view token = (comma != std::string_view::npos) ? rem.substr(0, comma) : rem;
                rem = (comma != std::string_view::npos) ? rem.substr(comma + 1) : std::string_view{};
                token = TrimOws(token);
                if (!token.empty()) {
                    last_coding = token;
                    // RFC 9112 §6.1: Reject unsupported/removed encodings like identity
                    if (CaseInsensitiveEqual(token, "identity")) {
                        state_ = ParserState::Error;
                        error_code_ = 400;
                        error_message_ = "Transfer-Encoding: identity is forbidden (RFC 9112 §6.1)";
                        result = ParseResult{.status = ParseStatus::ErrorBadRequest, .error_code = 400, .error_message = error_message_};
                        return false;
                    }
                }
            }

            // The final transfer coding MUST be chunked
            if (CaseInsensitiveEqual(last_coding, "chunked")) {
                te_is_chunked = true;
            } else {
                state_ = ParserState::Error;
                error_code_ = 400;
                error_message_ = "Final Transfer-Encoding must be chunked (RFC 9112 §6.1)";
                result = ParseResult{.status = ParseStatus::ErrorBadRequest, .error_code = 400, .error_message = error_message_};
                return false;
            }
        }
    }

    request_.is_chunked = te_is_chunked;

    // 2. Validate Content-Length
    bool has_cl = false;
    uint64_t cl_value = 0;

    for (size_t i = 0; i < request_.num_headers; ++i) {
        if (CaseInsensitiveEqual(request_.headers[i].name, "Content-Length")) {
            std::string_view cl_val = request_.headers[i].value;

            std::string_view rem = cl_val;
            while (!rem.empty()) {
                auto comma = rem.find(',');
                std::string_view token = (comma != std::string_view::npos) ? rem.substr(0, comma) : rem;
                rem = (comma != std::string_view::npos) ? rem.substr(comma + 1) : std::string_view{};
                token = TrimOws(token);

                if (token.empty()) {
                    state_ = ParserState::Error;
                    error_code_ = 400;
                    error_message_ = "Empty Content-Length value (RFC 9112 §5.2)";
                    result = ParseResult{.status = ParseStatus::ErrorBadRequest, .error_code = 400, .error_message = error_message_};
                    return false;
                }

                uint64_t parsed_num = 0;
                for (char c : token) {
                    if (c < '0' || c > '9') {
                        state_ = ParserState::Error;
                        error_code_ = 400;
                        error_message_ = "Invalid non-digit character in Content-Length (RFC 9110 §8.6)";
                        result = ParseResult{.status = ParseStatus::ErrorBadRequest, .error_code = 400, .error_message = error_message_};
                        return false;
                    }
                    if (parsed_num > (UINT64_MAX - static_cast<uint64_t>(c - '0')) / 10) {
                        state_ = ParserState::Error;
                        error_code_ = 400;
                        error_message_ = "Content-Length integer overflow";
                        result = ParseResult{.status = ParseStatus::ErrorBadRequest, .error_code = 400, .error_message = error_message_};
                        return false;
                    }
                    parsed_num = parsed_num * 10 + static_cast<uint64_t>(c - '0');
                }

                if (has_cl) {
                    if (parsed_num != cl_value) {
                        state_ = ParserState::Error;
                        error_code_ = 400;
                        error_message_ = "Conflicting Content-Length values detected (RFC 9112 §5.2 Request Smuggling Protection)";
                        result = ParseResult{.status = ParseStatus::ErrorBadRequest, .error_code = 400, .error_message = error_message_};
                        return false;
                    }
                } else {
                    has_cl = true;
                    cl_value = parsed_num;
                }
            }
        }
    }

    if (has_cl) {
        request_.has_content_length = true;
        request_.content_length = static_cast<size_t>(cl_value);
    }

    // 3. Strict RFC 9112 §6.1 / §6.2 Request Smuggling Protection:
    // Reject requests with BOTH Content-Length and Transfer-Encoding!
    if (has_te && has_cl) {
        state_ = ParserState::Error;
        error_code_ = 400;
        error_message_ = "Ambiguous framing: both Content-Length and Transfer-Encoding present (RFC 9112 §6.1 Request Smuggling Protection)";
        result = ParseResult{.status = ParseStatus::ErrorBadRequest, .error_code = 400, .error_message = error_message_};
        return false;
    }

    // 4. Connection Keep-Alive determination
    if (auto conn_hdr = request_.GetHeader("Connection"); conn_hdr.has_value()) {
        std::string_view conn_val = *conn_hdr;
        if (CaseInsensitiveEqual(conn_val, "close")) {
            request_.keep_alive = false;
        } else if (CaseInsensitiveEqual(conn_val, "keep-alive")) {
            request_.keep_alive = true;
        }
    }

    request_.headers_complete = true;

    // 5. Determine body parsing state
    if (request_.is_chunked) {
        state_ = ParserState::ChunkSize;
        current_chunk_size_ = 0;
        current_chunk_remaining_ = 0;
        crlf_bytes_read_ = 0;
        chunk_line_buf_len_ = 0;
    } else if (request_.has_content_length) {
        if (request_.content_length == 0) {
            state_ = ParserState::Complete;
            request_.is_complete = true;
        } else {
            state_ = ParserState::BodyIdentity;
            body_bytes_remaining_ = request_.content_length;
        }
    } else {
        // In HTTP/1.1 requests without Content-Length or Transfer-Encoding, body length is 0 (RFC 9112 §6)
        state_ = ParserState::Complete;
        request_.is_complete = true;
    }

    return true;
}

ParseResult HttpParser::ParseBodyIdentity(std::string_view data) {
    if (body_bytes_remaining_ == 0) {
        state_ = ParserState::Complete;
        request_.is_complete = true;
        return ParseResult{.status = ParseStatus::Complete, .bytes_consumed = 0};
    }

    size_t to_consume = std::min(data.size(), body_bytes_remaining_);
    std::string_view chunk = data.substr(0, to_consume);
    body_bytes_remaining_ -= to_consume;

    if (body_callback_) {
        body_callback_(chunk);
    }
    decoded_body_.append(chunk);

    ParseResult result{
        .status = (body_bytes_remaining_ == 0) ? ParseStatus::Complete : ParseStatus::BodyChunk,
        .bytes_consumed = to_consume,
        .body_chunk = chunk
    };

    if (body_bytes_remaining_ == 0) {
        state_ = ParserState::Complete;
        request_.is_complete = true;
        if (decoded_body_.size() == request_.content_length && !using_header_storage_) {
            request_.body = chunk; // Contiguous zero-copy slice
        }
    }

    return result;
}

ParseResult HttpParser::ParseChunked(std::string_view data) {
    size_t idx = 0;
    std::string_view last_body_chunk{};

    auto fail = [this](ParseStatus status, int code, std::string_view msg) -> ParseResult {
        state_ = ParserState::Error;
        error_code_ = code;
        error_message_ = msg;
        return ParseResult{
            .status = status,
            .bytes_consumed = 0,
            .error_code = code,
            .error_message = error_message_
        };
    };

    while (idx < data.size() && state_ != ParserState::Complete && state_ != ParserState::Error) {
        switch (state_) {
            case ParserState::ChunkSize: {
                char c = data[idx++];
                if (c == '\n') {
                    std::string_view line(chunk_line_buf_.data(), chunk_line_buf_len_);
                    if (!line.empty() && line.back() == '\r') {
                        line.remove_suffix(1);
                    }
                    chunk_line_buf_len_ = 0;

                    // Hex chunk size precedes optional ';' extension
                    auto semi = line.find(';');
                    std::string_view hex_str = (semi != std::string_view::npos) ? line.substr(0, semi) : line;
                    hex_str = TrimOws(hex_str);

                    if (hex_str.empty()) {
                        return fail(ParseStatus::ErrorBadRequest, 400, "Empty chunk size line");
                    }

                    uint64_t parsed_size = 0;
                    for (char h : hex_str) {
                        int digit = HexDigit(h);
                        if (digit < 0) {
                            return fail(ParseStatus::ErrorBadRequest, 400, "Invalid hex digit in chunk size");
                        }
                        if (parsed_size > (UINT64_MAX - static_cast<uint64_t>(digit)) / 16) {
                            return fail(ParseStatus::ErrorBadRequest, 400, "Chunk size integer overflow");
                        }
                        parsed_size = (parsed_size << 4) | static_cast<uint64_t>(digit);
                    }

                    current_chunk_size_ = static_cast<size_t>(parsed_size);
                    if (current_chunk_size_ > 0) {
                        current_chunk_remaining_ = current_chunk_size_;
                        state_ = ParserState::ChunkData;
                    } else {
                        // Terminal 0-length chunk
                        state_ = ParserState::ChunkTrailers;
                    }
                } else {
                    if (chunk_line_buf_len_ >= chunk_line_buf_.size()) {
                        return fail(ParseStatus::ErrorBadRequest, 400, "Chunk size line too long");
                    }
                    chunk_line_buf_[chunk_line_buf_len_++] = c;
                }
                break;
            }

            case ParserState::ChunkData: {
                size_t available = data.size() - idx;
                size_t to_consume = std::min(available, current_chunk_remaining_);
                std::string_view chunk = data.substr(idx, to_consume);
                idx += to_consume;
                current_chunk_remaining_ -= to_consume;

                if (body_callback_) {
                    body_callback_(chunk);
                }
                decoded_body_.append(chunk);
                last_body_chunk = chunk;

                if (current_chunk_remaining_ == 0) {
                    state_ = ParserState::ChunkCrlf;
                    crlf_bytes_read_ = 0;
                }
                break;
            }

            case ParserState::ChunkCrlf: {
                char c = data[idx++];
                if (crlf_bytes_read_ == 0) {
                    if (c == '\r') {
                        crlf_bytes_read_ = 1;
                    } else if (c == '\n') {
                        // Bare LF tolerance (RFC 9112 §2.2)
                        state_ = ParserState::ChunkSize;
                        crlf_bytes_read_ = 0;
                    } else {
                        return fail(ParseStatus::ErrorBadRequest, 400, "Malformed chunk: missing CRLF after chunk data");
                    }
                } else if (crlf_bytes_read_ == 1) {
                    if (c == '\n') {
                        state_ = ParserState::ChunkSize;
                        crlf_bytes_read_ = 0;
                    } else {
                        return fail(ParseStatus::ErrorBadRequest, 400, "Malformed chunk: expected LF after CR");
                    }
                }
                break;
            }

            case ParserState::ChunkTrailers: {
                char c = data[idx++];
                if (c == '\n') {
                    std::string_view line(chunk_line_buf_.data(), chunk_line_buf_len_);
                    if (!line.empty() && line.back() == '\r') {
                        line.remove_suffix(1);
                    }
                    chunk_line_buf_len_ = 0;

                    if (line.empty()) {
                        // Empty line terminates chunked stream
                        state_ = ParserState::Complete;
                        request_.is_complete = true;
                    }
                } else {
                    if (chunk_line_buf_len_ >= chunk_line_buf_.size()) {
                        return fail(ParseStatus::ErrorBadRequest, 400, "Trailer line too long");
                    }
                    chunk_line_buf_[chunk_line_buf_len_++] = c;
                }
                break;
            }

            default:
                break;
        }
    }

    ParseStatus final_status = ParseStatus::Incomplete;
    if (state_ == ParserState::Complete) {
        final_status = ParseStatus::Complete;
    } else if (!last_body_chunk.empty()) {
        final_status = ParseStatus::BodyChunk;
    }

    return ParseResult{
        .status = final_status,
        .bytes_consumed = idx,
        .body_chunk = last_body_chunk
    };
}

} // namespace helios
