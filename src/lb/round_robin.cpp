#include "round_robin.hpp"
#include "net/socket_utils.hpp"
#include <utility>

namespace helios {

RoundRobinLoadBalancer::RoundRobinLoadBalancer(std::vector<BackendEndpoint> backends)
    : backends_(std::move(backends)) {}

void RoundRobinLoadBalancer::AddBackend(std::string host, uint16_t port) {
    backends_.push_back(BackendEndpoint{
        .host = std::move(host),
        .port = port,
        .active = true
    });
}

std::optional<BackendEndpoint> RoundRobinLoadBalancer::SelectBackend() {
    if (backends_.empty()) {
        return std::nullopt;
    }

    size_t count = backends_.size();
    for (size_t i = 0; i < count; ++i) {
        size_t idx = (next_index_ + i) % count;
        if (backends_[idx].active) {
            next_index_ = (idx + 1) % count;
            return backends_[idx];
        }
    }

    return std::nullopt;
}

bool RoundRobinLoadBalancer::SelectBackendAddr(sockaddr_in* out_addr, BackendEndpoint* out_endpoint) {
    if (!out_addr) return false;
    auto ep = SelectBackend();
    if (!ep.has_value()) return false;

    if (out_endpoint) {
        *out_endpoint = ep.value();
    }

    return SocketUtils::ParseSockAddr(ep->host.c_str(), ep->port, out_addr);
}

void RoundRobinLoadBalancer::ClearBackends() noexcept {
    backends_.clear();
    next_index_ = 0;
}

} // namespace helios
