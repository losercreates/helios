#pragma once

#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include <cstddef>
#include <netinet/in.h>

namespace helios {

struct BackendEndpoint {
    std::string host;
    uint16_t port{0};
    bool active{true};
};

// Round-Robin Load Balancer selecting backend endpoints sequentially
class RoundRobinLoadBalancer {
public:
    RoundRobinLoadBalancer() = default;
    explicit RoundRobinLoadBalancer(std::vector<BackendEndpoint> backends);
    ~RoundRobinLoadBalancer() = default;

    // Add a backend endpoint
    void AddBackend(std::string host, uint16_t port);

    // Select the next active backend endpoint via Round-Robin
    [[nodiscard]] std::optional<BackendEndpoint> SelectBackend();

    // Select and parse into sockaddr_in
    [[nodiscard]] bool SelectBackendAddr(sockaddr_in* out_addr, BackendEndpoint* out_endpoint = nullptr);

    // Status queries
    [[nodiscard]] size_t GetBackendCount() const noexcept { return backends_.size(); }
    [[nodiscard]] bool IsEmpty() const noexcept { return backends_.empty(); }

    // Clear backends
    void ClearBackends() noexcept;

private:
    std::vector<BackendEndpoint> backends_;
    size_t next_index_{0};
};

} // namespace helios
