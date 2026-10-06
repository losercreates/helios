#pragma once

#include "load_balancer.hpp"
#include <vector>
#include <memory>

namespace helios {

// Least-Connections Load Balancer.
// Selects the eligible backend with the fewest active connections.
// Ties are broken deterministically by selecting the backend with the lowest ID.
class LeastConnectionsLoadBalancer : public LoadBalancer {
public:
    LeastConnectionsLoadBalancer() = default;
    ~LeastConnectionsLoadBalancer() override = default;

    // Backend Membership Management
    void AddBackend(std::shared_ptr<Backend> backend) override;
    bool RemoveBackend(uint32_t backend_id) override;

    // Select the eligible backend with the smallest active connection count
    [[nodiscard]] std::shared_ptr<Backend> SelectBackend() noexcept override;

    // State Queries
    [[nodiscard]] std::vector<std::shared_ptr<Backend>> GetAllBackends() const override;
    [[nodiscard]] size_t GetBackendCount() const noexcept override;
    [[nodiscard]] bool IsEmpty() const noexcept override;
    
    // Clear backends
    void ClearBackends() noexcept override;

private:
    std::vector<std::shared_ptr<Backend>> backends_;
};

} // namespace helios
