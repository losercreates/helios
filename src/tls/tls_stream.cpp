#include "tls_stream.hpp"
#include "tls_context.hpp"
#include <iostream>

namespace helios {

TlsStream::TlsStream(SSL* ssl)
    : ssl_(ssl) {
    if (ssl_) {
        rbio_ = BIO_new(BIO_s_mem());
        wbio_ = BIO_new(BIO_s_mem());
        BIO_set_mem_eof_return(rbio_, -1);
        BIO_set_mem_eof_return(wbio_, -1);
        // OpenSSL takes ownership of rbio_ and wbio_
        SSL_set_bio(ssl_, rbio_, wbio_);
        SSL_set_accept_state(ssl_);
    }
}

TlsStream::~TlsStream() {
    if (ssl_) {
        SSL_free(ssl_);
        ssl_ = nullptr;
        rbio_ = nullptr;
        wbio_ = nullptr;
    }
}

TlsStream::TlsStream(TlsStream&& other) noexcept
    : ssl_(other.ssl_),
      rbio_(other.rbio_),
      wbio_(other.wbio_),
      handshake_complete_(other.handshake_complete_) {
    other.ssl_ = nullptr;
    other.rbio_ = nullptr;
    other.wbio_ = nullptr;
    other.handshake_complete_ = false;
}

TlsStream& TlsStream::operator=(TlsStream&& other) noexcept {
    if (this != &other) {
        if (ssl_) {
            SSL_free(ssl_);
        }
        ssl_ = other.ssl_;
        rbio_ = other.rbio_;
        wbio_ = other.wbio_;
        handshake_complete_ = other.handshake_complete_;

        other.ssl_ = nullptr;
        other.rbio_ = nullptr;
        other.wbio_ = nullptr;
        other.handshake_complete_ = false;
    }
    return *this;
}

void TlsStream::DrainWbio(std::vector<uint8_t>& out_cipher) {
    if (!wbio_) return;
    char buf[4096];
    while (BIO_ctrl_pending(wbio_) > 0) {
        int n = BIO_read(wbio_, buf, sizeof(buf));
        if (n > 0) {
            out_cipher.insert(out_cipher.end(),
                              reinterpret_cast<const uint8_t*>(buf),
                              reinterpret_cast<const uint8_t*>(buf) + n);
        } else {
            break;
        }
    }
}

TlsHandshakeStatus TlsStream::Handshake(std::span<const uint8_t> in_cipher, std::vector<uint8_t>& out_cipher) {
    if (!ssl_) return TlsHandshakeStatus::Failed;
    if (handshake_complete_) return TlsHandshakeStatus::Complete;

    if (!in_cipher.empty()) {
        int written = BIO_write(rbio_, in_cipher.data(), static_cast<int>(in_cipher.size()));
        if (written <= 0) {
            return TlsHandshakeStatus::Failed;
        }
    }

    int ret = SSL_do_handshake(ssl_);
    DrainWbio(out_cipher);

    if (ret == 1) {
        handshake_complete_ = true;
        return TlsHandshakeStatus::Complete;
    }

    int err = SSL_get_error(ssl_, ret);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        return TlsHandshakeStatus::InProgress;
    }

    return TlsHandshakeStatus::Failed;
}

TlsDecryptResult TlsStream::Decrypt(std::span<const uint8_t> in_cipher, std::span<uint8_t> out_plain) {
    TlsDecryptResult result;
    if (!ssl_) {
        result.error = true;
        result.error_message = "SSL uninitialized";
        return result;
    }

    if (!in_cipher.empty()) {
        int written = BIO_write(rbio_, in_cipher.data(), static_cast<int>(in_cipher.size()));
        if (written <= 0) {
            result.error = true;
            result.error_message = "BIO_write failed to buffer ciphertext";
            return result;
        }
    }

    if (out_plain.empty()) {
        return result;
    }

    int n = SSL_read(ssl_, out_plain.data(), static_cast<int>(out_plain.size()));
    if (n > 0) {
        result.bytes_produced = static_cast<size_t>(n);
        return result;
    }

    int err = SSL_get_error(ssl_, n);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        result.want_read = true;
        return result;
    }

    if (err == SSL_ERROR_ZERO_RETURN) {
        result.peer_closed = true;
        return result;
    }

    result.error = true;
    result.error_message = TlsContext::GetLastOpenSslError();
    return result;
}

TlsDecryptResult TlsStream::Decrypt(std::span<const uint8_t> in_cipher, std::string& out_plain) {
    TlsDecryptResult result;
    if (!ssl_) {
        result.error = true;
        result.error_message = "SSL uninitialized";
        return result;
    }

    if (!in_cipher.empty()) {
        int written = BIO_write(rbio_, in_cipher.data(), static_cast<int>(in_cipher.size()));
        if (written <= 0) {
            result.error = true;
            result.error_message = "BIO_write failed to buffer ciphertext";
            return result;
        }
    }

    char buf[4096];
    while (true) {
        int n = SSL_read(ssl_, buf, sizeof(buf));
        if (n > 0) {
            out_plain.append(buf, static_cast<size_t>(n));
            result.bytes_produced += static_cast<size_t>(n);
        } else {
            int err = SSL_get_error(ssl_, n);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                result.want_read = true;
                break;
            }
            if (err == SSL_ERROR_ZERO_RETURN) {
                result.peer_closed = true;
                break;
            }
            result.error = true;
            result.error_message = TlsContext::GetLastOpenSslError();
            break;
        }
    }
    return result;
}

TlsEncryptResult TlsStream::Encrypt(std::span<const uint8_t> in_plain, std::vector<uint8_t>& out_cipher) {
    TlsEncryptResult result;
    if (!ssl_) {
        result.error = true;
        result.error_message = "SSL uninitialized";
        return result;
    }

    if (!in_plain.empty()) {
        int n = SSL_write(ssl_, in_plain.data(), static_cast<int>(in_plain.size()));
        if (n <= 0) {
            int err = SSL_get_error(ssl_, n);
            result.error = true;
            result.error_message = "SSL_write failed (err=" + std::to_string(err) + "): " + TlsContext::GetLastOpenSslError();
            return result;
        }
    }

    size_t before = out_cipher.size();
    DrainWbio(out_cipher);
    result.ciphertext_bytes = out_cipher.size() - before;
    return result;
}

void TlsStream::DrainPendingCiphertext(std::vector<uint8_t>& out_cipher) {
    DrainWbio(out_cipher);
}

void TlsStream::Shutdown(std::vector<uint8_t>& out_cipher) {
    if (ssl_) {
        SSL_shutdown(ssl_);
        DrainWbio(out_cipher);
    }
}

std::string TlsStream::GetAlpnSelected() const {
    if (!ssl_) return "";
    const unsigned char* alpn_data = nullptr;
    unsigned int alpn_len = 0;
    SSL_get0_alpn_selected(ssl_, &alpn_data, &alpn_len);
    if (alpn_data && alpn_len > 0) {
        return std::string(reinterpret_cast<const char*>(alpn_data), alpn_len);
    }
    return "";
}

std::string TlsStream::GetCipherName() const {
    if (!ssl_) return "";
    const char* c = SSL_get_cipher_name(ssl_);
    return c ? std::string(c) : "";
}

std::string TlsStream::GetTlsVersion() const {
    if (!ssl_) return "";
    const char* v = SSL_get_version(ssl_);
    return v ? std::string(v) : "";
}

std::string TlsStream::GetSniServerName() const {
    if (!ssl_) return "";
    const char* sni = SSL_get_servername(ssl_, TLSEXT_NAMETYPE_host_name);
    return sni ? std::string(sni) : "";
}

} // namespace helios
