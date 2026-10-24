#pragma once

#include <span>
#include <vector>
#include <string>
#include <cstdint>
#include <cstddef>
#include <openssl/ssl.h>
#include <openssl/bio.h>

namespace helios {

enum class TlsHandshakeStatus {
    InProgress,
    Complete,
    Failed
};

struct TlsDecryptResult {
    size_t bytes_produced{0};
    bool want_read{false};
    bool peer_closed{false};
    bool error{false};
    std::string error_message;
};

struct TlsEncryptResult {
    size_t ciphertext_bytes{0};
    bool error{false};
    std::string error_message;
};

// Represents an active in-memory non-blocking TLS stream session over OpenSSL memory BIOs
class TlsStream {
public:
    explicit TlsStream(SSL* ssl);
    ~TlsStream();

    // Non-copyable
    TlsStream(const TlsStream&) = delete;
    TlsStream& operator=(const TlsStream&) = delete;

    // Movable
    TlsStream(TlsStream&& other) noexcept;
    TlsStream& operator=(TlsStream&& other) noexcept;

    // Advance TLS handshake by feeding input ciphertext and draining outgoing handshake records
    TlsHandshakeStatus Handshake(std::span<const uint8_t> in_cipher, std::vector<uint8_t>& out_cipher);

    // Decrypt application data from incoming network ciphertext
    TlsDecryptResult Decrypt(std::span<const uint8_t> in_cipher, std::span<uint8_t> out_plain);
    TlsDecryptResult Decrypt(std::span<const uint8_t> in_cipher, std::string& out_plain);

    // Encrypt outgoing application plaintext into outgoing network ciphertext
    TlsEncryptResult Encrypt(std::span<const uint8_t> in_plain, std::vector<uint8_t>& out_cipher);

    // Drain any remaining encrypted bytes from output BIO
    void DrainPendingCiphertext(std::vector<uint8_t>& out_cipher);

    // Gracefully send TLS close_notify shutdown alert
    void Shutdown(std::vector<uint8_t>& out_cipher);

    [[nodiscard]] bool IsHandshakeComplete() const noexcept { return handshake_complete_; }
    [[nodiscard]] std::string GetAlpnSelected() const;
    [[nodiscard]] std::string GetCipherName() const;
    [[nodiscard]] std::string GetTlsVersion() const;
    [[nodiscard]] std::string GetSniServerName() const;
    [[nodiscard]] SSL* GetNativeHandle() const noexcept { return ssl_; }

private:
    void DrainWbio(std::vector<uint8_t>& out_cipher);

    SSL* ssl_{nullptr};
    BIO* rbio_{nullptr};
    BIO* wbio_{nullptr};
    bool handshake_complete_{false};
};

} // namespace helios
