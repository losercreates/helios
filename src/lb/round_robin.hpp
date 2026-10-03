#pragma once

#include "load_balancer.hpp"
#include <vector>
#include <memory>

namespace helios {

// Round-Robin Load Balancer selecting eligible backend endpoints sequentially
class RoundRobinLoadBalancer : public LoadBalancer {
public:
    RoundRobinLoadBalancer() = default;
    ~RoundRobinLoadBalancer() override = default;

    // Backend Membership Management
    void AddBackend(std::shared_ptr<Backend> backend) override;
    bool RemoveBackend(uint32_t backend_id) override;

    // Select the next active backend endpoint via Round-Robin
    [[nodiscard]] std::shared_ptr<Backend> SelectBackend() noexcept override;

    // State Queries
    [[nodiscard]] std::vector<std::shared_ptr<Backend>> GetAllBackends() const override;
    [[nodiscard]] size_t GetBackendCount() const noexcept override;
    [[nodiscard]] bool IsEmpty() const noexcept override;
    
    // Clear backends
    void ClearBackends() noexcept override;

private:
    std::vector<std::shared_ptr<Backend>> backends_;
    size_t next_index_{0};
};

} // namespace helios
