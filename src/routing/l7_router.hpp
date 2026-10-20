#pragma once

#include "l7_route.hpp"
#include <vector>
#include <string>
#include <string_view>
#include <optional>

namespace helios {

// Evaluates HTTP request Host and Path against configured L7 routes
class L7Router {
public:
    L7Router() = default;
    ~L7Router() = default;

    void AddRoute(L7RouteConfig route);
    void SetDefaultPool(std::string pool_name) { default_pool_ = std::move(pool_name); }
    [[nodiscard]] const std::string& GetDefaultPool() const noexcept { return default_pool_; }

    [[nodiscard]] const std::vector<L7RouteConfig>& GetRoutes() const noexcept { return routes_; }
    void ClearRoutes() noexcept { routes_.clear(); }

    // Evaluates Host and Path. Returns RouteMatchResult if matched or default pool exists, or std::nullopt (404)
    [[nodiscard]] std::optional<RouteMatchResult> Match(std::string_view host, std::string_view path) const;

    // Static matching helpers
    static bool HostMatches(std::string_view pattern, std::string_view host) noexcept;
    static bool PathMatches(std::string_view prefix, std::string_view path) noexcept;
    static std::string StripPrefix(std::string_view prefix, std::string_view path);

private:
    std::vector<L7RouteConfig> routes_;
    std::string default_pool_;
};

} // namespace helios
