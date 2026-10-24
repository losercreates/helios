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

TlsContext::TlsContext(TlsConfig config)
    : config_(std::move(config)) {
    // Encode ALPN protocols in wire format (length-prefixed strings)
    wire_alpn_protocols_.clear();
    for (const auto& proto : config_.alpn_protocols) {
        if (!proto.empty() && proto.size() <= 255) {
            wire_alpn_protocols_.push_back(static_cast<unsigned char>(proto.size()));
            wire_alpn_protocols_.insert(wire_alpn_protocols_.end(), proto.begin(), proto.end());
        }
    }
}

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

bool TlsContext::Initialize(std::string* error_out) {
    std::lock_guard lock(mutex_);

    if (ctx_) {
        SSL_CTX_free(ctx_);
        ctx_ = nullptr;
    }

    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, nullptr);

    ctx_ = SSL_CTX_new(TLS_server_method());
    if (!ctx_) {
        std::string err = "Failed to create SSL_CTX: " + GetLastOpenSslError();
        if (error_out) *error_out = err;
        return false;
    }

    // Protocol Version Constraints: Enforce TLS 1.2+ or TLS 1.3+
    if (config_.min_version == "TLSv1.3") {
        if (SSL_CTX_set_min_proto_version(ctx_, TLS1_3_VERSION) != 1) {
            std::string err = "Failed to set minimum TLS version to TLSv1.3: " + GetLastOpenSslError();
            if (error_out) *error_out = err;
            SSL_CTX_free(ctx_);
            ctx_ = nullptr;
            return false;
        }
    } else {
        if (SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION) != 1) {
            std::string err = "Failed to set minimum TLS version to TLSv1.2: " + GetLastOpenSslError();
            if (error_out) *error_out = err;
            SSL_CTX_free(ctx_);
            ctx_ = nullptr;
            return false;
        }
    }

    // Disable insecure and legacy protocols explicitly
    uint64_t options = SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_TLSv1 | SSL_OP_NO_TLSv1_1;
    options |= SSL_OP_NO_COMPRESSION; // Mitigate CRIME attacks
    if (config_.prefer_server_ciphers) {
        options |= SSL_OP_CIPHER_SERVER_PREFERENCE;
    }
    SSL_CTX_set_options(ctx_, static_cast<unsigned long>(options));

    // Configure Cipher Suites
    if (!config_.ciphers.empty()) {
        if (SSL_CTX_set_cipher_list(ctx_, config_.ciphers.c_str()) != 1) {
            std::string err = "Failed to set TLS cipher list '" + config_.ciphers + "': " + GetLastOpenSslError();
            if (error_out) *error_out = err;
            SSL_CTX_free(ctx_);
            ctx_ = nullptr;
            return false;
        }
    }

    if (!config_.ciphersuites.empty()) {
        if (SSL_CTX_set_ciphersuites(ctx_, config_.ciphersuites.c_str()) != 1) {
            std::string err = "Failed to set TLSv1.3 ciphersuites '" + config_.ciphersuites + "': " + GetLastOpenSslError();
            if (error_out) *error_out = err;
            SSL_CTX_free(ctx_);
            ctx_ = nullptr;
            return false;
        }
    }

    // Configure ALPN negotiation
    if (!wire_alpn_protocols_.empty()) {
        SSL_CTX_set_alpn_select_cb(ctx_, AlpnSelectCallback, this);
    }

    // Load Certificate Chain and Private Key
    if (!config_.cert_file.empty()) {
        if (SSL_CTX_use_certificate_chain_file(ctx_, config_.cert_file.c_str()) != 1) {
            std::string err = "Failed to load certificate file '" + config_.cert_file + "': " + GetLastOpenSslError();
            if (error_out) *error_out = err;
            SSL_CTX_free(ctx_);
            ctx_ = nullptr;
            return false;
        }
    }

    if (!config_.key_file.empty()) {
        if (SSL_CTX_use_PrivateKey_file(ctx_, config_.key_file.c_str(), SSL_FILETYPE_PEM) != 1) {
            std::string err = "Failed to load private key file '" + config_.key_file + "': " + GetLastOpenSslError();
            if (error_out) *error_out = err;
            SSL_CTX_free(ctx_);
            ctx_ = nullptr;
            return false;
        }

        if (SSL_CTX_check_private_key(ctx_) != 1) {
            std::string err = "Private key does not match public certificate: " + GetLastOpenSslError();
            if (error_out) *error_out = err;
            SSL_CTX_free(ctx_);
            ctx_ = nullptr;
            return false;
        }
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
    std::lock_guard lock(mutex_);
    if (!ctx_) {
        if (error_out) *error_out = "TlsContext is uninitialized";
        return false;
    }

    if (SSL_CTX_use_certificate_chain_file(ctx_, cert_file.c_str()) != 1) {
        std::string err = "Failed to reload certificate file '" + cert_file + "': " + GetLastOpenSslError();
        if (error_out) *error_out = err;
        return false;
    }

    if (SSL_CTX_use_PrivateKey_file(ctx_, key_file.c_str(), SSL_FILETYPE_PEM) != 1) {
        std::string err = "Failed to reload private key file '" + key_file + "': " + GetLastOpenSslError();
        if (error_out) *error_out = err;
        return false;
    }

    if (SSL_CTX_check_private_key(ctx_) != 1) {
        std::string err = "Reloaded private key does not match certificate: " + GetLastOpenSslError();
        if (error_out) *error_out = err;
        return false;
    }

    config_.cert_file = cert_file;
    config_.key_file = key_file;
    return true;
}

} // namespace helios
