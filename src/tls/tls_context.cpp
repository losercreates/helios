#include "tls_context.hpp"
#include "tls_stream.hpp"
#include <iostream>
#include <sstream>

namespace helios {

std::string TlsContext::GetLastOpenSslError() {
    unsigned long err_code = ERR_get_error();
    if (err_code == 0) {
        return "Unknown OpenSSL error";
    }
    char buf[256];
    ERR_error_string_n(err_code, buf, sizeof(buf));
    return std::string(buf);
}

int TlsContext::AlpnSelectCallback(SSL* /*ssl*/, const unsigned char** out, unsigned char* outlen,
                                   const unsigned char* in, unsigned int inlen, void* arg) {
    auto* self = static_cast<TlsContext*>(arg);
    if (!self || self->wire_alpn_protocols_.empty()) {
        return SSL_TLSEXT_ERR_NOACK;
    }

    if (SSL_select_next_proto(const_cast<unsigned char**>(out), outlen,
                              self->wire_alpn_protocols_.data(),
                              static_cast<unsigned int>(self->wire_alpn_protocols_.size()),
                              in, inlen) != OPENSSL_NPN_NEGOTIATED) {
        return SSL_TLSEXT_ERR_NOACK;
    }
    return SSL_TLSEXT_ERR_OK;
}

std::vector<unsigned char> TlsContext::EncodeWireAlpn(const std::vector<std::string>& protocols) {
    std::vector<unsigned char> wire;
    for (const auto& proto : protocols) {
        if (!proto.empty() && proto.size() <= 255) {
            wire.push_back(static_cast<unsigned char>(proto.size()));
            wire.insert(wire.end(), proto.begin(), proto.end());
        }
    }
    return wire;
}

TlsContext::TlsContext(TlsConfig config)
    : config_(std::move(config)),
      wire_alpn_protocols_(EncodeWireAlpn(config_.alpn_protocols)) {}

TlsContext::~TlsContext() {
    if (ctx_) {
        SSL_CTX_free(ctx_);
        ctx_ = nullptr;
    }
}

TlsContext::TlsContext(TlsContext&& other) noexcept {
    std::unique_lock lock(other.mutex_);
    config_ = std::move(other.config_);
    ctx_ = other.ctx_;
    other.ctx_ = nullptr;
    wire_alpn_protocols_ = std::move(other.wire_alpn_protocols_);
}

TlsContext& TlsContext::operator=(TlsContext&& other) noexcept {
    if (this != &other) {
        std::scoped_lock lock(mutex_, other.mutex_);
        if (ctx_) {
            SSL_CTX_free(ctx_);
        }
        config_ = std::move(other.config_);
        ctx_ = other.ctx_;
        other.ctx_ = nullptr;
        wire_alpn_protocols_ = std::move(other.wire_alpn_protocols_);
    }
    return *this;
}

SSL_CTX* TlsContext::CreateAndConfigureCtx(const TlsConfig& config,
                                          const std::vector<unsigned char>& wire_alpn,
                                          void* alpn_callback_arg,
                                          std::string* error_out) {
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, nullptr);

    SSL_CTX* new_ctx = SSL_CTX_new(TLS_server_method());
    if (!new_ctx) {
        std::string err = "Failed to create SSL_CTX: " + GetLastOpenSslError();
        if (error_out) *error_out = err;
        return nullptr;
    }

    // Protocol Version Constraints: Enforce TLS 1.2+ or TLS 1.3+
    if (config.min_version == "TLSv1.3") {
        if (SSL_CTX_set_min_proto_version(new_ctx, TLS1_3_VERSION) != 1) {
            std::string err = "Failed to set minimum TLS version to TLSv1.3: " + GetLastOpenSslError();
            if (error_out) *error_out = err;
            SSL_CTX_free(new_ctx);
            return nullptr;
        }
    } else {
        if (SSL_CTX_set_min_proto_version(new_ctx, TLS1_2_VERSION) != 1) {
            std::string err = "Failed to set minimum TLS version to TLSv1.2: " + GetLastOpenSslError();
            if (error_out) *error_out = err;
            SSL_CTX_free(new_ctx);
            return nullptr;
        }
    }

    // Disable insecure and legacy protocols explicitly
    uint64_t options = SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_TLSv1 | SSL_OP_NO_TLSv1_1;
    options |= SSL_OP_NO_COMPRESSION; // Mitigate CRIME attacks
    if (config.prefer_server_ciphers) {
        options |= SSL_OP_CIPHER_SERVER_PREFERENCE;
    }
    SSL_CTX_set_options(new_ctx, static_cast<unsigned long>(options));

    // Configure Cipher Suites
    if (!config.ciphers.empty()) {
        if (SSL_CTX_set_cipher_list(new_ctx, config.ciphers.c_str()) != 1) {
            std::string err = "Failed to set TLS cipher list '" + config.ciphers + "': " + GetLastOpenSslError();
            if (error_out) *error_out = err;
            SSL_CTX_free(new_ctx);
            return nullptr;
        }
    }

    if (!config.ciphersuites.empty()) {
        if (SSL_CTX_set_ciphersuites(new_ctx, config.ciphersuites.c_str()) != 1) {
            std::string err = "Failed to set TLSv1.3 ciphersuites '" + config.ciphersuites + "': " + GetLastOpenSslError();
            if (error_out) *error_out = err;
            SSL_CTX_free(new_ctx);
            return nullptr;
        }
    }

    // Configure ALPN negotiation
    if (!wire_alpn.empty()) {
        SSL_CTX_set_alpn_select_cb(new_ctx, AlpnSelectCallback, alpn_callback_arg);
    }

    // Load Certificate Chain and Private Key
    if (!config.cert_file.empty()) {
        if (SSL_CTX_use_certificate_chain_file(new_ctx, config.cert_file.c_str()) != 1) {
            std::string err = "Failed to load certificate file '" + config.cert_file + "': " + GetLastOpenSslError();
            if (error_out) *error_out = err;
            SSL_CTX_free(new_ctx);
            return nullptr;
        }
    }

    if (!config.key_file.empty()) {
        if (SSL_CTX_use_PrivateKey_file(new_ctx, config.key_file.c_str(), SSL_FILETYPE_PEM) != 1) {
            std::string err = "Failed to load private key file '" + config.key_file + "': " + GetLastOpenSslError();
            if (error_out) *error_out = err;
            SSL_CTX_free(new_ctx);
            return nullptr;
        }

        if (SSL_CTX_check_private_key(new_ctx) != 1) {
            std::string err = "Private key does not match public certificate: " + GetLastOpenSslError();
            if (error_out) *error_out = err;
            SSL_CTX_free(new_ctx);
            return nullptr;
        }
    }

    return new_ctx;
}

bool TlsContext::Initialize(std::string* error_out) {
    std::lock_guard lock(mutex_);
    SSL_CTX* new_ctx = CreateAndConfigureCtx(config_, wire_alpn_protocols_, this, error_out);
    if (!new_ctx) {
        return false;
    }
    if (ctx_) {
        SSL_CTX_free(ctx_);
    }
    ctx_ = new_ctx;
    return true;
}

bool TlsContext::Reload(const TlsConfig& new_config, std::string* error_out) {
    auto wire_alpn = EncodeWireAlpn(new_config.alpn_protocols);
    SSL_CTX* new_ctx = CreateAndConfigureCtx(new_config, wire_alpn, this, error_out);
    if (!new_ctx) {
        return false;
    }

    std::lock_guard lock(mutex_);
    SSL_CTX* old_ctx = ctx_;
    ctx_ = new_ctx;
    config_ = new_config;
    wire_alpn_protocols_ = std::move(wire_alpn);

    if (old_ctx) {
        SSL_CTX_free(old_ctx);
    }
    return true;
}

std::unique_ptr<TlsStream> TlsContext::CreateStream() const {
    std::lock_guard lock(mutex_);
    if (!ctx_) {
        return nullptr;
    }
    SSL* ssl = SSL_new(ctx_);
    if (!ssl) {
        return nullptr;
    }
    return std::make_unique<TlsStream>(ssl);
}

bool TlsContext::ReloadCertificates(const std::string& cert_file, const std::string& key_file, std::string* error_out) {
    TlsConfig cfg;
    {
        std::lock_guard lock(mutex_);
        cfg = config_;
    }
    cfg.cert_file = cert_file;
    cfg.key_file = key_file;
    return Reload(cfg, error_out);
}

} // namespace helios
