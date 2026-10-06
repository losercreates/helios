#include "consistent_hash.hpp"
#include <algorithm>

namespace helios {

ConsistentHashLoadBalancer::ConsistentHashLoadBalancer(size_t virtual_nodes) 
    : virtual_nodes_(virtual_nodes > 0 ? virtual_nodes : 1) {}

void ConsistentHashLoadBalancer::AddBackend(std::shared_ptr<Backend> backend) {
    if (!backend) return;
    backends_.push_back(std::move(backend));
    ring_dirty_ = true;
}

bool ConsistentHashLoadBalancer::RemoveBackend(uint32_t backend_id) {
    auto it = std::find_if(backends_.begin(), backends_.end(),
                           [backend_id](const std::shared_ptr<Backend>& b) { return b->GetId() == backend_id; });
    if (it != backends_.end()) {
        backends_.erase(it);
        ring_dirty_ = true;
        return true;
    }
    return false;
}

std::shared_ptr<Backend> ConsistentHashLoadBalancer::SelectBackend() noexcept {
    // Fallback to basic round-robin if no key is provided
    if (backends_.empty()) {
        return nullptr;
    }
    
    size_t count = backends_.size();
    for (size_t i = 0; i < count; ++i) {
        size_t idx = (rr_index_ + i) % count;
        auto& backend = backends_[idx];
        if (backend->IsEligible() && !backend->IsDraining()) {
            rr_index_ = (idx + 1) % count;
            return backend;
        }
    }
    return nullptr;
}

std::shared_ptr<Backend> ConsistentHashLoadBalancer::SelectBackendByKey(uint64_t hash_key) noexcept {
    if (ring_dirty_) {
        RebuildRing();
    }

    if (ring_.empty()) {
        return nullptr;
    }

    auto it = ring_.lower_bound(hash_key);
    if (it == ring_.end()) {
        it = ring_.begin();
    }

    // Traverse the ring clockwise to find the first eligible backend
    size_t checked_vnodes = 0;
    while (checked_vnodes < ring_.size()) {
        auto backend = it->second;
        if (backend->IsEligible() && !backend->IsDraining()) {
            return backend;
        }

        ++it;
        if (it == ring_.end()) {
            it = ring_.begin();
        }
        checked_vnodes++;
    }

    return nullptr;
}

std::vector<std::shared_ptr<Backend>> ConsistentHashLoadBalancer::GetAllBackends() const {
    return backends_;
}

size_t ConsistentHashLoadBalancer::GetBackendCount() const noexcept {
    return backends_.size();
}

bool ConsistentHashLoadBalancer::IsEmpty() const noexcept {
    return backends_.empty();
}

void ConsistentHashLoadBalancer::ClearBackends() noexcept {
    backends_.clear();
    ring_.clear();
    ring_dirty_ = false;
    rr_index_ = 0;
}

void ConsistentHashLoadBalancer::RebuildRing() noexcept {
    ring_.clear();
    for (const auto& backend : backends_) {
        for (size_t i = 0; i < virtual_nodes_; ++i) {
            std::string key_str = std::to_string(backend->GetId()) + "#" + std::to_string(i);
            uint64_t hash = HashString(key_str);
            ring_[hash] = backend;
        }
    }
    ring_dirty_ = false;
}

uint64_t ConsistentHashLoadBalancer::HashString(const std::string& input) const noexcept {
    // 64-bit FNV-1a hash algorithm
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (char c : input) {
        hash ^= static_cast<uint8_t>(c);
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

} // namespace helios
