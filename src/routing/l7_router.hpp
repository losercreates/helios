#pragma once

#include "l7_route.hpp"
#include <vector>
#include <string>
#include <string_view>
#include <optional>
#include <shared_mutex>
#include <mutex>

namespace helios {

// Evaluates HTTP request Host and Path against configured L7 routes in a thread-safe manner
class L7Router {
public:
    L7Router() = default;
    ~L7Router() = default;

    // Movable with mutex synchronization
    L7Router(L7Router&& other) noexcept {
        std::unique_lock lock(other.mutex_);
        routes_ = std::move(other.routes_);
        default_pool_ = std::move(other.default_pool_);
    }
    L7Router& operator=(L7Router&& other) noexcept {
        if (this != &other) {
            std::scoped_lock lock(mutex_, other.mutex_);
            routes_ = std::move(other.routes_);
            default_pool_ = std::move(other.default_pool_);
        }
        return *this;
    }

    // Non-copyable
    L7Router(const L7Router&) = delete;
    L7Router& operator=(const L7Router&) = delete;

    void AddRoute(L7RouteConfig route);
    void SetDefaultPool(std::string pool_name);
    [[nodiscard]] std::string GetDefaultPool() const;

    [[nodiscard]] std::vector<L7RouteConfig> GetRoutes() const;
    void ClearRoutes() noexcept;

    // Evaluates Host and Path. Returns RouteMatchResult if matched or default pool exists, or std::nullopt (404)
    [[nodiscard]] std::optional<RouteMatchResult> Match(std::string_view host, std::string_view path) const;

    // Static matching helpers
    static bool HostMatches(std::string_view pattern, std::string_view host) noexcept;
    static bool PathMatches(std::string_view prefix, std::string_view path) noexcept;
    static std::string StripPrefix(std::string_view prefix, std::string_view path);

private:
    mutable std::shared_mutex mutex_;
    std::vector<L7RouteConfig> routes_;
    std::string default_pool_;
};

} // namespace helios
