#pragma once

#include <string>
#include <string_view>
#include <optional>
#include "security/security_config.hpp"

namespace helios {

// Options for HTTP request proxy header injection and path rewriting
struct HeaderInjectionOptions {
    std::string client_ip{"127.0.0.1"};
    std::string client_proto{"http"};
    std::string host;
    std::string request_id;
    std::optional<std::string> rewritten_path;
    bool inject_forwarded_for{true};
    bool inject_forwarded_proto{true};
    bool inject_forwarded_host{true};
    bool inject_request_id{true};
};

class HttpRewriter {
public:
    // Generate a unique 32-character hexadecimal request ID
    static std::string GenerateRequestId();

    // Rewrites request line if rewritten_path is provided, and injects X-Forwarded-* headers
    static std::string RewriteRequest(std::string_view raw_request,
                                      const HeaderInjectionOptions& options);

    // Injects/rewrites downstream HTTP response security headers (HSTS, X-Content-Type-Options,
    // X-Frame-Options, Server, custom headers)
    static std::string RewriteResponse(std::string_view raw_response,
                                       const ResponseHeadersConfig& config,
                                       bool is_tls = false);
};

} // namespace helios
