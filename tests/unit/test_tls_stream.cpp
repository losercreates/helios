#include <gtest/gtest.h>
#include "tls/tls_context.hpp"
#include "tls/tls_stream.hpp"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <vector>
#include <string>
#include <span>

namespace helios {
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
        if (access(path.c_str(), R_OK) == 0) return path;
    }
    return "tests/test_certs/" + filename;
}

const std::string kCertPath = FindCertFile("server.crt");
const std::string kKeyPath = FindCertFile("server.key");

class TlsStreamTest : public ::testing::Test {
protected:
    void SetUp() override {
        TlsConfig config;
        config.enabled = true;
        config.cert_file = kCertPath;
        config.key_file = kKeyPath;
        config.min_version = "TLSv1.2";
        config.alpn_protocols = {"http/1.1"};

        server_context_ = std::make_unique<TlsContext>(config);
        ASSERT_TRUE(server_context_->Initialize());

        client_ctx_ = SSL_CTX_new(TLS_client_method());
        ASSERT_NE(client_ctx_, nullptr);
        SSL_CTX_set_min_proto_version(client_ctx_, TLS1_2_VERSION);

        // Configure client ALPN to advertise http/1.1
        unsigned char alpn[] = { 8, 'h', 't', 't', 'p', '/', '1', '.', '1' };
        SSL_CTX_set_alpn_protos(client_ctx_, alpn, sizeof(alpn));

        // Disable certificate verification for unit tests against self-signed cert
        SSL_CTX_set_verify(client_ctx_, SSL_VERIFY_NONE, nullptr);
    }

    void TearDown() override {
        if (client_ctx_) {
            SSL_CTX_free(client_ctx_);
            client_ctx_ = nullptr;
        }
    }

    std::vector<uint8_t> DrainBio(BIO* bio) {
        std::vector<uint8_t> data;
        char buf[4096];
        while (BIO_ctrl_pending(bio) > 0) {
            int n = BIO_read(bio, buf, sizeof(buf));
            if (n > 0) {
                data.insert(data.end(), reinterpret_cast<const uint8_t*>(buf), reinterpret_cast<const uint8_t*>(buf) + n);
            } else {
                break;
            }
        }
        return data;
    }

    std::unique_ptr<TlsContext> server_context_;
    SSL_CTX* client_ctx_{nullptr};
};

TEST_F(TlsStreamTest, CompleteInMemoryHandshakeAndDataTransfer) {
    auto server_stream = server_context_->CreateStream();
    ASSERT_NE(server_stream, nullptr);

    SSL* client_ssl = SSL_new(client_ctx_);
    ASSERT_NE(client_ssl, nullptr);

    BIO* c_rbio = BIO_new(BIO_s_mem());
    BIO* c_wbio = BIO_new(BIO_s_mem());
    BIO_set_mem_eof_return(c_rbio, -1);
    BIO_set_mem_eof_return(c_wbio, -1);
    SSL_set_bio(client_ssl, c_rbio, c_wbio);
    SSL_set_connect_state(client_ssl);

    // --- 1. Handshake Loop ---
    int iterations = 0;
    while ((!server_stream->IsHandshakeComplete() || !SSL_is_init_finished(client_ssl)) && iterations++ < 20) {
        // Step client handshake
        if (!SSL_is_init_finished(client_ssl)) {
            int ret = SSL_do_handshake(client_ssl);
            (void)ret;
        }
        std::vector<uint8_t> client_to_server = DrainBio(c_wbio);

        // Step server handshake
        std::vector<uint8_t> server_to_client;
        if (!client_to_server.empty() || !server_stream->IsHandshakeComplete()) {
            server_stream->Handshake(client_to_server, server_to_client);
        }

        // Pump server ciphertext to client
        if (!server_to_client.empty()) {
            BIO_write(c_rbio, server_to_client.data(), static_cast<int>(server_to_client.size()));
        }
    }

    EXPECT_TRUE(server_stream->IsHandshakeComplete());
    EXPECT_TRUE(SSL_is_init_finished(client_ssl));
    EXPECT_EQ(server_stream->GetAlpnSelected(), "http/1.1");
    EXPECT_FALSE(server_stream->GetCipherName().empty());
    EXPECT_FALSE(server_stream->GetTlsVersion().empty());

    // --- 2. Client -> Server Application Data Decryption ---
    std::string request = "GET /index.html HTTP/1.1\r\nHost: localhost\r\n\r\n";
    int write_res = SSL_write(client_ssl, request.data(), static_cast<int>(request.size()));
    ASSERT_EQ(write_res, static_cast<int>(request.size()));

    std::vector<uint8_t> client_cipher = DrainBio(c_wbio);
    ASSERT_FALSE(client_cipher.empty());

    std::vector<uint8_t> decrypted_buf(4096);
    auto decrypt_res = server_stream->Decrypt(client_cipher, decrypted_buf);
    EXPECT_FALSE(decrypt_res.error);
    EXPECT_EQ(decrypt_res.bytes_produced, request.size());
    std::string decrypted_str(reinterpret_cast<const char*>(decrypted_buf.data()), decrypt_res.bytes_produced);
    EXPECT_EQ(decrypted_str, request);

    // --- 3. Server -> Client Application Data Encryption ---
    std::string response = "HTTP/1.1 200 OK\r\nContent-Length: 13\r\n\r\nHello, World!";
    std::vector<uint8_t> server_cipher;
    std::span<const uint8_t> plain_span(reinterpret_cast<const uint8_t*>(response.data()), response.size());
    auto encrypt_res = server_stream->Encrypt(plain_span, server_cipher);
    EXPECT_FALSE(encrypt_res.error);
    EXPECT_GT(server_cipher.size(), 0u);

    // Pump to client and read
    BIO_write(c_rbio, server_cipher.data(), static_cast<int>(server_cipher.size()));
    char client_plain[256];
    int client_read = SSL_read(client_ssl, client_plain, sizeof(client_plain) - 1);
    ASSERT_EQ(client_read, static_cast<int>(response.size()));
    client_plain[client_read] = '\0';
    EXPECT_STREQ(client_plain, response.c_str());

    // --- 4. Orderly Shutdown ---
    std::vector<uint8_t> server_shutdown;
    server_stream->Shutdown(server_shutdown);
    if (!server_shutdown.empty()) {
        BIO_write(c_rbio, server_shutdown.data(), static_cast<int>(server_shutdown.size()));
    }
    int shutdown_res = SSL_read(client_ssl, client_plain, sizeof(client_plain));
    if (shutdown_res <= 0) {
        int err = SSL_get_error(client_ssl, shutdown_res);
        EXPECT_EQ(err, SSL_ERROR_ZERO_RETURN); // TLS close_notify received cleanly
    }

    SSL_free(client_ssl);
}

TEST_F(TlsStreamTest, HandshakeRejectsInvalidCiphertextGracefully) {
    auto server_stream = server_context_->CreateStream();
    ASSERT_NE(server_stream, nullptr);

    // Send garbage non-TLS bytes to simulate corrupt/malicious client
    std::vector<uint8_t> garbage = {0xFF, 0x00, 0xDE, 0xAD, 0xBE, 0xEF, 0x12, 0x34};
    std::vector<uint8_t> server_out;
    auto status = server_stream->Handshake(garbage, server_out);
    EXPECT_EQ(status, TlsHandshakeStatus::Failed);
    EXPECT_FALSE(server_stream->IsHandshakeComplete());
}

} // namespace
} // namespace helios
