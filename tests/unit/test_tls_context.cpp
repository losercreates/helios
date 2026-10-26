#include <gtest/gtest.h>
#include "tls/tls_context.hpp"
#include "tls/tls_stream.hpp"
#include <filesystem>

namespace helios {
namespace {

std::string FindCertFile(const std::string& filename) {
#ifdef HELIOS_TEST_CERTS_DIR
    std::string p = std::string(HELIOS_TEST_CERTS_DIR) + "/" + filename;
    if (std::filesystem::exists(p)) return p;
#endif
    std::vector<std::string> candidates = {
        "tests/test_certs/" + filename,
        "../tests/test_certs/" + filename,
        "../../tests/test_certs/" + filename
    };
    for (const auto& path : candidates) {
        if (std::filesystem::exists(path)) return path;
    }
    return "tests/test_certs/" + filename;
}

const std::string kCertPath = FindCertFile("server.crt");
const std::string kKeyPath = FindCertFile("server.key");

class TlsContextTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(std::filesystem::exists(kCertPath)) << "Test certificate missing: " << kCertPath;
        ASSERT_TRUE(std::filesystem::exists(kKeyPath)) << "Test key missing: " << kKeyPath;
    }
};

TEST_F(TlsContextTest, SuccessfulInitialization) {
    TlsConfig config;
    config.enabled = true;
    config.cert_file = kCertPath;
    config.key_file = kKeyPath;
    config.min_version = "TLSv1.2";

    TlsContext context(config);
    std::string err;
    EXPECT_TRUE(context.Initialize(&err)) << "Error: " << err;
    EXPECT_TRUE(context.IsInitialized());
    EXPECT_NE(context.GetNativeHandle(), nullptr);
}

TEST_F(TlsContextTest, MissingCertFileFailsGracefully) {
    TlsConfig config;
    config.enabled = true;
    config.cert_file = "non_existent_cert.pem";
    config.key_file = kKeyPath;

    TlsContext context(config);
    std::string err;
    EXPECT_FALSE(context.Initialize(&err));
    EXPECT_FALSE(context.IsInitialized());
    EXPECT_FALSE(err.empty());
}

TEST_F(TlsContextTest, MissingKeyFileFailsGracefully) {
    TlsConfig config;
    config.enabled = true;
    config.cert_file = kCertPath;
    config.key_file = "non_existent_key.pem";

    TlsContext context(config);
    std::string err;
    EXPECT_FALSE(context.Initialize(&err));
    EXPECT_FALSE(context.IsInitialized());
    EXPECT_FALSE(err.empty());
}

TEST_F(TlsContextTest, Tls13MinVersionAllowed) {
    TlsConfig config;
    config.enabled = true;
    config.cert_file = kCertPath;
    config.key_file = kKeyPath;
    config.min_version = "TLSv1.3";

    TlsContext context(config);
    std::string err;
    EXPECT_TRUE(context.Initialize(&err)) << "Error: " << err;
    EXPECT_TRUE(context.IsInitialized());
}

TEST_F(TlsContextTest, MoveSemanticsTransferContext) {
    TlsConfig config;
    config.enabled = true;
    config.cert_file = kCertPath;
    config.key_file = kKeyPath;

    TlsContext ctx1(config);
    EXPECT_TRUE(ctx1.Initialize());
    EXPECT_TRUE(ctx1.IsInitialized());

    TlsContext ctx2(std::move(ctx1));
    EXPECT_TRUE(ctx2.IsInitialized());
    EXPECT_FALSE(ctx1.IsInitialized());
}

TEST_F(TlsContextTest, ReloadCertificatesSuccess) {
    TlsConfig config;
    config.enabled = true;
    config.cert_file = kCertPath;
    config.key_file = kKeyPath;

    TlsContext context(config);
    EXPECT_TRUE(context.Initialize());

    std::string err;
    EXPECT_TRUE(context.ReloadCertificates(kCertPath, kKeyPath, &err)) << "Error: " << err;
    EXPECT_TRUE(context.IsInitialized());
}

TEST_F(TlsContextTest, ReloadFullConfigSuccess) {
    TlsConfig config;
    config.enabled = true;
    config.cert_file = kCertPath;
    config.key_file = kKeyPath;
    config.min_version = "TLSv1.2";

    TlsContext context(config);
    EXPECT_TRUE(context.Initialize());

    TlsConfig new_config = config;
    new_config.min_version = "TLSv1.3";
    std::string err;
    EXPECT_TRUE(context.Reload(new_config, &err)) << "Error: " << err;
    EXPECT_TRUE(context.IsInitialized());
    EXPECT_EQ(context.GetConfig().min_version, "TLSv1.3");
}

TEST_F(TlsContextTest, ReloadInvalidCertPreservesExistingContext) {
    TlsConfig config;
    config.enabled = true;
    config.cert_file = kCertPath;
    config.key_file = kKeyPath;

    TlsContext context(config);
    EXPECT_TRUE(context.Initialize());
    EXPECT_TRUE(context.IsInitialized());

    std::string err;
    EXPECT_FALSE(context.ReloadCertificates("/nonexistent/cert.crt", kKeyPath, &err));
    EXPECT_FALSE(err.empty());
    // Crucial: Context MUST remain valid and initialized after failed reload
    EXPECT_TRUE(context.IsInitialized());

    auto stream = context.CreateStream();
    EXPECT_NE(stream, nullptr);
}

TEST_F(TlsContextTest, CreateStreamProducesValidTlsStream) {
    TlsConfig config;
    config.enabled = true;
    config.cert_file = kCertPath;
    config.key_file = kKeyPath;

    TlsContext context(config);
    EXPECT_TRUE(context.Initialize());

    auto stream = context.CreateStream();
    ASSERT_NE(stream, nullptr);
    EXPECT_NE(stream->GetNativeHandle(), nullptr);
    EXPECT_FALSE(stream->IsHandshakeComplete());
}

} // namespace
} // namespace helios
