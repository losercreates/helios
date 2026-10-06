#include "least_connections.hpp"
#include <algorithm>
#include <cstdint>

namespace helios {

void LeastConnectionsLoadBalancer::AddBackend(std::shared_ptr<Backend> backend) {
    if (!backend) return;
    backends_.push_back(std::move(backend));
}

bool LeastConnectionsLoadBalancer::RemoveBackend(uint32_t backend_id) {
    auto it = std::find_if(backends_.begin(), backends_.end(),
                           [backend_id](const std::shared_ptr<Backend>& b) { return b->GetId() == backend_id; });
    if (it != backends_.end()) {
        backends_.erase(it);
        return true;
    }
    return false;
}

std::shared_ptr<Backend> LeastConnectionsLoadBalancer::SelectBackend() noexcept {
    if (backends_.empty()) {
        return nullptr;
    }

    std::shared_ptr<Backend> best = nullptr;
    uint32_t min_connections = UINT32_MAX;

    for (const auto& backend : backends_) {
        if (!backend->IsEligible() || backend->IsDraining()) {
            continue;
        }

        uint32_t active_conns = backend->GetActiveConnections();
        if (active_conns < min_connections) {
            min_connections = active_conns;
            best = backend;
        } else if (active_conns == min_connections && best != nullptr) {
            // Deterministic tie-breaking: prefer lowest ID
            if (backend->GetId() < best->GetId()) {
                best = backend;
            }
        }
    }

    return best;
}

std::vector<std::shared_ptr<Backend>> LeastConnectionsLoadBalancer::GetAllBackends() const {
    return backends_;
}

size_t LeastConnectionsLoadBalancer::GetBackendCount() const noexcept {
    return backends_.size();
}

bool LeastConnectionsLoadBalancer::IsEmpty() const noexcept {
    return backends_.empty();
}

void LeastConnectionsLoadBalancer::ClearBackends() noexcept {
    backends_.clear();
}

} // namespace helios
