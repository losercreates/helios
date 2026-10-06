#pragma once

#include "load_balancer.hpp"
#include <vector>
#include <memory>
#include <map>
#include <string>

namespace helios {

// Consistent Hashing Load Balancer.
// Maps a stable hash key to a backend using a hash ring with virtual nodes.
class ConsistentHashLoadBalancer : public LoadBalancer {
public:
    explicit ConsistentHashLoadBalancer(size_t virtual_nodes = 100);
    ~ConsistentHashLoadBalancer() override = default;

    // Backend Membership Management
    void AddBackend(std::shared_ptr<Backend> backend) override;
    bool RemoveBackend(uint32_t backend_id) override;

    // Fallback for selection without a key (round-robin)
    [[nodiscard]] std::shared_ptr<Backend> SelectBackend() noexcept override;
    
    // Core selection using caller-provided stable key
    [[nodiscard]] std::shared_ptr<Backend> SelectBackendByKey(uint64_t hash_key) noexcept override;

    // State Queries
    [[nodiscard]] std::vector<std::shared_ptr<Backend>> GetAllBackends() const override;
    [[nodiscard]] size_t GetBackendCount() const noexcept override;
    [[nodiscard]] bool IsEmpty() const noexcept override;
    
    // Clear backends
    void ClearBackends() noexcept override;

private:
    void RebuildRing() noexcept;
    uint64_t HashString(const std::string& input) const noexcept;

    size_t virtual_nodes_;
    std::vector<std::shared_ptr<Backend>> backends_;
    
    // The consistent hash ring
    std::map<uint64_t, std::shared_ptr<Backend>> ring_;
    bool ring_dirty_{false};
    
    // For parameterless fallback
    size_t rr_index_{0};
};

} // namespace helios
