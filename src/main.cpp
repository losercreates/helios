#include "foundation.hpp"
#include "worker.hpp"
#include "config/config_loader.hpp"
#include "metrics/prometheus_server.hpp"
#include "metrics/metrics_collector.hpp"
#include "logging/access_logger.hpp"
#include <iostream>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>
#include <string_view>
#include <memory>

namespace {
std::atomic<bool> g_stop_signal{false};
std::atomic<bool> g_reload_signal{false};

void SignalHandler(int signum) {
    if (signum == SIGHUP) {
        g_reload_signal.store(true, std::memory_order_release);
    } else {
        g_stop_signal.store(true, std::memory_order_release);
    }
}

void PrintUsage(const char* prog_name) {
    std::cout << "Usage: " << prog_name << " [OPTIONS]\n\n"
              << "Helios - Linux-Native High-Performance io_uring Reverse Proxy & Load Balancer\n\n"
              << "Options:\n"
              << "  -c, --config <path>     Path to declarative YAML configuration file\n"
              << "  -d, --daemon            Run continuously in foreground daemon mode until SIGINT/SIGTERM\n"
              << "  -t, --test-config       Validate configuration file syntax and schema, then exit\n"
              << "  -h, --help              Display this help message and exit\n"
              << "  -v, --version           Display version information and exit\n\n"
              << "Example:\n"
              << "  " << prog_name << " -c config/helios_production.yaml --daemon\n";
}
} // namespace

int main(int argc, char* argv[]) {
    auto info = helios::GetSystemInfo();

    std::string config_path;
    bool daemon_mode = false;
    bool test_config_only = false;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (arg == "--help" || arg == "-h") {
            PrintUsage(argv[0]);
            return 0;
        }
        if (arg == "--version" || arg == "-v") {
            std::cout << info.name << " v" << info.version
                      << " (C++ Standard: " << info.cpp_standard << ")\n";
            return 0;
        }
        if (arg == "--daemon" || arg == "-d") {
            daemon_mode = true;
        } else if (arg == "--test-config" || arg == "-t") {
            test_config_only = true;
        } else if (arg == "--config" || arg == "-c") {
            if (i + 1 < argc) {
                config_path = argv[++i];
            } else {
                std::cerr << "[HELIOS] Error: Option " << arg << " requires an argument.\n";
                return 1;
            }
        } else if (arg.rfind("--config=", 0) == 0) {
            config_path = arg.substr(9);
        } else {
            std::cerr << "[HELIOS] Error: Unknown option '" << arg << "'. Use --help for usage.\n";
            return 1;
        }
    }

    std::cout << "[HELIOS] Starting " << info.name << " v" << info.version << "\n";
    std::cout << "[HELIOS] Compiled with C++ standard: " << info.cpp_standard << "\n";

    // Setup signal handling for graceful shutdown & dynamic hot-reload
    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);
    std::signal(SIGHUP, SignalHandler);

    helios::HeliosConfig config;

    if (!config_path.empty()) {
        std::cout << "[HELIOS] Loading configuration from: " << config_path << "\n";
        auto res = helios::ConfigLoader::LoadFromFile(config_path);
        if (!res.success) {
            std::cerr << "[HELIOS] Fatal Configuration Error: " << res.error_message << "\n";
            return 1;
        }
        config = std::move(res.config);
    } else {
        if (test_config_only) {
            std::cerr << "[HELIOS] Error: --test-config requires a configuration file specified via --config <path>.\n";
            return 1;
        }

        std::cout << "[HELIOS] No configuration file specified. Using default configuration (127.0.0.1:8080 -> 127.0.0.1:9001).\n";
        config.server.host = "127.0.0.1";
        config.server.port = 8080;
        config.server.workers = std::thread::hardware_concurrency();
        if (config.server.workers == 0) config.server.workers = 2;

        helios::BackendPoolConfig default_pool;
        default_pool.name = "default_pool";
        default_pool.algorithm = "round_robin";
        default_pool.endpoints.push_back({"127.0.0.1", 9001, 1});
        config.backend_pools.push_back(std::move(default_pool));

        config.metrics.enabled = true;
        config.metrics.port = 9090;
    }

    if (test_config_only) {
        std::cout << "[HELIOS] Configuration file syntax and schema validation successful.\n";
        return 0;
    }

    // Initialize Prometheus Telemetry Server if enabled
    std::shared_ptr<helios::MetricsCollector> metrics_collector;
    std::unique_ptr<helios::PrometheusServer> prometheus_server;

    if (config.metrics.enabled) {
        metrics_collector = std::make_shared<helios::MetricsCollector>();
        auto prom_cfg = config.ToPrometheusConfig();
        prometheus_server = std::make_unique<helios::PrometheusServer>(metrics_collector, prom_cfg);
        if (prometheus_server->Start()) {
            std::cout << "[HELIOS] Prometheus telemetry server listening on http://"
                      << prom_cfg.host << ":" << prometheus_server->GetPort()
                      << config.metrics.path << "\n";
        } else {
            std::cerr << "[HELIOS] Warning: Failed to start Prometheus telemetry server on port "
                      << prom_cfg.port << "\n";
        }
    }

    // Initialize High-Performance Asynchronous Access Logger if enabled
    std::shared_ptr<helios::AccessLogger> access_logger;
    if (config.logging.enabled) {
        auto log_cfg = config.ToAccessLoggerConfig();
        access_logger = std::make_shared<helios::AccessLogger>(log_cfg);
        if (access_logger->Start()) {
            std::cout << "[HELIOS] Asynchronous access logger active (format="
                      << config.logging.format << ", output=" << config.logging.output
                      << ", queue_capacity=" << config.logging.queue_capacity << ")\n";
        } else {
            std::cerr << "[HELIOS] Warning: Failed to start access logger.\n";
        }
    }

    // Initialize Multi-Worker Proxy Pool
    auto pool_config = config.ToWorkerPoolConfig();
    helios::WorkerPool pool(pool_config);

    if (access_logger) {
        pool.SetAccessLogger(access_logger);
    }

    if (metrics_collector) {
        pool.SetMetricsCollector(metrics_collector);
    }

    for (const auto& backend_pool : config.backend_pools) {
        for (const auto& ep : backend_pool.endpoints) {
            pool.AddBackend(ep.host, ep.port, ep.weight);
        }
    }

    if (pool.Start()) {
        std::cout << "[HELIOS] Multi-Worker Thread-per-Core Proxy initialized with "
                  << pool.GetWorkerCount() << " workers on port " << pool.GetPort() << "\n";

        if (daemon_mode) {
            std::cout << "[HELIOS] Running in daemon mode. Send SIGINT/SIGTERM to stop, SIGHUP to reload.\n";
            while (!g_stop_signal.load(std::memory_order_relaxed)) {
                if (g_reload_signal.exchange(false, std::memory_order_acq_rel)) {
                    if (config_path.empty()) {
                        std::cerr << "[HELIOS] Reload ignored: No configuration file specified (running with defaults).\n";
                    } else {
                        std::cout << "[HELIOS] SIGHUP received: Reloading configuration from " << config_path << "...\n";
                        auto res = helios::ConfigLoader::LoadFromFile(config_path);
                        if (!res.success) {
                            std::cerr << "[HELIOS] Configuration reload failed: " << res.error_message
                                      << " - Retaining current configuration.\n";
                        } else {
                            std::vector<helios::WorkerPool::BackendInfo> new_endpoints;
                            for (const auto& pool_cfg : res.config.backend_pools) {
                                for (const auto& ep : pool_cfg.endpoints) {
                                    new_endpoints.push_back({ep.host, ep.port, ep.weight});
                                }
                            }

                            pool.ReloadBackends(new_endpoints);
                            pool.ReloadSecurity(res.config.security);
                            if (res.config.tls.enabled) {
                                std::string tls_err;
                                if (!pool.ReloadTls(res.config.tls, &tls_err)) {
                                    std::cerr << "[HELIOS] TLS configuration reload failed: " << tls_err
                                              << " - Retaining current TLS context.\n";
                                } else {
                                    std::cout << "[HELIOS] TLS certificates and security configuration reloaded successfully.\n";
                                }
                            }
                            config = std::move(res.config);
                            std::cout << "[HELIOS] Configuration reloaded successfully ("
                                      << new_endpoints.size() << " backend endpoint(s) active).\n";
                        }
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        } else {
            // Self-validation run (e.g. for startup integration tests)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        std::cout << "[HELIOS] Initiating graceful draining on all workers...\n";
        pool.Stop();
    } else {
        std::cerr << "[HELIOS] Multi-worker listener initialization failed on port " << pool_config.listen_port << "\n";
        if (prometheus_server && prometheus_server->IsRunning()) {
            prometheus_server->Stop();
        }
        return 1;
    }

    if (prometheus_server && prometheus_server->IsRunning()) {
        prometheus_server->Stop();
    }

    if (access_logger && access_logger->IsRunning()) {
        std::cout << "[HELIOS] Flushing and stopping access logger ("
                  << access_logger->GetTotalLogged() << " records logged, "
                  << access_logger->GetTotalDropped() << " dropped)...\n";
        access_logger->Stop();
    }

    std::cout << "[HELIOS] Exiting cleanly (0).\n";
    return 0;
}
