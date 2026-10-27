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
