#pragma once

#include "load_balancer.hpp"
#include <vector>
#include <memory>

namespace helios {

// Smooth Weighted Round-Robin Load Balancer.
// Implements NGINX-style smooth weighted round-robin distribution to prevent
// long streaks of consecutive requests to the same highly-weighted backend.
class WeightedRoundRobinLoadBalancer : public LoadBalancer {
public:
    WeightedRoundRobinLoadBalancer() = default;
    ~WeightedRoundRobinLoadBalancer() override = default;

    // Backend Membership Management
    void AddBackend(std::shared_ptr<Backend> backend) override;
    bool RemoveBackend(uint32_t backend_id) override;

    // Select the next active backend endpoint via Smooth WRR
    [[nodiscard]] std::shared_ptr<Backend> SelectBackend() noexcept override;

    // State Queries
    [[nodiscard]] std::vector<std::shared_ptr<Backend>> GetAllBackends() const override;
    [[nodiscard]] size_t GetBackendCount() const noexcept override;
    [[nodiscard]] bool IsEmpty() const noexcept override;
    
    // Clear backends
    void ClearBackends() noexcept override;

private:
    struct WRRState {
        std::shared_ptr<Backend> backend;
        int current_weight{0};
    };

    std::vector<WRRState> backends_;
};

} // namespace helios
