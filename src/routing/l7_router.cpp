#include "l7_router.hpp"
#include <algorithm>
#include <cctype>

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

std::string_view StripPort(std::string_view host) noexcept {
    auto colon = host.find(':');
    if (colon != std::string_view::npos) {
        return host.substr(0, colon);
    }
    return host;
}

} // namespace

void L7Router::AddRoute(L7RouteConfig route) {
    std::unique_lock lock(mutex_);
    routes_.push_back(std::move(route));
}

void L7Router::SetDefaultPool(std::string pool_name) {
    std::unique_lock lock(mutex_);
    default_pool_ = std::move(pool_name);
}

std::string L7Router::GetDefaultPool() const {
    std::shared_lock lock(mutex_);
    return default_pool_;
}

std::vector<L7RouteConfig> L7Router::GetRoutes() const {
    std::shared_lock lock(mutex_);
    return routes_;
}

void L7Router::ClearRoutes() noexcept {
    std::unique_lock lock(mutex_);
    routes_.clear();
}

bool L7Router::HostMatches(std::string_view pattern, std::string_view host) noexcept {
    pattern = StripPort(pattern);
    host = StripPort(host);

    if (pattern.empty() || pattern == "*") {
        return true;
    }

    if (pattern.starts_with("*.")) {
        std::string_view suffix = pattern.substr(1); // includes leading '.'
        if (host.size() > suffix.size()) {
            std::string_view host_suffix = host.substr(host.size() - suffix.size());
            return CaseInsensitiveEqual(suffix, host_suffix);
        }
        return false;
    }

    return CaseInsensitiveEqual(pattern, host);
}

bool L7Router::PathMatches(std::string_view prefix, std::string_view path) noexcept {
    if (prefix.empty() || prefix == "/") {
        return true;
    }

    // Separate query parameters before evaluating path prefix
    auto query_pos = path.find('?');
    std::string_view path_only = (query_pos != std::string_view::npos) ? path.substr(0, query_pos) : path;

    if (prefix.ends_with('/')) {
        return path_only.starts_with(prefix);
    }

    if (path_only == prefix) {
        return true;
    }

    if (path_only.starts_with(prefix)) {
        return path_only[prefix.size()] == '/';
    }

    return false;
}

std::string L7Router::StripPrefix(std::string_view prefix, std::string_view path) {
    if (prefix.empty() || prefix == "/") {
        return std::string(path);
    }

    auto query_pos = path.find('?');
    std::string_view path_only = (query_pos != std::string_view::npos) ? path.substr(0, query_pos) : path;
    std::string_view query_part = (query_pos != std::string_view::npos) ? path.substr(query_pos) : "";

    std::string_view stripped = path_only;
    if (path_only.starts_with(prefix)) {
        stripped = path_only.substr(prefix.size());
    }

    std::string result;
    result.reserve(stripped.size() + query_part.size() + 1);

    if (stripped.empty() || stripped.front() != '/') {
        result.push_back('/');
    }
    result.append(stripped);
    result.append(query_part);

    return result;
}

std::optional<RouteMatchResult> L7Router::Match(std::string_view host, std::string_view path) const {
    std::shared_lock lock(mutex_);
    const L7RouteConfig* best_route = nullptr;
    int best_host_tier = 0; // 3 = exact, 2 = wildcard (*.), 1 = catch-all (* / empty)
    size_t best_path_len = 0;

    for (const auto& route : routes_) {
        int host_tier = 0;
        if (route.host.empty() || route.host == "*") {
            host_tier = 1;
        } else if (route.host.starts_with("*.")) {
            host_tier = 2;
        } else {
            host_tier = 3;
        }

        if (!HostMatches(route.host, host)) {
            continue;
        }

        if (!PathMatches(route.path_prefix, path)) {
            continue;
        }

        // Higher host tier wins; within same tier, longer path prefix wins
        if (host_tier > best_host_tier || 
           (host_tier == best_host_tier && route.path_prefix.size() > best_path_len)) {
            best_route = &route;
            best_host_tier = host_tier;
            best_path_len = route.path_prefix.size();
        }
    }

    if (best_route) {
        RouteMatchResult res;
        res.route = best_route;
        res.target_pool = best_route->target_pool;
        res.matched = true;
        res.strip_prefix = best_route->strip_prefix;
        res.inject_headers = best_route->inject_headers;
        if (best_route->strip_prefix) {
            res.rewritten_path = StripPrefix(best_route->path_prefix, path);
        } else {
            res.rewritten_path = std::string(path);
        }
        return res;
    }

    // Unmatched: fallback to default pool if configured
    if (!default_pool_.empty()) {
        RouteMatchResult res;
        res.route = nullptr;
        res.target_pool = default_pool_;
        res.rewritten_path = std::string(path);
        res.matched = true;
        res.strip_prefix = false;
        res.inject_headers = true;
        return res;
    }

    // No match and no default pool -> 404 Not Found
    return std::nullopt;
}

} // namespace helios
