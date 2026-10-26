#pragma once

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <span>
#include <openssl/ssl.h>
#include <openssl/err.h>

namespace helios {

class TlsStream;

// Configuration settings for TLS termination
struct TlsConfig {
    bool enabled{false};
    uint16_t port{8443};
    std::string cert_file;
    std::string key_file;
    std::string min_version{"TLSv1.2"}; // "TLSv1.2" or "TLSv1.3"
    std::string ciphers{"ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305"};
    std::string ciphersuites{"TLS_AES_128_GCM_SHA256:TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256"};
    std::vector<std::string> alpn_protocols{"http/1.1"};
    bool prefer_server_ciphers{true};
};

// Thread-safe wrapper around OpenSSL SSL_CTX managing certificates, ciphers, and protocol options
class TlsContext {
public:
    explicit TlsContext(TlsConfig config);
    ~TlsContext();

    // Non-copyable
    TlsContext(const TlsContext&) = delete;
    TlsContext& operator=(const TlsContext&) = delete;

    // Movable
    TlsContext(TlsContext&& other) noexcept;
    TlsContext& operator=(TlsContext&& other) noexcept;

    // Initialize SSL_CTX with configured certificates and security parameters
    [[nodiscard]] bool Initialize(std::string* error_out = nullptr);

    // Create a new in-memory TlsStream connection session
    [[nodiscard]] std::unique_ptr<TlsStream> CreateStream() const;

    // Dynamic certificate and key reloading without restarting
    bool ReloadCertificates(const std::string& cert_file, const std::string& key_file, std::string* error_out = nullptr);

    // Dynamic full TLS configuration and certificate reloading (atomic swap)
    bool Reload(const TlsConfig& new_config, std::string* error_out = nullptr);

    [[nodiscard]] bool IsInitialized() const noexcept {
        std::lock_guard lock(mutex_);
        return ctx_ != nullptr;
    }

    [[nodiscard]] SSL_CTX* GetNativeHandle() const noexcept {
        std::lock_guard lock(mutex_);
        return ctx_;
    }

    [[nodiscard]] TlsConfig GetConfig() const noexcept {
        std::lock_guard lock(mutex_);
        return config_;
    }

    // Static helper to retrieve formatted OpenSSL error string
    static std::string GetLastOpenSslError();

private:
    static int AlpnSelectCallback(SSL* ssl, const unsigned char** out, unsigned char* outlen,
                                  const unsigned char* in, unsigned int inlen, void* arg);

    static SSL_CTX* CreateAndConfigureCtx(const TlsConfig& config,
                                          const std::vector<unsigned char>& wire_alpn,
                                          void* alpn_callback_arg,
                                          std::string* error_out);
    static std::vector<unsigned char> EncodeWireAlpn(const std::vector<std::string>& protocols);

    mutable std::mutex mutex_;
    TlsConfig config_;
    SSL_CTX* ctx_{nullptr};
    std::vector<unsigned char> wire_alpn_protocols_;
};

} // namespace helios
