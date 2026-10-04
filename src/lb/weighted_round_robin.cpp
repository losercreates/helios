#include "weighted_round_robin.hpp"
#include <algorithm>
#include <utility>

namespace helios {

void WeightedRoundRobinLoadBalancer::AddBackend(std::shared_ptr<Backend> backend) {
    if (!backend) return;
    backends_.push_back(WRRState{std::move(backend), 0});
}

bool WeightedRoundRobinLoadBalancer::RemoveBackend(uint32_t backend_id) {
    auto it = std::find_if(backends_.begin(), backends_.end(),
                           [backend_id](const WRRState& state) { return state.backend->GetId() == backend_id; });
    if (it != backends_.end()) {
        backends_.erase(it);
        return true;
    }
    return false;
}

std::shared_ptr<Backend> WeightedRoundRobinLoadBalancer::SelectBackend() noexcept {
    if (backends_.empty()) {
        return nullptr;
    }

    int total_weight = 0;
    WRRState* best = nullptr;

    for (auto& state : backends_) {
        auto& b = state.backend;
        if (!b->IsEligible() || b->IsDraining()) {
            continue;
        }

        int weight = static_cast<int>(b->GetWeight());
        if (weight <= 0) {
            continue;
        }

        state.current_weight += weight;
        total_weight += weight;

        if (best == nullptr || state.current_weight > best->current_weight) {
            best = &state;
        }
    }

    if (best == nullptr) {
        return nullptr;
    }

    best->current_weight -= total_weight;
    return best->backend;
}

std::vector<std::shared_ptr<Backend>> WeightedRoundRobinLoadBalancer::GetAllBackends() const {
    std::vector<std::shared_ptr<Backend>> list;
    list.reserve(backends_.size());
    for (const auto& state : backends_) {
        list.push_back(state.backend);
    }
    return list;
}

size_t WeightedRoundRobinLoadBalancer::GetBackendCount() const noexcept {
    return backends_.size();
}

bool WeightedRoundRobinLoadBalancer::IsEmpty() const noexcept {
    return backends_.empty();
}

void WeightedRoundRobinLoadBalancer::ClearBackends() noexcept {
    backends_.clear();
}

} // namespace helios
