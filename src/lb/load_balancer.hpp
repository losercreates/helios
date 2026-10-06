#pragma once

#include "backend.hpp"
#include <memory>
#include <vector>

namespace helios {

class LoadBalancer {
public:
    virtual ~LoadBalancer() = default;

    // Backend Membership Management
    virtual void AddBackend(std::shared_ptr<Backend> backend) = 0;
    virtual bool RemoveBackend(uint32_t backend_id) = 0;
    
    // Core selection mechanism for hot-path usage.
    // Must return nullptr if no backends are eligible.
    [[nodiscard]] virtual std::shared_ptr<Backend> SelectBackend() noexcept = 0;
    
    // Key-based selection mechanism for session affinity / consistent hashing.
    // By default, this falls back to the stateless selection algorithm.
    [[nodiscard]] virtual std::shared_ptr<Backend> SelectBackendByKey(uint64_t hash_key) noexcept {
        (void)hash_key;
        return SelectBackend();
    }
    
    // State Queries
    [[nodiscard]] virtual std::vector<std::shared_ptr<Backend>> GetAllBackends() const = 0;
    [[nodiscard]] virtual size_t GetBackendCount() const noexcept = 0;
    [[nodiscard]] virtual bool IsEmpty() const noexcept = 0;
    virtual void ClearBackends() noexcept = 0;
};

} // namespace helios
