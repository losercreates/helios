#include <gtest/gtest.h>
#include "net/l4_proxy_server.hpp"
#include "worker.hpp"
#include "metrics/metrics_collector.hpp"

#include <openssl/ssl.h>
#include <openssl/err.h>
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

// Lightweight HTTP backend for integration testing
class TestHttpBackend {
public:
    TestHttpBackend() = default;
    ~TestHttpBackend() { Stop(); }

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
        tv.tv_usec = 25000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        listen(sock, 64);
        listen_fd_.store(sock);
        running_ = true;
        worker_ = std::thread([this]() {
            while (running_) {
                int sfd = listen_fd_.load();
                if (sfd < 0) break;
                sockaddr_in client_addr{};
                socklen_t clen = sizeof(client_addr);
                int cfd = accept(sfd, reinterpret_cast<sockaddr*>(&client_addr), &clen);
                if (cfd < 0) continue;

                char buf[8192];
                ssize_t n = recv(cfd, buf, sizeof(buf) - 1, 0);
                if (n > 0) {
                    buf[n] = '\0';
                    {
                        std::lock_guard<std::mutex> lock(req_mutex_);
                        last_request_ = std::string(buf, static_cast<size_t>(n));
                    }
                    request_count_++;
                    std::string resp =
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: text/plain\r\n"
                        "Content-Length: 13\r\n"
                        "Connection: close\r\n\r\n"
                        "Hello, HTTPS!";
                    send(cfd, resp.data(), resp.size(), MSG_NOSIGNAL);
                }
                close(cfd);
            }
        });
        return true;
    }

    void Stop() {
        if (running_) {
            running_ = false;
            int fd = listen_fd_.exchange(-1);
            if (fd >= 0) {
                shutdown(fd, SHUT_RDWR);
                close(fd);
            }
            if (worker_.joinable()) {
                worker_.join();
            }
        }
    }

    [[nodiscard]] uint16_t GetPort() const noexcept { return port_; }
    [[nodiscard]] size_t GetRequestCount() const noexcept { return request_count_.load(); }
    [[nodiscard]] std::string GetLastRequest() {
        std::lock_guard<std::mutex> lock(req_mutex_);
        return last_request_;
    }

private:
    std::atomic<int> listen_fd_{-1};
    uint16_t port_{0};
    std::atomic<bool> running_{false};
    std::atomic<size_t> request_count_{0};
    std::mutex req_mutex_;
    std::string last_request_;
    std::thread worker_;
};

} // namespace

class TlsIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        stop_proxy_ = false;
        cert_path_ = FindCertFile("server.crt");
        key_path_ = FindCertFile("server.key");
    }

    void TearDown() override {
        StopProxyLoop();
    }

    void StartProxyLoop(L4ProxyServer& proxy) {
        stop_proxy_ = false;
        proxy_thread_ = std::thread([&proxy, this]() {
            while (!stop_proxy_) {
                proxy.RunOnce(std::chrono::milliseconds(1));
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            proxy.Stop();
        });
    }

    void StopProxyLoop() {
        if (proxy_thread_.joinable()) {
            stop_proxy_ = true;
            proxy_thread_.join();
        }
    }

    int ConnectToProxy(uint16_t port) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        EXPECT_GT(fd, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            close(fd);
            return -1;
        }
        return fd;
    }

    std::string cert_path_;
    std::string key_path_;
    std::atomic<bool> stop_proxy_{false};
    std::thread proxy_thread_;
};

// 1. Full HTTPS Proxying: TLS Handshake, ALPN, Request Decryption, Upstream Forwarding, Response Encryption
TEST_F(TlsIntegrationTest, TlsTerminationSuccessfulProxying) {
    TestHttpBackend backend;
    ASSERT_TRUE(backend.Start());

    auto metrics = std::make_shared<WorkerMetrics>(0);

    L4ProxyServer::Config config;
    config.listen_port = 0;
    config.enable_l7_routing = true;
    config.enable_tls = true;
    config.tls_config.enabled = true;
    config.tls_config.cert_file = cert_path_;
    config.tls_config.key_file = key_path_;
    config.tls_config.alpn_protocols = {"http/1.1"};

    L4ProxyServer proxy(config);
    proxy.SetMetrics(metrics);

    // Setup L7 route
    proxy.AddNamedPool("backend_pool", "round_robin");
    proxy.AddBackendToNamedPool("backend_pool", "127.0.0.1", backend.GetPort());
    L7RouteConfig route;
    route.path_prefix = "/";
    route.target_pool = "backend_pool";
    proxy.AddRoute(route);

    ASSERT_TRUE(proxy.Start());
    StartProxyLoop(proxy);

    // Connect via OpenSSL TLS Client
    SSL_CTX* client_ctx = SSL_CTX_new(TLS_client_method());
    ASSERT_NE(client_ctx, nullptr);
    SSL_CTX_set_verify(client_ctx, SSL_VERIFY_NONE, nullptr);

    // Set ALPN http/1.1
    unsigned char alpn[] = { 8, 'h', 't', 't', 'p', '/', '1', '.', '1' };
    SSL_CTX_set_alpn_protos(client_ctx, alpn, sizeof(alpn));

    int sock_fd = ConnectToProxy(proxy.GetPort());
    ASSERT_GT(sock_fd, 0);

    SSL* ssl = SSL_new(client_ctx);
    ASSERT_NE(ssl, nullptr);
    SSL_set_fd(ssl, sock_fd);
    SSL_ctrl(ssl, SSL_CTRL_SET_TLSEXT_HOSTNAME, TLSEXT_NAMETYPE_host_name, const_cast<char*>("localhost"));

    int ret = SSL_connect(ssl);
    ASSERT_EQ(ret, 1);

    // Verify ALPN negotiation
    const unsigned char* alpn_res = nullptr;
    unsigned int alpn_len = 0;
    SSL_get0_alpn_selected(ssl, &alpn_res, &alpn_len);
    if (alpn_res && alpn_len > 0) {
        std::string negotiated(reinterpret_cast<const char*>(alpn_res), alpn_len);
        EXPECT_EQ(negotiated, "http/1.1");
    }

    // Send HTTPS GET request
    std::string req = "GET /test HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    int written = SSL_write(ssl, req.data(), static_cast<int>(req.size()));
    EXPECT_EQ(written, static_cast<int>(req.size()));

    // Read response over TLS
    char buf[4096];
    int n = SSL_read(ssl, buf, sizeof(buf) - 1);
    ASSERT_GT(n, 0);
    buf[n] = '\0';
    std::string resp(buf, static_cast<size_t>(n));

    EXPECT_NE(resp.find("200 OK"), std::string::npos);
    EXPECT_NE(resp.find("Hello, HTTPS!"), std::string::npos);

    // Close SSL session
    SSL_shutdown(ssl);
    SSL_free(ssl);
    SSL_CTX_free(client_ctx);
    close(sock_fd);

    // Verify upstream backend received X-Forwarded-Proto: https
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(backend.GetRequestCount(), 1u);
    std::string upstream_req = backend.GetLastRequest();
    EXPECT_NE(upstream_req.find("X-Forwarded-Proto: https"), std::string::npos);

    // Verify TLS Prometheus metrics
    EXPECT_GE(metrics->tls_handshakes_total.load(), 1u);
    EXPECT_EQ(metrics->tls_handshake_errors_total.load(), 0u);
    EXPECT_EQ(metrics->tls_active_connections.load(), 0u);
    StopProxyLoop();
}

// 2. Plaintext Client Rejected on TLS Termination Port
TEST_F(TlsIntegrationTest, TlsNonTlsClientRejectedGracefully) {
    L4ProxyServer::Config config;
    config.listen_port = 0;
    config.enable_l7_routing = true;
    config.enable_tls = true;
    config.tls_config.enabled = true;
    config.tls_config.cert_file = cert_path_;
    config.tls_config.key_file = key_path_;

    auto metrics = std::make_shared<WorkerMetrics>(0);
    L4ProxyServer proxy(config);
    proxy.SetMetrics(metrics);
    ASSERT_TRUE(proxy.Start());
    StartProxyLoop(proxy);

    int sock_fd = ConnectToProxy(proxy.GetPort());
    ASSERT_GT(sock_fd, 0);

    // Send plaintext HTTP directly to TLS port
    std::string plain_req = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    send(sock_fd, plain_req.data(), plain_req.size(), 0);

    // Server should reject TLS handshake and close connection
    char buf[256];
    ssize_t n = recv(sock_fd, buf, sizeof(buf), 0);
    EXPECT_LE(n, 0);
    close(sock_fd);

    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_GE(metrics->tls_handshake_errors_total.load(), 1u);
    EXPECT_EQ(metrics->tls_active_connections.load(), 0u);
    StopProxyLoop();
}

// 3. Encrypted Direct 404 Response over TLS
TEST_F(TlsIntegrationTest, TlsDirectErrorResponseEncrypted) {
    L4ProxyServer::Config config;
    config.listen_port = 0;
    config.enable_l7_routing = true;
    config.enable_tls = true;
    config.tls_config.enabled = true;
    config.tls_config.cert_file = cert_path_;
    config.tls_config.key_file = key_path_;

    L4ProxyServer proxy(config);
    ASSERT_TRUE(proxy.Start());
    StartProxyLoop(proxy);

    SSL_CTX* client_ctx = SSL_CTX_new(TLS_client_method());
    ASSERT_NE(client_ctx, nullptr);
    SSL_CTX_set_verify(client_ctx, SSL_VERIFY_NONE, nullptr);

    int sock_fd = ConnectToProxy(proxy.GetPort());
    ASSERT_GT(sock_fd, 0);

    SSL* ssl = SSL_new(client_ctx);
    SSL_set_fd(ssl, sock_fd);
    int ret = SSL_connect(ssl);
    ASSERT_EQ(ret, 1);

    // Send request with no matching route
    std::string req = "GET /nonexistent HTTP/1.1\r\nHost: unknown.com\r\nConnection: close\r\n\r\n";
    SSL_write(ssl, req.data(), static_cast<int>(req.size()));

    char buf[4096];
    int n = SSL_read(ssl, buf, sizeof(buf) - 1);
    ASSERT_GT(n, 0);
    buf[n] = '\0';
    std::string resp(buf, static_cast<size_t>(n));

    // Verify 404 Not Found was received and decrypted over TLS
    EXPECT_NE(resp.find("404 Not Found"), std::string::npos);

    SSL_shutdown(ssl);
    SSL_free(ssl);
    SSL_CTX_free(client_ctx);
    close(sock_fd);
    StopProxyLoop();
}

// 4. WorkerPool Multi-Worker HTTPS Termination
TEST_F(TlsIntegrationTest, WorkerPoolTlsTermination) {
    TestHttpBackend backend;
    ASSERT_TRUE(backend.Start());

    WorkerPoolConfig config;
    config.num_workers = 2;
    config.listen_port = 0;
    config.tls.enabled = true;
    config.tls.cert_file = cert_path_;
    config.tls.key_file = key_path_;

    WorkerPool pool(config);
    pool.AddBackend("127.0.0.1", backend.GetPort());
    ASSERT_TRUE(pool.Start());

    uint16_t port = pool.GetPort();
    ASSERT_GT(port, 0);

    // Send requests to pool
    for (int i = 0; i < 4; ++i) {
        SSL_CTX* client_ctx = SSL_CTX_new(TLS_client_method());
        ASSERT_NE(client_ctx, nullptr);
        SSL_CTX_set_verify(client_ctx, SSL_VERIFY_NONE, nullptr);

        int sock_fd = ConnectToProxy(port);
        ASSERT_GT(sock_fd, 0);

        SSL* ssl = SSL_new(client_ctx);
        SSL_set_fd(ssl, sock_fd);
        ASSERT_EQ(SSL_connect(ssl), 1);

        std::string req = "GET /pool HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        SSL_write(ssl, req.data(), static_cast<int>(req.size()));

        char buf[2048];
        int n = SSL_read(ssl, buf, sizeof(buf) - 1);
        ASSERT_GT(n, 0);
        buf[n] = '\0';
        std::string resp(buf, static_cast<size_t>(n));
        EXPECT_NE(resp.find("200 OK"), std::string::npos);

        SSL_shutdown(ssl);
        SSL_free(ssl);
        SSL_CTX_free(client_ctx);
        close(sock_fd);
    }

    EXPECT_GE(backend.GetRequestCount(), 4u);
    pool.Stop();
    pool.Join();
}

// 5. Dynamic TLS Certificate & Configuration Hot-Reload (SIGHUP behavior)
TEST_F(TlsIntegrationTest, WorkerPoolSighupTlsHotReload) {
    TestHttpBackend backend;
    ASSERT_TRUE(backend.Start());

    // Generate alternate self-signed certificate with distinct CN
    std::string cert2_path = cert_path_ + ".v2.crt";
    std::string key2_path = key_path_ + ".v2.key";
    std::string cmd = "openssl req -x509 -newkey rsa:2048 -nodes -keyout " + key2_path +
                      " -out " + cert2_path + " -days 1 -subj '/CN=localhost-v2' "
                      "-addext 'subjectAltName=DNS:localhost,IP:127.0.0.1' 2>/dev/null";
    ASSERT_EQ(::system(cmd.c_str()), 0);

    WorkerPoolConfig config;
    config.num_workers = 2;
    config.listen_port = 0;
    config.tls.enabled = true;
    config.tls.cert_file = cert_path_;
    config.tls.key_file = key_path_;

    WorkerPool pool(config);
    pool.AddBackend("127.0.0.1", backend.GetPort());
    ASSERT_TRUE(pool.Start());

    uint16_t port = pool.GetPort();
    ASSERT_GT(port, 0);

    auto verify_server_cert_cn = [&](const std::string& expected_cn) {
        SSL_CTX* client_ctx = SSL_CTX_new(TLS_client_method());
        EXPECT_NE(client_ctx, nullptr);
        SSL_CTX_set_verify(client_ctx, SSL_VERIFY_NONE, nullptr);

        int sock_fd = ConnectToProxy(port);
        EXPECT_GT(sock_fd, 0);

        SSL* ssl = SSL_new(client_ctx);
        SSL_set_fd(ssl, sock_fd);
        EXPECT_EQ(SSL_connect(ssl), 1);

        X509* server_cert = SSL_get_peer_certificate(ssl);
        EXPECT_NE(server_cert, nullptr);
        if (server_cert) {
            char name_buf[256];
            X509_NAME_oneline(X509_get_subject_name(server_cert), name_buf, sizeof(name_buf));
            EXPECT_NE(std::string(name_buf).find(expected_cn), std::string::npos)
                << "Expected CN: " << expected_cn << " but got: " << name_buf;
            X509_free(server_cert);
        }

        std::string req = "GET /check HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        SSL_write(ssl, req.data(), static_cast<int>(req.size()));

        char buf[1024];
        int n = SSL_read(ssl, buf, sizeof(buf) - 1);
        EXPECT_GT(n, 0);
        if (n > 0) {
            buf[n] = '\0';
            EXPECT_NE(std::string(buf).find("200 OK"), std::string::npos);
        }

        SSL_shutdown(ssl);
        SSL_free(ssl);
        SSL_CTX_free(client_ctx);
        close(sock_fd);
    };

    // Phase 1: Verify initial certificate CN (localhost)
    verify_server_cert_cn("localhost");

    // Phase 2: Dynamic hot-reload with Certificate v2
    TlsConfig new_tls_config = config.tls;
    new_tls_config.cert_file = cert2_path;
    new_tls_config.key_file = key2_path;

    std::string err;
    ASSERT_TRUE(pool.ReloadTls(new_tls_config, &err)) << "Reload error: " << err;

    // Phase 3: Verify new connections receive renewed certificate (localhost-v2)
    verify_server_cert_cn("localhost-v2");

    // Phase 4: Negative test - invalid reload must fail and preserve active context
    TlsConfig bad_config = new_tls_config;
    bad_config.cert_file = "/nonexistent/invalid.crt";
    EXPECT_FALSE(pool.ReloadTls(bad_config, &err));
    EXPECT_FALSE(err.empty());

    // Phase 5: Verify proxy still functions with Certificate v2 without outage
    verify_server_cert_cn("localhost-v2");

    pool.Stop();
    pool.Join();

    std::remove(cert2_path.c_str());
    std::remove(key2_path.c_str());
}
