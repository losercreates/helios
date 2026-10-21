#include <gtest/gtest.h>
#include "config/config_loader.hpp"
#include <filesystem>

using namespace helios;

namespace {
std::string ResolveConfigPath(const std::string& filename) {
#ifdef HELIOS_TEST_CONFIG_DIR
    std::string full_path = std::string(HELIOS_TEST_CONFIG_DIR) + "/" + filename;
    if (std::filesystem::exists(full_path)) return full_path;
#endif
    if (std::filesystem::exists("config/" + filename)) return "config/" + filename;
    if (std::filesystem::exists("../config/" + filename)) return "../config/" + filename;
    if (std::filesystem::exists("../../config/" + filename)) return "../../config/" + filename;
    return filename;
}
} // namespace

// ============================================================================
// 1. Preset Configuration Files
// ============================================================================

TEST(ConfigLoaderTest, LoadHeliosMvpPreset) {
    std::string path = ResolveConfigPath("helios_mvp.yaml");
    auto result = ConfigLoader::LoadFromFile(path);
    ASSERT_TRUE(result.success) << "Failed to load helios_mvp.yaml from " << path << ": " << result.error_message;

    const auto& cfg = result.config;
    EXPECT_EQ(cfg.server.host, "0.0.0.0");
    EXPECT_EQ(cfg.server.port, 8080);
    EXPECT_EQ(cfg.server.workers, 1);
    EXPECT_EQ(cfg.server.max_connections, 10000);
    EXPECT_EQ(cfg.server.buffer_pool_blocks, 4096);
    EXPECT_EQ(cfg.server.buffer_block_size, 16384);

    ASSERT_EQ(cfg.backend_pools.size(), 1);
    EXPECT_EQ(cfg.backend_pools[0].name, "default_pool");
    EXPECT_EQ(cfg.backend_pools[0].algorithm, "round_robin");
    ASSERT_EQ(cfg.backend_pools[0].endpoints.size(), 2);
    EXPECT_EQ(cfg.backend_pools[0].endpoints[0].host, "127.0.0.1");
    EXPECT_EQ(cfg.backend_pools[0].endpoints[0].port, 9001);
    EXPECT_EQ(cfg.backend_pools[0].endpoints[1].host, "127.0.0.1");
    EXPECT_EQ(cfg.backend_pools[0].endpoints[1].port, 9002);

    EXPECT_EQ(cfg.timeouts.idle_timeout.count(), 30000);
    EXPECT_EQ(cfg.timeouts.half_close_fallback.count(), 5000);

    // Verify WorkerPoolConfig mapping
    auto wcfg = cfg.ToWorkerPoolConfig();
    EXPECT_EQ(wcfg.listen_host, "0.0.0.0");
    EXPECT_EQ(wcfg.listen_port, 8080);
    EXPECT_EQ(wcfg.num_workers, 1);
    EXPECT_EQ(wcfg.total_buffers, 4096);
    EXPECT_EQ(wcfg.buffer_size, 16384);
    EXPECT_EQ(wcfg.lb_algorithm, "round_robin");
}

TEST(ConfigLoaderTest, LoadHeliosProductionPreset) {
    std::string path = ResolveConfigPath("helios_production.yaml");
    auto result = ConfigLoader::LoadFromFile(path);
    ASSERT_TRUE(result.success) << "Failed to load helios_production.yaml from " << path << ": " << result.error_message;

    const auto& cfg = result.config;
    EXPECT_EQ(cfg.server.host, "0.0.0.0");
    EXPECT_EQ(cfg.server.port, 443);
    EXPECT_EQ(cfg.server.workers, 0); // 0 = hardware concurrency
    EXPECT_EQ(cfg.server.max_connections, 50000);
    EXPECT_EQ(cfg.server.buffer_pool_blocks, 8192);
    EXPECT_EQ(cfg.server.buffer_block_size, 16384);

    ASSERT_EQ(cfg.backend_pools.size(), 1);
    EXPECT_EQ(cfg.backend_pools[0].name, "web_backends");
    EXPECT_EQ(cfg.backend_pools[0].algorithm, "weighted_round_robin");
    ASSERT_EQ(cfg.backend_pools[0].endpoints.size(), 2);
    EXPECT_EQ(cfg.backend_pools[0].endpoints[0].host, "10.0.1.10");
    EXPECT_EQ(cfg.backend_pools[0].endpoints[0].port, 8080);
    EXPECT_EQ(cfg.backend_pools[0].endpoints[0].weight, 3);
    EXPECT_EQ(cfg.backend_pools[0].endpoints[1].host, "10.0.1.11");
    EXPECT_EQ(cfg.backend_pools[0].endpoints[1].port, 8080);
    EXPECT_EQ(cfg.backend_pools[0].endpoints[1].weight, 2);

    EXPECT_EQ(cfg.timeouts.idle_timeout.count(), 60000);
    EXPECT_EQ(cfg.timeouts.header_read_timeout.count(), 5000);
    EXPECT_EQ(cfg.timeouts.connect_timeout.count(), 2000);
    EXPECT_EQ(cfg.timeouts.half_close_fallback.count(), 5000);

    EXPECT_TRUE(cfg.health_check.enabled);
    EXPECT_EQ(cfg.health_check.interval.count(), 5000);
    EXPECT_EQ(cfg.health_check.timeout.count(), 1000);
    EXPECT_EQ(cfg.health_check.unhealthy_threshold, 3);
    EXPECT_EQ(cfg.health_check.healthy_threshold, 2);

    EXPECT_TRUE(cfg.metrics.enabled);
    EXPECT_EQ(cfg.metrics.port, 9090);

    // Verify Prometheus config mapping
    auto pcfg = cfg.ToPrometheusConfig();
    EXPECT_EQ(pcfg.host, "0.0.0.0");
    EXPECT_EQ(pcfg.port, 9090);
}

// ============================================================================
// 2. String Parsing & Data Model Validation
// ============================================================================

TEST(ConfigLoaderTest, ParseYamlStringComplete) {
    const std::string yaml = R"(
server:
  host: "127.0.0.1"
  port: 8443
  workers: 4
  cpu_affinity: true
  max_connections: 25000
  buffer_pool_blocks: 2048
  buffer_block_size: 32768
  io_uring_entries: 1024

backend_pools:
  - name: "api_cluster"
    algorithm: "least_connections"
    endpoints:
      - host: "192.168.1.50"
        port: 8000
        weight: 1
      - host: "192.168.1.51"
        port: 8000
        weight: 2

timeouts:
  idle_timeout_ms: 45000
  header_read_timeout_ms: 3000
  connect_timeout_ms: 1500
  half_close_fallback_ms: 4000
  drain_timeout_ms: 10000

health_check:
  enabled: true
  interval_ms: 3000
  timeout_ms: 800
  unhealthy_threshold: 4
  healthy_threshold: 3

metrics:
  enabled: true
  port: 9100
  path: "/custom_metrics"
)";

    auto result = ConfigLoader::LoadFromString(yaml);
    ASSERT_TRUE(result.success) << "Failed to parse YAML string: " << result.error_message;

    const auto& cfg = result.config;
    EXPECT_EQ(cfg.server.host, "127.0.0.1");
    EXPECT_EQ(cfg.server.port, 8443);
    EXPECT_EQ(cfg.server.workers, 4);
    EXPECT_TRUE(cfg.server.cpu_affinity);
    EXPECT_EQ(cfg.server.buffer_block_size, 32768);
    EXPECT_EQ(cfg.server.io_uring_entries, 1024);

    ASSERT_EQ(cfg.backend_pools.size(), 1);
    EXPECT_EQ(cfg.backend_pools[0].name, "api_cluster");
    EXPECT_EQ(cfg.backend_pools[0].algorithm, "least_connections");
    ASSERT_EQ(cfg.backend_pools[0].endpoints.size(), 2);
    EXPECT_EQ(cfg.backend_pools[0].endpoints[0].host, "192.168.1.50");
    EXPECT_EQ(cfg.backend_pools[0].endpoints[0].port, 8000);
    EXPECT_EQ(cfg.backend_pools[0].endpoints[1].weight, 2);

    EXPECT_EQ(cfg.timeouts.idle_timeout.count(), 45000);
    EXPECT_EQ(cfg.timeouts.drain_timeout.count(), 10000);

    EXPECT_TRUE(cfg.health_check.enabled);
    EXPECT_EQ(cfg.health_check.interval.count(), 3000);
    EXPECT_EQ(cfg.health_check.unhealthy_threshold, 4);

    EXPECT_TRUE(cfg.metrics.enabled);
    EXPECT_EQ(cfg.metrics.port, 9100);
    EXPECT_EQ(cfg.metrics.path, "/custom_metrics");
}

TEST(ConfigLoaderTest, ParseYamlWithCommentsAndQuotes) {
    const std::string yaml = R"(
# Top-level comment
server: # inline comment
  host: "10.0.0.1" # another comment
  port: 9000
  buffer_block_size: 16384
  io_uring_entries: 512

backend_pools:
  - name: 'quoted_pool'
    algorithm: "consistent_hash"
    endpoints:
      - host: '10.0.0.2'
        port: 9001
        weight: 1 # weight comment
)";

    auto result = ConfigLoader::LoadFromString(yaml);
    ASSERT_TRUE(result.success) << "Failed to parse: " << result.error_message;
    EXPECT_EQ(result.config.server.host, "10.0.0.1");
    EXPECT_EQ(result.config.backend_pools[0].name, "quoted_pool");
    EXPECT_EQ(result.config.backend_pools[0].algorithm, "consistent_hash");
    EXPECT_EQ(result.config.backend_pools[0].endpoints[0].host, "10.0.0.2");
}

// ============================================================================
// 3. Syntax & File Errors
// ============================================================================

TEST(ConfigLoaderTest, NonExistentFileError) {
    auto result = ConfigLoader::LoadFromFile("non_existent_config_file_12345.yaml");
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error_message.find("Failed to open"), std::string::npos);
}

TEST(ConfigLoaderTest, SyntaxErrorMissingColon) {
    const std::string yaml = R"(
server:
  host "missing_colon"
)";
    auto result = ConfigLoader::LoadFromString(yaml);
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error_message.find("Expected key-value pair"), std::string::npos);
}

// ============================================================================
// 4. Schema Validation Failures
// ============================================================================

TEST(ConfigLoaderTest, ValidationFailsOnEmptyBackendPools) {
    const std::string yaml = R"(
server:
  host: "127.0.0.1"
  port: 8080
)";
    auto result = ConfigLoader::LoadFromString(yaml);
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error_message.find("At least one backend pool must be defined"), std::string::npos);
}

TEST(ConfigLoaderTest, ValidationFailsOnUnknownAlgorithm) {
    const std::string yaml = R"(
server:
  host: "127.0.0.1"
  port: 8080

backend_pools:
  - name: "pool1"
    algorithm: "non_existent_algorithm"
    endpoints:
      - host: "127.0.0.1"
        port: 9001
)";
    auto result = ConfigLoader::LoadFromString(yaml);
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error_message.find("Unknown load balancing algorithm"), std::string::npos);
}

TEST(ConfigLoaderTest, ValidationFailsOnZeroEndpointPort) {
    const std::string yaml = R"(
server:
  host: "127.0.0.1"
  port: 8080

backend_pools:
  - name: "pool1"
    algorithm: "round_robin"
    endpoints:
      - host: "127.0.0.1"
        port: 0
)";
    auto result = ConfigLoader::LoadFromString(yaml);
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error_message.find("invalid port 0"), std::string::npos);
}

TEST(ConfigLoaderTest, ValidationFailsOnZeroEndpointWeight) {
    const std::string yaml = R"(
server:
  host: "127.0.0.1"
  port: 8080

backend_pools:
  - name: "pool1"
    algorithm: "round_robin"
    endpoints:
      - host: "127.0.0.1"
        port: 9001
        weight: 0
)";
    auto result = ConfigLoader::LoadFromString(yaml);
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error_message.find("weight >= 1"), std::string::npos);
}

TEST(ConfigLoaderTest, ValidationFailsOnNonPowerOfTwoBufferSize) {
    const std::string yaml = R"(
server:
  host: "127.0.0.1"
  port: 8080
  buffer_block_size: 15000

backend_pools:
  - name: "pool1"
    algorithm: "round_robin"
    endpoints:
      - host: "127.0.0.1"
        port: 9001
)";
    auto result = ConfigLoader::LoadFromString(yaml);
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error_message.find("buffer_block_size must be a power of two"), std::string::npos);
}

TEST(ConfigLoaderTest, ValidationFailsOnInvalidMetricsPath) {
    const std::string yaml = R"(
server:
  host: "127.0.0.1"
  port: 8080

backend_pools:
  - name: "pool1"
    algorithm: "round_robin"
    endpoints:
      - host: "127.0.0.1"
        port: 9001

metrics:
  enabled: true
  port: 9090
  path: "metrics_without_leading_slash"
)";
    auto result = ConfigLoader::LoadFromString(yaml);
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error_message.find("metrics.path must start with '/'"), std::string::npos);
}

TEST(ConfigLoaderTest, ParseSecurityConfigValid) {
    const std::string yaml = R"(
server:
  host: "127.0.0.1"
  port: 8080

backend_pools:
  - name: "pool1"
    algorithm: "round_robin"
    endpoints:
      - host: "127.0.0.1"
        port: 9001

security:
  acl:
    enabled: true
    default_policy: "deny"
    rules:
      - cidr: "192.168.1.0/24"
        action: "allow"
      - cidr: "10.0.0.1/32"
        action: "allow"
  rate_limit:
    enabled: true
    rate_per_sec: 50.0
    burst_size: 15
    cidr_mask: 24
  slowloris:
    enabled: true
    header_read_timeout_ms: 3500
)";
    auto result = ConfigLoader::LoadFromString(yaml);
    ASSERT_TRUE(result.success) << "Failed to parse: " << result.error_message;
    EXPECT_TRUE(result.config.security.acl.enabled);
    EXPECT_EQ(result.config.security.acl.default_policy, "deny");
    ASSERT_EQ(result.config.security.acl.rules.size(), 2u);
    EXPECT_EQ(result.config.security.acl.rules[0].cidr, "192.168.1.0/24");
    EXPECT_EQ(result.config.security.acl.rules[0].action, "allow");

    EXPECT_TRUE(result.config.security.rate_limit.enabled);
    EXPECT_DOUBLE_EQ(result.config.security.rate_limit.rate_per_sec, 50.0);
    EXPECT_EQ(result.config.security.rate_limit.burst_size, 15u);
    EXPECT_EQ(result.config.security.rate_limit.cidr_mask, 24);

    EXPECT_TRUE(result.config.security.slowloris.enabled);
    EXPECT_EQ(result.config.security.slowloris.header_read_timeout.count(), 3500);
}

TEST(ConfigLoaderTest, ValidationFailsOnInvalidCidr) {
    const std::string yaml = R"(
server:
  host: "127.0.0.1"
  port: 8080

backend_pools:
  - name: "pool1"
    algorithm: "round_robin"
    endpoints:
      - host: "127.0.0.1"
        port: 9001

security:
  acl:
    enabled: true
    default_policy: "allow"
    rules:
      - cidr: "invalid-cidr/99"
        action: "deny"
)";
    auto result = ConfigLoader::LoadFromString(yaml);
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error_message.find("invalid CIDR format"), std::string::npos);
}

TEST(ConfigLoaderTest, ValidationFailsOnInvalidRateLimit) {
    const std::string yaml = R"(
server:
  host: "127.0.0.1"
  port: 8080

backend_pools:
  - name: "pool1"
    algorithm: "round_robin"
    endpoints:
      - host: "127.0.0.1"
        port: 9001

security:
  rate_limit:
    enabled: true
    rate_per_sec: 0.0
    burst_size: 10
)";
    auto result = ConfigLoader::LoadFromString(yaml);
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error_message.find("rate_per_sec must be > 0"), std::string::npos);
}

