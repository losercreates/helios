#include <gtest/gtest.h>
#include "net/l4_proxy_server.hpp"
#include "metrics/metrics_collector.hpp"

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <thread>
#include <chrono>
#include <string>
#include <memory>
#include <vector>
#include <atomic>
#include <mutex>

using namespace helios;

namespace {

std::string FindCertFile(const std::string& filename) {
#ifdef HELIOS_TEST_CERTS_DIR
    std::string p = std::string(HELIOS_TEST_CERTS_DIR) + "/" + filename;
    if (access(p.c_str(), R_OK) == 0) return p;
#endif
    std::vector<std::string> candidates = {
        "tests/test_certs/" + filename,
        "../tests/test_certs/" + filename,
        "../../tests/test_certs/" + filename
    };
    for (const auto& path : candidates) {
        if (access(path.c_str(), R_OK) == 0) {
            return path;
        }
    }
    return "tests/test_certs/" + filename;
}

std::string ComputeWebSocketAcceptKey(std::string_view client_key) {
    static constexpr std::string_view kMagicGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string combined = std::string(client_key) + std::string(kMagicGuid);

    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(combined.data()), combined.size(), hash);

    char b64[64];
    int b64_len = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(b64), hash, SHA_DIGEST_LENGTH);
    return std::string(b64, static_cast<size_t>(b64_len));
}

std::vector<uint8_t> MakeClientFrame(uint8_t opcode, std::string_view payload, uint32_t mask = 0x12345678) {
    std::vector<uint8_t> frame;
    frame.push_back(0x80 | (opcode & 0x0F)); // FIN + opcode
    size_t len = payload.size();
    if (len < 126) {
        frame.push_back(0x80 | static_cast<uint8_t>(len));
    } else {
        frame.push_back(0x80 | 126);
        frame.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
        frame.push_back(static_cast<uint8_t>(len & 0xFF));
    }
    uint8_t mask_bytes[4] = {
        static_cast<uint8_t>((mask >> 24) & 0xFF),
        static_cast<uint8_t>((mask >> 16) & 0xFF),
        static_cast<uint8_t>((mask >> 8) & 0xFF),
        static_cast<uint8_t>(mask & 0xFF)
    };
    for (int i = 0; i < 4; ++i) {
        frame.push_back(mask_bytes[i]);
    }
    for (size_t i = 0; i < payload.size(); ++i) {
        frame.push_back(static_cast<uint8_t>(payload[i]) ^ mask_bytes[i % 4]);
    }
    return frame;
}

std::vector<uint8_t> MakeServerFrame(uint8_t opcode, std::string_view payload) {
    std::vector<uint8_t> frame;
    frame.push_back(0x80 | (opcode & 0x0F)); // FIN + opcode
    size_t len = payload.size();
    if (len < 126) {
        frame.push_back(static_cast<uint8_t>(len));
    } else {
        frame.push_back(126);
        frame.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
        frame.push_back(static_cast<uint8_t>(len & 0xFF));
    }
    for (char c : payload) {
        frame.push_back(static_cast<uint8_t>(c));
    }
    return frame;
}

// RFC 6455 Mock WebSocket backend for end-to-end integration testing
class TestWebSocketBackend {
public:
    TestWebSocketBackend() = default;
    ~TestWebSocketBackend() { Stop(); }

    bool Start() {
        int sock = ::socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return false;
        int opt = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = 0;
        if (::bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            close(sock);
            return false;
        }
        socklen_t len = sizeof(addr);
        getsockname(sock, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);

        struct timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 25000; // 25ms timeout for clean worker shutdown
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        listen(sock, 64);
        listen_fd_.store(sock);
        running_ = true;
        worker_ = std::thread([this]() { RunWorker(); });
        return true;
    }

    void Stop() {
        running_ = false;
        int sfd = listen_fd_.exchange(-1);
        if (sfd >= 0) {
            close(sfd);
        }
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    [[nodiscard]] uint16_t GetPort() const noexcept { return port_; }
    [[nodiscard]] size_t GetUpgradeCount() const noexcept { return upgrade_count_.load(); }
    [[nodiscard]] size_t GetFramesEchoed() const noexcept { return frames_echoed_.load(); }

private:
    void RunWorker() {
        while (running_) {
            int sfd = listen_fd_.load();
            if (sfd < 0) break;
            sockaddr_in client_addr{};
            socklen_t clen = sizeof(client_addr);
            int cfd = accept(sfd, reinterpret_cast<sockaddr*>(&client_addr), &clen);
            if (cfd < 0) continue;

            // Set socket timeout for recv inside connection
            struct timeval tv{};
            tv.tv_sec = 1;
            tv.tv_usec = 0;
            setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

            // Read HTTP handshake
            std::string req;
            char buf[4096];
            while (running_) {
                ssize_t n = recv(cfd, buf, sizeof(buf), 0);
                if (n <= 0) break;
                req.append(buf, static_cast<size_t>(n));
                if (req.find("\r\n\r\n") != std::string::npos) break;
            }

            if (req.find("GET /reject") != std::string::npos) {
                // Reject route
                std::string resp =
                    "HTTP/1.1 404 Not Found\r\n"
                    "Content-Type: text/plain\r\n"
                    "Content-Length: 9\r\n"
                    "Connection: close\r\n\r\n"
                    "Not Found";
                send(cfd, resp.data(), resp.size(), MSG_NOSIGNAL);
                close(cfd);
                continue;
            }

            // Extract Sec-WebSocket-Key
            std::string key_prefix = "Sec-WebSocket-Key: ";
            auto key_pos = req.find(key_prefix);
            if (key_pos == std::string::npos) {
                // Not a websocket handshake
                std::string resp = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                send(cfd, resp.data(), resp.size(), MSG_NOSIGNAL);
                close(cfd);
                continue;
            }

            auto end_of_line = req.find("\r\n", key_pos);
            std::string key = req.substr(key_pos + key_prefix.size(), end_of_line - (key_pos + key_prefix.size()));
            std::string accept_val = ComputeWebSocketAcceptKey(key);

            std::string handshake_resp =
                "HTTP/1.1 101 Switching Protocols\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                "Sec-WebSocket-Accept: " + accept_val + "\r\n\r\n";

            send(cfd, handshake_resp.data(), handshake_resp.size(), MSG_NOSIGNAL);
            upgrade_count_++;

            // Handle WebSocket frames loop
            std::vector<uint8_t> frame_buf;
            while (running_) {
                ssize_t n = recv(cfd, buf, sizeof(buf), 0);
                if (n <= 0) break;
                frame_buf.insert(frame_buf.end(), buf, buf + n);

                while (frame_buf.size() >= 2) {
                    uint8_t b0 = frame_buf[0];
                    uint8_t b1 = frame_buf[1];
                    uint8_t opcode = b0 & 0x0F;
                    bool masked = (b1 & 0x80) != 0;
                    size_t payload_len = b1 & 0x7F;
                    size_t header_len = 2;

                    if (payload_len == 126) {
                        if (frame_buf.size() < 4) break;
                        payload_len = (static_cast<size_t>(frame_buf[2]) << 8) | frame_buf[3];
                        header_len = 4;
                    }

                    if (masked) header_len += 4;
                    if (frame_buf.size() < header_len + payload_len) break;

                    uint8_t mask_key[4] = {0, 0, 0, 0};
                    if (masked) {
                        mask_key[0] = frame_buf[header_len - 4];
                        mask_key[1] = frame_buf[header_len - 3];
                        mask_key[2] = frame_buf[header_len - 2];
                        mask_key[3] = frame_buf[header_len - 1];
                    }

                    std::string payload;
                    payload.resize(payload_len);
                    for (size_t i = 0; i < payload_len; ++i) {
                        uint8_t byte = frame_buf[header_len + i];
                        if (masked) {
                            byte ^= mask_key[i % 4];
                        }
                        payload[i] = static_cast<char>(byte);
                    }

                    frame_buf.erase(frame_buf.begin(), frame_buf.begin() + static_cast<std::ptrdiff_t>(header_len + payload_len));

                    if (opcode == 0x8) {
                        // Close frame: echo close frame and exit
                        auto close_resp = MakeServerFrame(0x8, payload);
                        send(cfd, close_resp.data(), close_resp.size(), MSG_NOSIGNAL);
                        goto conn_done;
                    } else if (opcode == 0x1 || opcode == 0x2) {
                        // Echo text or binary frame
                        auto echo_resp = MakeServerFrame(opcode, payload);
                        send(cfd, echo_resp.data(), echo_resp.size(), MSG_NOSIGNAL);
                        frames_echoed_++;
                    }
                }
            }
conn_done:
            close(cfd);
        }
    }

    std::atomic<int> listen_fd_{-1};
    uint16_t port_{0};
    std::atomic<bool> running_{false};
    std::thread worker_;
    std::atomic<size_t> upgrade_count_{0};
    std::atomic<size_t> frames_echoed_{0};
};

} // namespace

// ============================================================================
// 1. Plain TCP WebSocket Passthrough (ws://)
// ============================================================================

TEST(WebSocketIntegration, PlainTcpWebSocketBidirectionalPassthrough) {
    TestWebSocketBackend backend;
    ASSERT_TRUE(backend.Start());

    auto metrics = std::make_shared<WorkerMetrics>(0);
    L4ProxyServer::Config cfg;
    cfg.listen_host = "127.0.0.1";
    cfg.listen_port = 0;
    cfg.enable_l7_routing = true;

    L7RouteConfig route;
    route.name = "ws-route";
    route.target_pool = "ws-pool";
    route.path_prefix = "/ws";
    route.strip_prefix = false;
    cfg.routes.push_back(route);

    L4ProxyServer server(cfg);
    server.SetMetrics(metrics);
    server.AddNamedPool("ws-pool", "round_robin");
    server.AddBackendToNamedPool("ws-pool", "127.0.0.1", backend.GetPort());
    ASSERT_TRUE(server.Start());

    std::atomic<bool> running{true};
    std::thread server_thread([&]() {
        while (running) {
            server.RunOnce(std::chrono::milliseconds(10));
        }
    });

    uint16_t proxy_port = server.GetPort();
    ASSERT_GT(proxy_port, 0);

    // Client connects to Helios proxy
    int client_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(client_fd, 0);

    sockaddr_in paddr{};
    paddr.sin_family = AF_INET;
    paddr.sin_addr.s_addr = inet_addr("127.0.0.1");
    paddr.sin_port = htons(proxy_port);
    ASSERT_EQ(::connect(client_fd, reinterpret_cast<sockaddr*>(&paddr), sizeof(paddr)), 0);

    // Send WebSocket handshake request
    std::string handshake_req =
        "GET /ws HTTP/1.1\r\n"
        "Host: localhost:" + std::to_string(proxy_port) + "\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";

    ssize_t sent = ::send(client_fd, handshake_req.data(), handshake_req.size(), 0);
    ASSERT_EQ(sent, static_cast<ssize_t>(handshake_req.size()));

    // Read 101 Switching Protocols response
    char resp_buf[2048];
    std::string resp;
    while (resp.find("\r\n\r\n") == std::string::npos) {
        ssize_t n = ::recv(client_fd, resp_buf, sizeof(resp_buf), 0);
        ASSERT_GT(n, 0);
        resp.append(resp_buf, static_cast<size_t>(n));
    }

    EXPECT_NE(resp.find("101 Switching Protocols"), std::string::npos);
    EXPECT_NE(resp.find("Upgrade: websocket"), std::string::npos);
    EXPECT_NE(resp.find("Sec-WebSocket-Accept: "), std::string::npos);

    // Send text WebSocket frame from client
    std::string text_msg = "Helios Zero-Copy WebSocket Streaming!";
    auto client_frame = MakeClientFrame(0x1, text_msg);
    sent = ::send(client_fd, client_frame.data(), client_frame.size(), 0);
    ASSERT_EQ(sent, static_cast<ssize_t>(client_frame.size()));

    // Read echoed server frame
    std::vector<uint8_t> echo_buf(256);
    ssize_t recvd = ::recv(client_fd, echo_buf.data(), echo_buf.size(), 0);
    ASSERT_GE(recvd, static_cast<ssize_t>(text_msg.size() + 2));

    EXPECT_EQ(echo_buf[0], 0x81); // FIN + Text frame
    size_t payload_len = echo_buf[1] & 0x7F;
    EXPECT_EQ(payload_len, text_msg.size());
    std::string echoed_payload(reinterpret_cast<char*>(echo_buf.data() + 2), payload_len);
    EXPECT_EQ(echoed_payload, text_msg);

    // Send binary WebSocket frame
    std::string binary_msg = "\x01\x02\x03\x04\xDE\xAD\xBE\xEF";
    auto binary_frame = MakeClientFrame(0x2, binary_msg);
    sent = ::send(client_fd, binary_frame.data(), binary_frame.size(), 0);
    ASSERT_EQ(sent, static_cast<ssize_t>(binary_frame.size()));

    recvd = ::recv(client_fd, echo_buf.data(), echo_buf.size(), 0);
    ASSERT_GE(recvd, static_cast<ssize_t>(binary_msg.size() + 2));
    EXPECT_EQ(echo_buf[0], 0x82); // FIN + Binary frame
    std::string echoed_bin(reinterpret_cast<char*>(echo_buf.data() + 2), binary_msg.size());
    EXPECT_EQ(echoed_bin, binary_msg);

    // Send close frame
    auto close_frame = MakeClientFrame(0x8, "");
    ::send(client_fd, close_frame.data(), close_frame.size(), 0);
    ::close(client_fd);

    // Wait for worker metrics update
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(metrics->websocket_upgrades_total.load(), 1u);
    EXPECT_EQ(backend.GetUpgradeCount(), 1u);
    EXPECT_EQ(backend.GetFramesEchoed(), 2u);

    running = false;
    if (server_thread.joinable()) {
        server_thread.join();
    }
    server.Stop();
}

// ============================================================================
// 2. TLS Terminated WebSocket Passthrough (wss://)
// ============================================================================

TEST(WebSocketIntegration, TlsTerminatedWebSocketPassthrough) {
    TestWebSocketBackend backend;
    ASSERT_TRUE(backend.Start());

    auto metrics = std::make_shared<WorkerMetrics>(0);
    L4ProxyServer::Config cfg;
    cfg.listen_host = "127.0.0.1";
    cfg.listen_port = 0;
    cfg.enable_l7_routing = true;
    cfg.enable_tls = true;
    cfg.tls_config.cert_file = FindCertFile("server.crt");
    cfg.tls_config.key_file = FindCertFile("server.key");

    L7RouteConfig route;
    route.name = "wss-route";
    route.target_pool = "wss-pool";
    route.path_prefix = "/ws";
    route.strip_prefix = false;
    cfg.routes.push_back(route);

    L4ProxyServer server(cfg);
    server.SetMetrics(metrics);
    server.AddNamedPool("wss-pool", "round_robin");
    server.AddBackendToNamedPool("wss-pool", "127.0.0.1", backend.GetPort());
    ASSERT_TRUE(server.Start());

    std::atomic<bool> running{true};
    std::thread server_thread([&]() {
        while (running) {
            server.RunOnce(std::chrono::milliseconds(10));
        }
    });

    uint16_t proxy_port = server.GetPort();
    ASSERT_GT(proxy_port, 0);

    // Initialize OpenSSL client
    const SSL_METHOD* method = TLS_client_method();
    SSL_CTX* ctx = SSL_CTX_new(method);
    ASSERT_NE(ctx, nullptr);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);

    int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(sock, 0);

    sockaddr_in paddr{};
    paddr.sin_family = AF_INET;
    paddr.sin_addr.s_addr = inet_addr("127.0.0.1");
    paddr.sin_port = htons(proxy_port);
    ASSERT_EQ(::connect(sock, reinterpret_cast<sockaddr*>(&paddr), sizeof(paddr)), 0);

    SSL* ssl = SSL_new(ctx);
    ASSERT_NE(ssl, nullptr);
    SSL_set_fd(ssl, sock);
    ASSERT_EQ(SSL_connect(ssl), 1);

    // Send WebSocket handshake request over TLS
    std::string handshake_req =
        "GET /ws HTTP/1.1\r\n"
        "Host: localhost:" + std::to_string(proxy_port) + "\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: sN92ja3L0kG3+Z21q3vL8Q==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";

    int sent = SSL_write(ssl, handshake_req.data(), static_cast<int>(handshake_req.size()));
    ASSERT_EQ(sent, static_cast<int>(handshake_req.size()));

    // Read 101 Switching Protocols response over TLS
    char resp_buf[2048];
    std::string resp;
    while (resp.find("\r\n\r\n") == std::string::npos) {
        int n = SSL_read(ssl, resp_buf, sizeof(resp_buf));
        ASSERT_GT(n, 0);
        resp.append(resp_buf, static_cast<size_t>(n));
    }

    EXPECT_NE(resp.find("101 Switching Protocols"), std::string::npos);
    EXPECT_NE(resp.find("Upgrade: websocket"), std::string::npos);

    // Send text WebSocket frame over TLS
    std::string wss_msg = "Secure WSS message over Helios TLS termination";
    auto client_frame = MakeClientFrame(0x1, wss_msg);
    sent = SSL_write(ssl, client_frame.data(), static_cast<int>(client_frame.size()));
    ASSERT_EQ(sent, static_cast<int>(client_frame.size()));

    // Read echoed frame over TLS
    std::vector<uint8_t> echo_buf(512);
    int recvd = SSL_read(ssl, echo_buf.data(), static_cast<int>(echo_buf.size()));
    ASSERT_GE(recvd, static_cast<int>(wss_msg.size() + 2));

    EXPECT_EQ(echo_buf[0], 0x81);
    size_t payload_len = echo_buf[1] & 0x7F;
    EXPECT_EQ(payload_len, wss_msg.size());
    std::string echoed_payload(reinterpret_cast<char*>(echo_buf.data() + 2), payload_len);
    EXPECT_EQ(echoed_payload, wss_msg);

    // Shutdown TLS and close
    SSL_shutdown(ssl);
    SSL_free(ssl);
    ::close(sock);
    SSL_CTX_free(ctx);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(metrics->websocket_upgrades_total.load(), 1u);
    EXPECT_EQ(backend.GetUpgradeCount(), 1u);
    EXPECT_EQ(backend.GetFramesEchoed(), 1u);

    running = false;
    if (server_thread.joinable()) {
        server_thread.join();
    }
    server.Stop();
}

// ============================================================================
// 3. Upstream Rejection Test (Backend returns 404)
// ============================================================================

TEST(WebSocketIntegration, UpstreamRejectionPassthrough) {
    TestWebSocketBackend backend;
    ASSERT_TRUE(backend.Start());

    auto metrics = std::make_shared<WorkerMetrics>(0);
    L4ProxyServer::Config cfg;
    cfg.listen_host = "127.0.0.1";
    cfg.listen_port = 0;
    cfg.enable_l7_routing = true;

    L7RouteConfig route;
    route.name = "reject-route";
    route.target_pool = "reject-pool";
    route.path_prefix = "/reject";
    route.strip_prefix = false;
    cfg.routes.push_back(route);

    L4ProxyServer server(cfg);
    server.SetMetrics(metrics);
    server.AddNamedPool("reject-pool", "round_robin");
    server.AddBackendToNamedPool("reject-pool", "127.0.0.1", backend.GetPort());
    ASSERT_TRUE(server.Start());

    std::atomic<bool> running{true};
    std::thread server_thread([&]() {
        while (running) {
            server.RunOnce(std::chrono::milliseconds(10));
        }
    });

    uint16_t proxy_port = server.GetPort();
    ASSERT_GT(proxy_port, 0);

    int client_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(client_fd, 0);

    sockaddr_in paddr{};
    paddr.sin_family = AF_INET;
    paddr.sin_addr.s_addr = inet_addr("127.0.0.1");
    paddr.sin_port = htons(proxy_port);
    ASSERT_EQ(::connect(client_fd, reinterpret_cast<sockaddr*>(&paddr), sizeof(paddr)), 0);

    std::string handshake_req =
        "GET /reject HTTP/1.1\r\n"
        "Host: localhost:" + std::to_string(proxy_port) + "\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";

    ::send(client_fd, handshake_req.data(), handshake_req.size(), 0);

    char resp_buf[2048];
    std::string resp;
    while (resp.find("\r\n\r\n") == std::string::npos) {
        ssize_t n = ::recv(client_fd, resp_buf, sizeof(resp_buf), 0);
        if (n <= 0) break;
        resp.append(resp_buf, static_cast<size_t>(n));
    }
    ::close(client_fd);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Should receive 404 Not Found from backend
    EXPECT_NE(resp.find("404 Not Found"), std::string::npos);
    // Upgrades metric should NOT increment
    EXPECT_EQ(metrics->websocket_upgrades_total.load(), 0u);

    running = false;
    if (server_thread.joinable()) {
        server_thread.join();
    }
    server.Stop();
}

// ============================================================================
// 4. Connection Pooling Exclusion
// ============================================================================

TEST(WebSocketIntegration, WebSocketConnectionPoolingExclusion) {
    TestWebSocketBackend backend;
    ASSERT_TRUE(backend.Start());

    auto metrics = std::make_shared<WorkerMetrics>(0);
    L4ProxyServer::Config cfg;
    cfg.listen_host = "127.0.0.1";
    cfg.listen_port = 0;
    cfg.enable_l7_routing = true;

    L7RouteConfig route;
    route.name = "pool-route";
    route.target_pool = "pool-pool";
    route.path_prefix = "/ws";
    route.strip_prefix = false;
    cfg.routes.push_back(route);

    BackendConnectionPoolConfig pool_cfg;
    pool_cfg.max_idle_per_backend = 10;
    pool_cfg.idle_timeout = std::chrono::milliseconds(5000);

    L4ProxyServer server(cfg);
    server.SetMetrics(metrics);
    // Enable connection pooling on this pool
    server.AddNamedPool("pool-pool", "round_robin", true, pool_cfg);
    server.AddBackendToNamedPool("pool-pool", "127.0.0.1", backend.GetPort());
    ASSERT_TRUE(server.Start());

    std::atomic<bool> running{true};
    std::thread server_thread([&]() {
        while (running) {
            server.RunOnce(std::chrono::milliseconds(10));
        }
    });

    uint16_t proxy_port = server.GetPort();
    ASSERT_GT(proxy_port, 0);

    int client_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(client_fd, 0);

    sockaddr_in paddr{};
    paddr.sin_family = AF_INET;
    paddr.sin_addr.s_addr = inet_addr("127.0.0.1");
    paddr.sin_port = htons(proxy_port);
    ASSERT_EQ(::connect(client_fd, reinterpret_cast<sockaddr*>(&paddr), sizeof(paddr)), 0);

    std::string handshake_req =
        "GET /ws HTTP/1.1\r\n"
        "Host: localhost:" + std::to_string(proxy_port) + "\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";

    ::send(client_fd, handshake_req.data(), handshake_req.size(), 0);

    char resp_buf[2048];
    std::string resp;
    while (resp.find("\r\n\r\n") == std::string::npos) {
        ssize_t n = ::recv(client_fd, resp_buf, sizeof(resp_buf), 0);
        ASSERT_GT(n, 0);
        resp.append(resp_buf, static_cast<size_t>(n));
    }
    EXPECT_NE(resp.find("101 Switching Protocols"), std::string::npos);

    // Send text frame
    auto frame = MakeClientFrame(0x1, "Testing pool exclusion");
    ::send(client_fd, frame.data(), frame.size(), 0);

    std::vector<uint8_t> echo_buf(256);
    ssize_t recvd = ::recv(client_fd, echo_buf.data(), echo_buf.size(), 0);
    ASSERT_GT(recvd, 0);

    // Close client connection
    ::close(client_fd);

    // Allow event loop to process closure
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Verify pool has NOT recycled this upgraded socket (idle count should be 0)
    const auto& pools = server.GetNamedPools();
    auto it = pools.find("pool-pool");
    ASSERT_NE(it, pools.end());
    ASSERT_NE(it->second.pool, nullptr);
    EXPECT_EQ(it->second.pool->TotalIdleCount(), 0u);

    running = false;
    if (server_thread.joinable()) {
        server_thread.join();
    }
    server.Stop();
}
