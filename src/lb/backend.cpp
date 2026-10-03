#include "backend.hpp"
#include "net/socket_utils.hpp"
#include <utility>

namespace helios {

Backend::Backend(uint32_t id, std::string host, uint16_t port, uint32_t weight)
    : id_(id), host_(std::move(host)), port_(port), weight_(weight) {
    // Pre-parse the sockaddr_in for fast hot-path usage.
    // If parsing fails, we zero it out.
    if (!SocketUtils::ParseSockAddr(host_.c_str(), port_, &addr_)) {
        addr_ = {};
    }
}

} // namespace helios
