#pragma once

#include "worker.hpp"
#include "metrics/prometheus_server.hpp"
#include "logging/access_logger.hpp"
#include <cstdint>
#include <string>
#include <vector>
#include <chrono>
#include <map>
#include <memory>
#include <optional>

namespace helios {

enum class YamlNodeType {
    Null,
    Scalar,
    Sequence,
    Map
};

struct YamlNode {
    YamlNodeType type{YamlNodeType::Null};
    std::string value;
    std::vector<YamlNode> sequence;
    std::map<std::string, YamlNode> map;
    size_t line{0};
    size_t column{0};

    [[nodiscard]] bool IsNull() const noexcept { return type == YamlNodeType::Null; }
    [[nodiscard]] bool IsScalar() const noexcept { return type == YamlNodeType::Scalar; }
    [[nodiscard]] bool IsSequence() const noexcept { return type == YamlNodeType::Sequence; }
    [[nodiscard]] bool IsMap() const noexcept { return type == YamlNodeType::Map; }

    [[nodiscard]] std::string AsString(const std::string& default_val = "") const;
    [[nodiscard]] int64_t AsInt(int64_t default_val = 0) const;
    [[nodiscard]] uint64_t AsUInt(uint64_t default_val = 0) const;
    [[nodiscard]] double AsDouble(double default_val = 0.0) const;
    [[nodiscard]] bool AsBool(bool default_val = false) const;

    [[nodiscard]] const YamlNode* Find(const std::string& key) const;
    [[nodiscard]] const YamlNode& operator[](const std::string& key) const;
};

struct ServerConfig {
    std::string host{"0.0.0.0"};
    uint16_t port{8080};
    size_t workers{0}; // 0 = hardware concurrency
    bool cpu_affinity{true};
    size_t max_connections{10000};
    size_t buffer_pool_blocks{1024};
    size_t buffer_block_size{16384};
    size_t io_uring_entries{512};
};

#include "backend/backend_pool.hpp"

struct BackendEndpointConfig {
    std::string host{"127.0.0.1"};
    uint16_t port{9001};
    uint32_t weight{1};
};

struct BackendPoolConfig {
    std::string name{"default_pool"};
    std::string algorithm{"round_robin"};
    std::vector<BackendEndpointConfig> endpoints;
    bool enable_connection_pool{false};
    BackendConnectionPoolConfig connection_pool_config{};
};

struct TimeoutsConfig {
    std::chrono::milliseconds idle_timeout{30000};
    std::chrono::milliseconds header_read_timeout{5000};
    std::chrono::milliseconds connect_timeout{2000};
    std::chrono::milliseconds half_close_fallback{5000};
    std::chrono::milliseconds drain_timeout{15000};
};

struct HealthCheckConfig {
    bool enabled{false};
    std::chrono::milliseconds interval{5000};
    std::chrono::milliseconds timeout{1000};
    uint32_t unhealthy_threshold{3};
    uint32_t healthy_threshold{2};
};

struct MetricsConfig {
    bool enabled{true};
    uint16_t port{9090};
    std::string path{"/metrics"};
};

struct LoggingConfig {
    bool enabled{false};
    std::string format{"json"}; // "json" or "combined"
    std::string output{"stdout"}; // "stdout", "stderr", or file path
    size_t queue_capacity{65536};
    std::chrono::milliseconds flush_interval{100};
};

struct HeliosConfig {
    ServerConfig server;
    std::vector<BackendPoolConfig> backend_pools;
    TimeoutsConfig timeouts;
    HealthCheckConfig health_check;
    MetricsConfig metrics;
    LoggingConfig logging;

    [[nodiscard]] WorkerPoolConfig ToWorkerPoolConfig() const;
    [[nodiscard]] PrometheusServer::Config ToPrometheusConfig() const;
    [[nodiscard]] AccessLoggerConfig ToAccessLoggerConfig() const;
};

struct ConfigResult {
    bool success{false};
    HeliosConfig config;
    std::string error_message;
};

class ConfigLoader {
public:
    // Load and validate YAML from a local file path
    static ConfigResult LoadFromFile(const std::string& filepath);

    // Load and validate YAML from a string payload
    static ConfigResult LoadFromString(const std::string& yaml_content);

    // Parse generic YAML text into YamlNode tree
    static bool ParseYaml(const std::string& yaml_content, YamlNode& root, std::string& error_message);

    // Populate typed HeliosConfig from YamlNode
    static bool FromYaml(const YamlNode& root, HeliosConfig& config, std::string& error_message);

    // Validate semantic rules according to contracts/config-schema.md
    static bool Validate(const HeliosConfig& config, std::string& error_message);
};

} // namespace helios
