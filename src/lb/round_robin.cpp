#include "round_robin.hpp"
#include <algorithm>
#include <utility>

namespace helios {

void RoundRobinLoadBalancer::AddBackend(std::shared_ptr<Backend> backend) {
    if (!backend) return;
    backends_.push_back(std::move(backend));
}

bool RoundRobinLoadBalancer::RemoveBackend(uint32_t backend_id) {
    auto it = std::find_if(backends_.begin(), backends_.end(),
                           [backend_id](const std::shared_ptr<Backend>& b) { return b->GetId() == backend_id; });
    if (it != backends_.end()) {
        backends_.erase(it);
        // Ensure next_index_ stays within bounds
        if (backends_.empty()) {
            next_index_ = 0;
        } else {
            next_index_ = next_index_ % backends_.size();
        }
        return true;
    }
    return false;
}

std::shared_ptr<Backend> RoundRobinLoadBalancer::SelectBackend() noexcept {
    if (backends_.empty()) {
        return nullptr;
    }

    size_t count = backends_.size();
    for (size_t i = 0; i < count; ++i) {
        size_t idx = (next_index_ + i) % count;
        auto& backend = backends_[idx];
        if (backend->IsEligible() && !backend->IsDraining()) {
            next_index_ = (idx + 1) % count;
            return backend;
        }
    }

    return nullptr;
}

std::vector<std::shared_ptr<Backend>> RoundRobinLoadBalancer::GetAllBackends() const {
    return backends_;
}

size_t RoundRobinLoadBalancer::GetBackendCount() const noexcept {
    return backends_.size();
}

bool RoundRobinLoadBalancer::IsEmpty() const noexcept {
    return backends_.empty();
}

void RoundRobinLoadBalancer::ClearBackends() noexcept {
    backends_.clear();
    next_index_ = 0;
}

} // namespace helios
