#pragma once

#include <string>
#include <string_view>
#include <optional>
#include <vector>

namespace helios {

// Configuration for an individual L7 HTTP route rule
struct L7RouteConfig {
    std::string name;
    std::string host;               // Exact e.g. "api.example.com", wildcard "*.example.com", or "*" / "" (any host)
    std::string path_prefix{"/"};   // URL path prefix to match (e.g. "/api", "/static", "/")
    std::string target_pool;        // Target backend pool name (e.g. "api_pool")
    bool strip_prefix{false};       // Strip path_prefix before forwarding upstream (e.g. /api/users -> /users)
    bool inject_headers{true};      // Inject X-Forwarded-For, X-Forwarded-Proto, X-Forwarded-Host, X-Request-ID
};

// Result of evaluating a request against configured L7 routes
struct RouteMatchResult {
    const L7RouteConfig* route{nullptr};
    std::string target_pool;
    std::string rewritten_path;
    bool matched{false};
    bool strip_prefix{false};
    bool inject_headers{true};
};

} // namespace helios
