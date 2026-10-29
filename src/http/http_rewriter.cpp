#include "http_rewriter.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <random>
#include <sstream>
#include <iomanip>

namespace helios {

namespace {

bool CaseInsensitiveEqual(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::string_view TrimWhitespace(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

} // namespace

std::string HttpRewriter::GenerateRequestId() {
    static thread_local std::mt19937_64 rng(std::random_device{}());
    static thread_local std::uniform_int_distribution<uint64_t> dist;

    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    uint64_t r = dist(rng);

    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(16) << static_cast<uint64_t>(now)
        << std::setw(16) << r;
    return oss.str();
}

std::string HttpRewriter::RewriteRequest(std::string_view raw_request,
                                        const HeaderInjectionOptions& options) {
    auto header_end = raw_request.find("\r\n\r\n");
    if (header_end == std::string_view::npos) {
        // Fallback for single newline termination
        header_end = raw_request.find("\n\n");
        if (header_end == std::string_view::npos) {
            return std::string(raw_request);
        }
    }

    size_t delim_len = (raw_request.substr(header_end, 4) == "\r\n\r\n") ? 4 : 2;
    std::string_view headers_part = raw_request.substr(0, header_end);
    std::string_view body_part = raw_request.substr(header_end + delim_len);

    std::string out;
    out.reserve(raw_request.size() + 256);

    // 1. Process Request Line (first line)
    auto first_crlf = headers_part.find("\r\n");
    size_t line_delim_len = 2;
    if (first_crlf == std::string_view::npos) {
        first_crlf = headers_part.find('\n');
        line_delim_len = 1;
    }

    if (first_crlf == std::string_view::npos) {
        return std::string(raw_request);
    }

    std::string_view request_line = headers_part.substr(0, first_crlf);
    if (options.rewritten_path.has_value() && !options.rewritten_path->empty()) {
        // Parse Method, Path, Version
        auto sp1 = request_line.find(' ');
        if (sp1 != std::string_view::npos) {
            auto sp2 = request_line.find(' ', sp1 + 1);
            if (sp2 != std::string_view::npos) {
                std::string_view method = request_line.substr(0, sp1);
                std::string_view version = request_line.substr(sp2 + 1);
                out.append(method);
                out.push_back(' ');
                out.append(*options.rewritten_path);
                out.push_back(' ');
                out.append(version);
                out.append("\r\n");
            } else {
                out.append(request_line);
                out.append("\r\n");
            }
        } else {
            out.append(request_line);
            out.append("\r\n");
        }
    } else {
        out.append(request_line);
        out.append("\r\n");
    }

    // 2. Process Existing Headers
    bool seen_forwarded_for = false;
    bool seen_forwarded_proto = false;
    bool seen_forwarded_host = false;
    bool seen_request_id = false;
    std::string extracted_host = options.host;

    size_t cur = first_crlf + line_delim_len;
    while (cur < headers_part.size()) {
        auto next_crlf = headers_part.find("\r\n", cur);
        size_t next_delim = 2;
        if (next_crlf == std::string_view::npos) {
            next_crlf = headers_part.find('\n', cur);
            next_delim = 1;
        }

        std::string_view line = (next_crlf != std::string_view::npos)
                                ? headers_part.substr(cur, next_crlf - cur)
                                : headers_part.substr(cur);

        if (line.empty()) {
            break;
        }

        auto colon = line.find(':');
        if (colon != std::string_view::npos) {
            std::string_view name = TrimWhitespace(line.substr(0, colon));
            std::string_view val = TrimWhitespace(line.substr(colon + 1));

            if (CaseInsensitiveEqual(name, "x-forwarded-for")) {
                seen_forwarded_for = true;
                if (options.inject_forwarded_for && !options.client_ip.empty()) {
                    out.append("X-Forwarded-For: ");
                    out.append(val);
                    out.append(", ");
                    out.append(options.client_ip);
                    out.append("\r\n");
                } else {
                    out.append(line);
                    out.append("\r\n");
                }
            } else if (CaseInsensitiveEqual(name, "x-forwarded-proto")) {
                seen_forwarded_proto = true;
                out.append(line);
                out.append("\r\n");
            } else if (CaseInsensitiveEqual(name, "x-forwarded-host")) {
                seen_forwarded_host = true;
                out.append(line);
                out.append("\r\n");
            } else if (CaseInsensitiveEqual(name, "x-request-id")) {
                seen_request_id = true;
                out.append(line);
                out.append("\r\n");
            } else {
                if (CaseInsensitiveEqual(name, "host") && extracted_host.empty()) {
                    extracted_host = std::string(val);
                }
                out.append(line);
                out.append("\r\n");
            }
        } else {
            out.append(line);
            out.append("\r\n");
        }

        if (next_crlf == std::string_view::npos) break;
        cur = next_crlf + next_delim;
    }

    // 3. Inject Missing Headers
    if (!seen_forwarded_for && options.inject_forwarded_for && !options.client_ip.empty()) {
        out.append("X-Forwarded-For: ");
        out.append(options.client_ip);
        out.append("\r\n");
    }

    if (!seen_forwarded_proto && options.inject_forwarded_proto && !options.client_proto.empty()) {
        out.append("X-Forwarded-Proto: ");
        out.append(options.client_proto);
        out.append("\r\n");
    }

    if (!seen_forwarded_host && options.inject_forwarded_host && !extracted_host.empty()) {
        out.append("X-Forwarded-Host: ");
        out.append(extracted_host);
        out.append("\r\n");
    }

    if (!seen_request_id && options.inject_request_id) {
        std::string req_id = options.request_id.empty() ? GenerateRequestId() : options.request_id;
        out.append("X-Request-ID: ");
        out.append(req_id);
        out.append("\r\n");
    }

    // 4. Terminate Headers and Append Body
    out.append("\r\n");
    out.append(body_part);

    return out;
}

std::string HttpRewriter::RewriteResponse(std::string_view raw_response,
                                          const ResponseHeadersConfig& config,
                                          bool is_tls) {
    if (!config.enabled) {
        return std::string(raw_response);
    }

    if (raw_response.rfind("HTTP/", 0) != 0) {
        return std::string(raw_response);
    }

    auto header_end = raw_response.find("\r\n\r\n");
    size_t delim_len = 4;
    if (header_end == std::string_view::npos) {
        header_end = raw_response.find("\n\n");
        delim_len = 2;
        if (header_end == std::string_view::npos) {
            return std::string(raw_response);
        }
    }

    std::string_view headers_part = raw_response.substr(0, header_end);
    std::string_view body_part = raw_response.substr(header_end + delim_len);

    auto first_crlf = headers_part.find("\r\n");
    size_t line_delim_len = 2;
    if (first_crlf == std::string_view::npos) {
        first_crlf = headers_part.find('\n');
        line_delim_len = 1;
    }
    if (first_crlf == std::string_view::npos) {
        return std::string(raw_response);
    }

    std::string_view status_line = headers_part.substr(0, first_crlf);
    std::string out;
    out.reserve(raw_response.size() + 256);

    out.append(status_line);
    out.append("\r\n");

    bool seen_server = false;
    bool seen_hsts = false;
    bool seen_content_type_options = false;
    bool seen_frame_options = false;
    std::vector<bool> seen_custom(config.custom_headers.size(), false);

    size_t cur = first_crlf + line_delim_len;
    while (cur < headers_part.size()) {
        auto next_crlf = headers_part.find("\r\n", cur);
        size_t next_delim = 2;
        if (next_crlf == std::string_view::npos) {
            next_crlf = headers_part.find('\n', cur);
            next_delim = 1;
        }

        std::string_view line = (next_crlf != std::string_view::npos)
                                ? headers_part.substr(cur, next_crlf - cur)
                                : headers_part.substr(cur);

        if (line.empty()) break;

        auto colon = line.find(':');
        if (colon != std::string_view::npos) {
            std::string_view name = TrimWhitespace(line.substr(0, colon));

            if (CaseInsensitiveEqual(name, "server")) {
                seen_server = true;
                if (config.set_server_header && !config.server_value.empty()) {
                    out.append("Server: ");
                    out.append(config.server_value);
                    out.append("\r\n");
                } else {
                    out.append(line);
                    out.append("\r\n");
                }
            } else if (CaseInsensitiveEqual(name, "strict-transport-security")) {
                seen_hsts = true;
                if (config.hsts_enabled && is_tls && !config.hsts_value.empty()) {
                    out.append("Strict-Transport-Security: ");
                    out.append(config.hsts_value);
                    out.append("\r\n");
                } else {
                    out.append(line);
                    out.append("\r\n");
                }
            } else if (CaseInsensitiveEqual(name, "x-content-type-options")) {
                seen_content_type_options = true;
                if (config.x_content_type_options) {
                    out.append("X-Content-Type-Options: nosniff\r\n");
                } else {
                    out.append(line);
                    out.append("\r\n");
                }
            } else if (CaseInsensitiveEqual(name, "x-frame-options")) {
                seen_frame_options = true;
                if (!config.x_frame_options.empty()) {
                    out.append("X-Frame-Options: ");
                    out.append(config.x_frame_options);
                    out.append("\r\n");
                } else {
                    out.append(line);
                    out.append("\r\n");
                }
            } else {
                bool matched_custom = false;
                for (size_t i = 0; i < config.custom_headers.size(); ++i) {
                    if (CaseInsensitiveEqual(name, config.custom_headers[i].first)) {
                        seen_custom[i] = true;
                        matched_custom = true;
                        out.append(config.custom_headers[i].first);
                        out.append(": ");
                        out.append(config.custom_headers[i].second);
                        out.append("\r\n");
                        break;
                    }
                }
                if (!matched_custom) {
                    out.append(line);
                    out.append("\r\n");
                }
            }
        } else {
            out.append(line);
            out.append("\r\n");
        }

        if (next_crlf == std::string_view::npos) break;
        cur = next_crlf + next_delim;
    }

    // Inject missing configured headers
    if (!seen_server && config.set_server_header && !config.server_value.empty()) {
        out.append("Server: ");
        out.append(config.server_value);
        out.append("\r\n");
    }

    if (!seen_hsts && config.hsts_enabled && is_tls && !config.hsts_value.empty()) {
        out.append("Strict-Transport-Security: ");
        out.append(config.hsts_value);
        out.append("\r\n");
    }

    if (!seen_content_type_options && config.x_content_type_options) {
        out.append("X-Content-Type-Options: nosniff\r\n");
    }

    if (!seen_frame_options && !config.x_frame_options.empty()) {
        out.append("X-Frame-Options: ");
        out.append(config.x_frame_options);
        out.append("\r\n");
    }

    for (size_t i = 0; i < config.custom_headers.size(); ++i) {
        if (!seen_custom[i] && !config.custom_headers[i].first.empty()) {
            out.append(config.custom_headers[i].first);
            out.append(": ");
            out.append(config.custom_headers[i].second);
            out.append("\r\n");
        }
    }

    out.append("\r\n");
    out.append(body_part);
    return out;
}

} // namespace helios
