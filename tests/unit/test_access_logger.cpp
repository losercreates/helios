#include <gtest/gtest.h>
#include "logging/access_logger.hpp"
#include "config/config_loader.hpp"
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>

using namespace helios;

namespace {
AccessLogEntry CreateSampleEntry() {
    AccessLogEntry entry;
    entry.timestamp = std::chrono::system_clock::from_time_t(1700000000); // Deterministic timestamp
    entry.client_ip = "192.168.1.100";
    entry.client_port = 54321;
    entry.method = "GET";
    entry.uri = "/api/v1/health";
    entry.http_version = "HTTP/1.1";
    entry.status_code = 200;
    entry.bytes_sent = 1024;
    entry.bytes_received = 256;
    entry.duration_us = std::chrono::microseconds(1250);
    entry.upstream_endpoint = "10.0.0.1:80";
    return entry;
}
} // namespace

TEST(AccessLoggerTest, FormatJsonStructure) {
    auto entry = CreateSampleEntry();
    std::string json = AccessLogger::FormatJson(entry);

    EXPECT_NE(json.find("\"client\":\"192.168.1.100:54321\""), std::string::npos);
    EXPECT_NE(json.find("\"method\":\"GET\""), std::string::npos);
    EXPECT_NE(json.find("\"uri\":\"/api/v1/health\""), std::string::npos);
    EXPECT_NE(json.find("\"version\":\"HTTP/1.1\""), std::string::npos);
    EXPECT_NE(json.find("\"status\":200"), std::string::npos);
    EXPECT_NE(json.find("\"bytes_sent\":1024"), std::string::npos);
    EXPECT_NE(json.find("\"bytes_received\":256"), std::string::npos);
    EXPECT_NE(json.find("\"duration_us\":1250"), std::string::npos);
    EXPECT_NE(json.find("\"upstream\":\"10.0.0.1:80\""), std::string::npos);
    EXPECT_EQ(json.back(), '\n');
}

TEST(AccessLoggerTest, FormatJsonWithUpstreamDurationPoolAndRequestId) {
    auto entry = CreateSampleEntry();
    entry.upstream_duration_us = std::chrono::microseconds(850);
    entry.matched_pool = "api_cluster";
    entry.request_id = "abc123def456";

    std::string json = AccessLogger::FormatJson(entry);
    EXPECT_NE(json.find("\"upstream_duration_us\":850"), std::string::npos);
    EXPECT_NE(json.find("\"pool\":\"api_cluster\""), std::string::npos);
    EXPECT_NE(json.find("\"request_id\":\"abc123def456\""), std::string::npos);
}

TEST(AccessLoggerTest, FormatCombinedStructure) {
    auto entry = CreateSampleEntry();
    std::string combined = AccessLogger::FormatCombined(entry);

    EXPECT_NE(combined.find("192.168.1.100 - - ["), std::string::npos);
    EXPECT_NE(combined.find("\"GET /api/v1/health HTTP/1.1\" 200 1024 1250us [10.0.0.1:80]"), std::string::npos);
    EXPECT_EQ(combined.back(), '\n');
}

TEST(AccessLoggerTest, LifecycleStartStop) {
    AccessLoggerConfig cfg;
    cfg.output = "stdout";
    AccessLogger logger(cfg);

    EXPECT_FALSE(logger.IsRunning());
    EXPECT_TRUE(logger.Start());
    EXPECT_TRUE(logger.IsRunning());
    logger.Stop();
    EXPECT_FALSE(logger.IsRunning());
}

TEST(AccessLoggerTest, AsyncFileLoggingAndFlush) {
    std::string temp_log = "/tmp/helios_test_access_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + ".log";

    AccessLoggerConfig cfg;
    cfg.enabled = true;
    cfg.format = LogFormat::Json;
    cfg.output = temp_log;
    cfg.queue_capacity = 1000;
    cfg.flush_interval = std::chrono::milliseconds(50);

    {
        AccessLogger logger(cfg);
        ASSERT_TRUE(logger.Start());

        for (int i = 0; i < 50; ++i) {
            auto entry = CreateSampleEntry();
            entry.uri = "/test/" + std::to_string(i);
            logger.Log(std::move(entry));
        }

        EXPECT_EQ(logger.GetTotalLogged(), 50);
        EXPECT_EQ(logger.GetTotalDropped(), 0);

        // Allow background thread to flush
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        logger.Stop();
    }

    // Verify written contents from disk
    ASSERT_TRUE(std::filesystem::exists(temp_log));
    std::ifstream in(temp_log);
    ASSERT_TRUE(in.is_open());

    std::string line;
    int line_count = 0;
    while (std::getline(in, line)) {
        if (!line.empty()) {
            line_count++;
            EXPECT_NE(line.find("\"client\":\"192.168.1.100:54321\""), std::string::npos);
        }
    }
    EXPECT_EQ(line_count, 50);

    std::filesystem::remove(temp_log);
}

TEST(AccessLoggerTest, QueueDropOnOverflow) {
    AccessLoggerConfig cfg;
    cfg.enabled = true;
    cfg.format = LogFormat::Json;
    cfg.output = "stdout";
    cfg.queue_capacity = 5; // Very small queue to force overflow
    cfg.flush_interval = std::chrono::milliseconds(1000);

    AccessLogger logger(cfg);
    // Do not start background worker so queue stays full
    // But mark as running to accept Log calls
    ASSERT_TRUE(logger.Start());

    for (int i = 0; i < 20; ++i) {
        logger.Log(CreateSampleEntry());
    }

    EXPECT_GT(logger.GetTotalDropped(), 0);
    logger.Stop();
}

TEST(AccessLoggerTest, ConfigYamlParsing) {
    std::string yaml = R"(
server:
  host: "127.0.0.1"
  port: 8080
  workers: 1
  max_connections: 1000
  buffer_pool_blocks: 1024
  buffer_block_size: 4096
  io_uring_entries: 128

backend_pools:
  - name: "test_pool"
    algorithm: "round_robin"
    endpoints:
      - host: "127.0.0.1"
        port: 9001
        weight: 1

logging:
  enabled: true
  format: "combined"
  output: "/var/log/helios/access.log"
  queue_capacity: 131072
  flush_interval_ms: 250
)";

    auto res = ConfigLoader::LoadFromString(yaml);
    ASSERT_TRUE(res.success) << res.error_message;

    EXPECT_TRUE(res.config.logging.enabled);
    EXPECT_EQ(res.config.logging.format, "combined");
    EXPECT_EQ(res.config.logging.output, "/var/log/helios/access.log");
    EXPECT_EQ(res.config.logging.queue_capacity, 131072);
    EXPECT_EQ(res.config.logging.flush_interval.count(), 250);

    auto logger_cfg = res.config.ToAccessLoggerConfig();
    EXPECT_TRUE(logger_cfg.enabled);
    EXPECT_EQ(logger_cfg.format, LogFormat::Combined);
    EXPECT_EQ(logger_cfg.output, "/var/log/helios/access.log");
    EXPECT_EQ(logger_cfg.queue_capacity, 131072);
    EXPECT_EQ(logger_cfg.flush_interval.count(), 250);
}

TEST(AccessLoggerTest, ReopenForLogRotation) {
    auto ts = std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    std::string active_log = "/tmp/helios_rotate_active_" + ts + ".log";
    std::string rotated_log = "/tmp/helios_rotate_rotated_" + ts + ".log";

    AccessLoggerConfig cfg;
    cfg.enabled = true;
    cfg.format = LogFormat::Json;
    cfg.output = active_log;
    cfg.queue_capacity = 1000;
    cfg.flush_interval = std::chrono::milliseconds(20);

    AccessLogger logger(cfg);
    ASSERT_TRUE(logger.Start());

    // 1. Write initial entries to active log
    for (int i = 0; i < 5; ++i) {
        auto entry = CreateSampleEntry();
        entry.uri = "/pre-rotation/" + std::to_string(i);
        logger.Log(std::move(entry));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ASSERT_TRUE(std::filesystem::exists(active_log));

    // 2. Simulate logrotate: rename file
    std::filesystem::rename(active_log, rotated_log);
    ASSERT_TRUE(std::filesystem::exists(rotated_log));
    ASSERT_FALSE(std::filesystem::exists(active_log));

    // 3. Trigger Reopen()
    EXPECT_TRUE(logger.Reopen());

    // 4. Write subsequent entries post-rotation
    for (int i = 0; i < 5; ++i) {
        auto entry = CreateSampleEntry();
        entry.uri = "/post-rotation/" + std::to_string(i);
        logger.Log(std::move(entry));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    logger.Stop();

    // 5. Verify both files exist and contain their respective entries
    ASSERT_TRUE(std::filesystem::exists(rotated_log));
    ASSERT_TRUE(std::filesystem::exists(active_log));

    {
        std::ifstream in(rotated_log);
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        EXPECT_NE(content.find("/pre-rotation/"), std::string::npos);
        EXPECT_EQ(content.find("/post-rotation/"), std::string::npos);
    }
    {
        std::ifstream in(active_log);
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        EXPECT_NE(content.find("/post-rotation/"), std::string::npos);
        EXPECT_EQ(content.find("/pre-rotation/"), std::string::npos);
    }

    std::filesystem::remove(rotated_log);
    std::filesystem::remove(active_log);
}
