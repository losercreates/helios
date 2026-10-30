#include "access_logger.hpp"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <ctime>

namespace helios {

AccessLogger::AccessLogger(AccessLoggerConfig config)
    : config_(std::move(config)) {
    queue_.reserve(1024);
}

AccessLogger::~AccessLogger() {
    Stop();
}

bool AccessLogger::Start() {
    if (running_.load(std::memory_order_acquire)) return true;

    if (!config_.output.empty() && config_.output != "stdout" && config_.output != "stderr") {
        std::lock_guard<std::mutex> lock(output_mutex_);
        file_stream_.open(config_.output, std::ios::out | std::ios::app);
        if (!file_stream_.is_open()) {
            std::cerr << "[HELIOS] Warning: Failed to open access log file: " << config_.output
                      << ". Falling back to stdout.\n";
            config_.output = "stdout";
        }
    }

    running_.store(true, std::memory_order_release);
    stop_requested_.store(false, std::memory_order_release);

    worker_thread_ = std::thread(&AccessLogger::FlushLoop, this);
    return true;
}

void AccessLogger::Stop() {
    if (!running_.load(std::memory_order_acquire)) return;

    stop_requested_.store(true, std::memory_order_release);
    cv_.notify_all();

    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }

    // Flush any remaining records
    std::vector<AccessLogEntry> remaining;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        remaining.swap(queue_);
    }

    if (!remaining.empty()) {
        std::string buffer;
        buffer.reserve(remaining.size() * 128);
        for (const auto& entry : remaining) {
            if (config_.format == LogFormat::Json) {
                buffer.append(FormatJson(entry));
            } else {
                buffer.append(FormatCombined(entry));
            }
        }
        WriteBuffer(buffer);
    }

    {
        std::lock_guard<std::mutex> lock(output_mutex_);
        if (file_stream_.is_open()) {
            file_stream_.flush();
            file_stream_.close();
        }
    }

    running_.store(false, std::memory_order_release);
}

bool AccessLogger::Reopen(const std::string& new_path) {
    std::lock_guard<std::mutex> lock(output_mutex_);
    if (!new_path.empty()) {
        config_.output = new_path;
    }
    if (config_.output.empty() || config_.output == "stdout" || config_.output == "stderr") {
        return true;
    }
    if (file_stream_.is_open()) {
        file_stream_.flush();
        file_stream_.close();
    }
    file_stream_.clear();
    file_stream_.open(config_.output, std::ios::out | std::ios::app);
    return file_stream_.is_open();
}

void AccessLogger::Log(AccessLogEntry entry) {
    if (!running_.load(std::memory_order_relaxed)) return;

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (queue_.size() >= config_.queue_capacity) {
            total_dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        queue_.push_back(std::move(entry));
    }
    total_logged_.fetch_add(1, std::memory_order_relaxed);
    cv_.notify_one();
}

void AccessLogger::FlushLoop() {
    while (!stop_requested_.load(std::memory_order_relaxed)) {
        std::vector<AccessLogEntry> batch;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            cv_.wait_for(lock, config_.flush_interval, [this]() {
                return !queue_.empty() || stop_requested_.load(std::memory_order_relaxed);
            });

            if (queue_.empty()) {
                continue;
            }

            batch.swap(queue_);
        }

        if (!batch.empty()) {
            std::string buffer;
            buffer.reserve(batch.size() * 128);
            for (const auto& entry : batch) {
                if (config_.format == LogFormat::Json) {
                    buffer.append(FormatJson(entry));
                } else {
                    buffer.append(FormatCombined(entry));
                }
            }
            WriteBuffer(buffer);
        }
    }
}

void AccessLogger::WriteBuffer(const std::string& data) {
    if (data.empty()) return;

    std::lock_guard<std::mutex> lock(output_mutex_);
    if (config_.output == "stdout") {
        std::cout << data;
        std::cout.flush();
    } else if (config_.output == "stderr") {
        std::cerr << data;
        std::cerr.flush();
    } else if (file_stream_.is_open()) {
        file_stream_ << data;
        file_stream_.flush();
    }
}

std::string AccessLogger::FormatIso8601(std::chrono::system_clock::time_point tp) {
    std::time_t tt = std::chrono::system_clock::to_time_t(tp);
    std::tm tm_buf{};
    gmtime_r(&tt, &tm_buf);

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()) % 1000;

    std::ostringstream ss;
    ss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%S")
       << '.' << std::setfill('0') << std::setw(3) << ms.count() << 'Z';
    return ss.str();
}

std::string AccessLogger::FormatJson(const AccessLogEntry& entry) {
    std::string out;
    out.reserve(256);
    out.append("{\"time\":\"");
    out.append(FormatIso8601(entry.timestamp));
    out.append("\",\"client\":\"");
    out.append(entry.client_ip);
    if (entry.client_port > 0) {
        out.append(":");
        out.append(std::to_string(entry.client_port));
    }
    out.append("\",\"method\":\"");
    out.append(entry.method);
    out.append("\",\"uri\":\"");
    out.append(entry.uri);
    out.append("\",\"version\":\"");
    out.append(entry.http_version);
    out.append("\",\"status\":");
    out.append(std::to_string(entry.status_code));
    out.append(",\"bytes_sent\":");
    out.append(std::to_string(entry.bytes_sent));
    out.append(",\"bytes_received\":");
    out.append(std::to_string(entry.bytes_received));
    out.append(",\"duration_us\":");
    out.append(std::to_string(entry.duration_us.count()));
    out.append(",\"upstream\":\"");
    out.append(entry.upstream_endpoint);
    out.append("\"");
    if (entry.upstream_duration_us.count() > 0) {
        out.append(",\"upstream_duration_us\":");
        out.append(std::to_string(entry.upstream_duration_us.count()));
    }
    if (!entry.matched_pool.empty()) {
        out.append(",\"pool\":\"");
        out.append(entry.matched_pool);
        out.append("\"");
    }
    if (!entry.request_id.empty()) {
        out.append(",\"request_id\":\"");
        out.append(entry.request_id);
        out.append("\"");
    }
    out.append("}\n");
    return out;
}

std::string AccessLogger::FormatCombined(const AccessLogEntry& entry) {
    std::time_t tt = std::chrono::system_clock::to_time_t(entry.timestamp);
    std::tm tm_buf{};
    gmtime_r(&tt, &tm_buf);

    char time_str[64];
    std::strftime(time_str, sizeof(time_str), "%d/%b/%Y:%H:%M:%S +0000", &tm_buf);

    std::string out;
    out.reserve(256);
    out.append(entry.client_ip);
    out.append(" - - [");
    out.append(time_str);
    out.append("] \"");
    out.append(entry.method);
    out.append(" ");
    out.append(entry.uri);
    out.append(" ");
    out.append(entry.http_version);
    out.append("\" ");
    out.append(std::to_string(entry.status_code));
    out.append(" ");
    out.append(std::to_string(entry.bytes_sent));
    out.append(" ");
    out.append(std::to_string(entry.duration_us.count()));
    out.append("us [");
    out.append(entry.upstream_endpoint);
    out.append("]\n");
    return out;
}

} // namespace helios
