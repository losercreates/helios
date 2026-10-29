#include <gtest/gtest.h>
#include "http/http_rewriter.hpp"

using namespace helios;

TEST(HttpRewriterTest, InjectsStandardHeaders) {
    std::string req = 
        "GET /index.html HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "User-Agent: curl/8.0\r\n"
        "\r\n";

    HeaderInjectionOptions opts;
    opts.client_ip = "203.0.113.195";
    opts.client_proto = "http";
    opts.host = "example.com";
    opts.request_id = "test-req-id-12345";

    std::string rewritten = HttpRewriter::RewriteRequest(req, opts);

    EXPECT_NE(rewritten.find("X-Forwarded-For: 203.0.113.195\r\n"), std::string::npos);
    EXPECT_NE(rewritten.find("X-Forwarded-Proto: http\r\n"), std::string::npos);
    EXPECT_NE(rewritten.find("X-Forwarded-Host: example.com\r\n"), std::string::npos);
    EXPECT_NE(rewritten.find("X-Request-ID: test-req-id-12345\r\n"), std::string::npos);
    EXPECT_NE(rewritten.find("User-Agent: curl/8.0\r\n"), std::string::npos);
}

TEST(HttpRewriterTest, AppendsToExistingForwardedFor) {
    std::string req = 
        "POST /api/v1/data HTTP/1.1\r\n"
        "Host: api.example.com\r\n"
        "X-Forwarded-For: 10.0.0.1\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "hello";

    HeaderInjectionOptions opts;
    opts.client_ip = "192.168.1.50";
    opts.request_id = "req-999";

    std::string rewritten = HttpRewriter::RewriteRequest(req, opts);

    EXPECT_NE(rewritten.find("X-Forwarded-For: 10.0.0.1, 192.168.1.50\r\n"), std::string::npos);
    EXPECT_NE(rewritten.find("X-Request-ID: req-999\r\n"), std::string::npos);
    // Preserves body
    EXPECT_TRUE(rewritten.ends_with("hello"));
}

TEST(HttpRewriterTest, RewritesRequestPathWhenProvided) {
    std::string req = 
        "GET /api/v1/users?limit=10 HTTP/1.1\r\n"
        "Host: api.example.com\r\n"
        "\r\n";

    HeaderInjectionOptions opts;
    opts.client_ip = "127.0.0.1";
    opts.rewritten_path = "/users?limit=10";

    std::string rewritten = HttpRewriter::RewriteRequest(req, opts);

    EXPECT_TRUE(rewritten.starts_with("GET /users?limit=10 HTTP/1.1\r\n"));
    EXPECT_NE(rewritten.find("X-Forwarded-For: 127.0.0.1\r\n"), std::string::npos);
}

TEST(HttpRewriterTest, AutoGeneratesUniqueRequestIdIfEmpty) {
    std::string req = 
        "GET / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "\r\n";

    HeaderInjectionOptions opts;
    opts.client_ip = "127.0.0.1";
    opts.request_id = ""; // Empty -> should auto-generate

    std::string rewritten1 = HttpRewriter::RewriteRequest(req, opts);
    std::string rewritten2 = HttpRewriter::RewriteRequest(req, opts);

    auto pos1 = rewritten1.find("X-Request-ID: ");
    ASSERT_NE(pos1, std::string::npos);
    auto end1 = rewritten1.find("\r\n", pos1);
    std::string id1 = rewritten1.substr(pos1 + 14, end1 - pos1 - 14);

    auto pos2 = rewritten2.find("X-Request-ID: ");
    ASSERT_NE(pos2, std::string::npos);
    auto end2 = rewritten2.find("\r\n", pos2);
    std::string id2 = rewritten2.substr(pos2 + 14, end2 - pos2 - 14);

    EXPECT_FALSE(id1.empty());
    EXPECT_FALSE(id2.empty());
    EXPECT_NE(id1, id2);
}

TEST(HttpRewriterTest, PreservesWebSocketHandshakeHeaders) {
    std::string req =
        "GET /chat HTTP/1.1\r\n"
        "Host: ws.example.com\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Protocol: chat, superchat\r\n"
        "Sec-WebSocket-Extensions: permessage-deflate\r\n"
        "\r\n";

    HeaderInjectionOptions opts;
    opts.client_ip = "192.0.2.1";
    opts.client_proto = "https";
    opts.request_id = "ws-req-001";

    std::string rewritten = HttpRewriter::RewriteRequest(req, opts);

    EXPECT_NE(rewritten.find("Upgrade: websocket\r\n"), std::string::npos);
    EXPECT_NE(rewritten.find("Connection: Upgrade\r\n"), std::string::npos);
    EXPECT_NE(rewritten.find("Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"), std::string::npos);
    EXPECT_NE(rewritten.find("Sec-WebSocket-Version: 13\r\n"), std::string::npos);
    EXPECT_NE(rewritten.find("Sec-WebSocket-Protocol: chat, superchat\r\n"), std::string::npos);
    EXPECT_NE(rewritten.find("Sec-WebSocket-Extensions: permessage-deflate\r\n"), std::string::npos);
    EXPECT_NE(rewritten.find("X-Forwarded-For: 192.0.2.1\r\n"), std::string::npos);
    EXPECT_NE(rewritten.find("X-Forwarded-Proto: https\r\n"), std::string::npos);
    EXPECT_NE(rewritten.find("X-Request-ID: ws-req-001\r\n"), std::string::npos);
}

TEST(HttpRewriterTest, RewriteResponse_InjectsSecurityHeaders) {
    std::string resp =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: 13\r\n"
        "\r\n"
        "Hello World\r\n";

    ResponseHeadersConfig cfg;
    cfg.enabled = true;
    cfg.set_server_header = true;
    cfg.server_value = "helios/2.0";
    cfg.hsts_enabled = true;
    cfg.hsts_value = "max-age=31536000; includeSubDomains";
    cfg.x_content_type_options = true;
    cfg.x_frame_options = "DENY";
    cfg.custom_headers = {{"X-Custom-Env", "Production"}, {"X-Rate-Limit", "100"}};

    std::string rewritten_tls = HttpRewriter::RewriteResponse(resp, cfg, true);
    EXPECT_NE(rewritten_tls.find("Server: helios/2.0\r\n"), std::string::npos);
    EXPECT_NE(rewritten_tls.find("Strict-Transport-Security: max-age=31536000; includeSubDomains\r\n"), std::string::npos);
    EXPECT_NE(rewritten_tls.find("X-Content-Type-Options: nosniff\r\n"), std::string::npos);
    EXPECT_NE(rewritten_tls.find("X-Frame-Options: DENY\r\n"), std::string::npos);
    EXPECT_NE(rewritten_tls.find("X-Custom-Env: Production\r\n"), std::string::npos);
    EXPECT_NE(rewritten_tls.find("X-Rate-Limit: 100\r\n"), std::string::npos);
    EXPECT_TRUE(rewritten_tls.ends_with("Hello World\r\n"));

    // When plaintext (non-TLS), HSTS should NOT be injected
    std::string rewritten_plain = HttpRewriter::RewriteResponse(resp, cfg, false);
    EXPECT_NE(rewritten_plain.find("Server: helios/2.0\r\n"), std::string::npos);
    EXPECT_EQ(rewritten_plain.find("Strict-Transport-Security"), std::string::npos);
    EXPECT_NE(rewritten_plain.find("X-Content-Type-Options: nosniff\r\n"), std::string::npos);
    EXPECT_NE(rewritten_plain.find("X-Frame-Options: DENY\r\n"), std::string::npos);
}

TEST(HttpRewriterTest, RewriteResponse_OverridesExistingHeaders) {
    std::string resp =
        "HTTP/1.1 200 OK\r\n"
        "Server: Apache/2.4\r\n"
        "X-Frame-Options: ALLOWALL\r\n"
        "Content-Length: 0\r\n"
        "\r\n";

    ResponseHeadersConfig cfg;
    cfg.enabled = true;
    cfg.set_server_header = true;
    cfg.server_value = "helios";
    cfg.x_frame_options = "SAMEORIGIN";

    std::string rewritten = HttpRewriter::RewriteResponse(resp, cfg, false);
    EXPECT_NE(rewritten.find("Server: helios\r\n"), std::string::npos);
    EXPECT_EQ(rewritten.find("Server: Apache/2.4"), std::string::npos);
    EXPECT_NE(rewritten.find("X-Frame-Options: SAMEORIGIN\r\n"), std::string::npos);
    EXPECT_EQ(rewritten.find("X-Frame-Options: ALLOWALL"), std::string::npos);
}

TEST(HttpRewriterTest, RewriteResponse_BypassesNonHttpOrDisabled) {
    std::string raw_binary = "\x01\x02\x03\x04\x05\x06\x07";
    ResponseHeadersConfig cfg;
    cfg.enabled = true;

    EXPECT_EQ(HttpRewriter::RewriteResponse(raw_binary, cfg, true), raw_binary);

    std::string http_resp = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
    cfg.enabled = false;
    EXPECT_EQ(HttpRewriter::RewriteResponse(http_resp, cfg, true), http_resp);
}
