#include <gtest/gtest.h>
#include "http/http_parser.hpp"
#include <cstring>
#include <string>
#include <vector>

using namespace helios;

// ============================================================================
// 1. Standard Requests
// ============================================================================

TEST(HttpParserStandardRequests, SimpleGetRequest) {
    HttpParser parser;
    std::string raw =
        "GET /index.html HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "User-Agent: helios-test/1.0\r\n"
        "Accept: */*\r\n"
        "\r\n";

    ParseResult res = parser.Parse(raw);
    EXPECT_TRUE(res.IsComplete());
    EXPECT_EQ(res.status, ParseStatus::Complete);
    EXPECT_EQ(res.bytes_consumed, raw.size());

    const auto& req = parser.GetRequest();
    EXPECT_EQ(req.method, HttpMethod::GET);
    EXPECT_EQ(req.method_str, "GET");
    EXPECT_EQ(req.uri, "/index.html");
    EXPECT_EQ(req.path, "/index.html");
    EXPECT_TRUE(req.query.empty());
    EXPECT_EQ(req.http_major, 1);
    EXPECT_EQ(req.http_minor, 1);
    EXPECT_TRUE(req.keep_alive);

    // Case-insensitive header lookups
    EXPECT_EQ(req.GetHeader("Host"), "example.com");
    EXPECT_EQ(req.GetHeader("host"), "example.com");
    EXPECT_EQ(req.GetHeader("HOST"), "example.com");
    EXPECT_EQ(req.GetHeader("User-Agent"), "helios-test/1.0");
    EXPECT_EQ(req.GetHeader("accept"), "*/*");
    EXPECT_FALSE(req.GetHeader("Non-Existent").has_value());
    EXPECT_EQ(req.Headers().size(), 3u);
}

TEST(HttpParserStandardRequests, GetWithQueryParams) {
    HttpParser parser;
    std::string raw =
        "GET /api/v1/search?q=helios+proxy&limit=50&active=true HTTP/1.1\r\n"
        "Host: localhost:8080\r\n"
        "\r\n";

    ParseResult res = parser.Parse(raw);
    EXPECT_TRUE(res.IsComplete());

    const auto& req = parser.GetRequest();
    EXPECT_EQ(req.method, HttpMethod::GET);
    EXPECT_EQ(req.uri, "/api/v1/search?q=helios+proxy&limit=50&active=true");
    EXPECT_EQ(req.path, "/api/v1/search");
    EXPECT_EQ(req.query, "q=helios+proxy&limit=50&active=true");

    EXPECT_EQ(req.GetQueryParam("q"), "helios+proxy");
    EXPECT_EQ(req.GetQueryParam("limit"), "50");
    EXPECT_EQ(req.GetQueryParam("active"), "true");
    EXPECT_FALSE(req.GetQueryParam("missing").has_value());
}

TEST(HttpParserStandardRequests, PostWithContentLength) {
    HttpParser parser;
    std::string body = R"({"user":"helios","role":"admin"})";
    std::string raw =
        "POST /api/users HTTP/1.1\r\n"
        "Host: example.org\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "\r\n" + body;

    ParseResult res = parser.Parse(raw);
    EXPECT_TRUE(res.IsComplete());
    EXPECT_EQ(res.bytes_consumed, raw.size());

    const auto& req = parser.GetRequest();
    EXPECT_EQ(req.method, HttpMethod::POST);
    EXPECT_EQ(req.path, "/api/users");
    EXPECT_TRUE(req.has_content_length);
    EXPECT_EQ(req.content_length, body.size());
    EXPECT_FALSE(req.is_chunked);
    EXPECT_EQ(parser.GetBody(), body);
}

TEST(HttpParserStandardRequests, HeadRequestWithoutBody) {
    HttpParser parser;
    std::string raw =
        "HEAD /healthz HTTP/1.1\r\n"
        "Host: internal.service\r\n"
        "Connection: close\r\n"
        "\r\n";

    ParseResult res = parser.Parse(raw);
    EXPECT_TRUE(res.IsComplete());

    const auto& req = parser.GetRequest();
    EXPECT_EQ(req.method, HttpMethod::HEAD);
    EXPECT_EQ(req.path, "/healthz");
    EXPECT_FALSE(req.keep_alive);
    EXPECT_TRUE(parser.GetBody().empty());
}

TEST(HttpParserStandardRequests, DeleteRequest) {
    HttpParser parser;
    std::string raw =
        "DELETE /items/42 HTTP/1.1\r\n"
        "Host: example.org\r\n"
        "\r\n";

    ParseResult res = parser.Parse(raw);
    EXPECT_TRUE(res.IsComplete());

    const auto& req = parser.GetRequest();
    EXPECT_EQ(req.method, HttpMethod::DELETE_);
    EXPECT_EQ(req.path, "/items/42");
}

TEST(HttpParserStandardRequests, MultiLineHeadersObsFold) {
    HttpParser parser;
    std::string raw =
        "GET /obs HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Subject: Line1\r\n"
        " Line2\r\n"
        "\tLine3\r\n"
        "X-Other: normal\r\n"
        "\r\n";

    ParseResult res = parser.Parse(raw);
    EXPECT_TRUE(res.IsComplete());

    const auto& req = parser.GetRequest();
    auto subj = req.GetHeader("Subject");
    ASSERT_TRUE(subj.has_value());
    EXPECT_NE(subj->find("Line1"), std::string_view::npos);
    EXPECT_NE(subj->find("Line2"), std::string_view::npos);
    EXPECT_NE(subj->find("Line3"), std::string_view::npos);
    EXPECT_EQ(req.GetHeader("X-Other"), "normal");
}

TEST(HttpParserStandardRequests, BufferOverload) {
    HttpParser parser;
    std::string raw =
        "GET /buffer-test HTTP/1.1\r\n"
        "Host: test\r\n"
        "\r\n";

    std::vector<uint8_t> mem(1024, 0);
    std::memcpy(mem.data(), raw.data(), raw.size());

    Buffer buf;
    buf.data = mem.data();
    buf.capacity = mem.size();
    buf.read_offset = 0;
    buf.write_offset = raw.size();

    ParseResult res = parser.Parse(buf);
    EXPECT_TRUE(res.IsComplete());
    EXPECT_EQ(buf.read_offset, raw.size());
    EXPECT_EQ(parser.GetRequest().path, "/buffer-test");
}

// ============================================================================
// 2. Strict RFC 9112 Request Smuggling Rejection
// ============================================================================

TEST(HttpParserRequestSmugglingRejection, ConflictingContentLengthAndTransferEncoding_CL_First) {
    HttpParser parser;
    std::string raw =
        "POST / HTTP/1.1\r\n"
        "Host: victim.com\r\n"
        "Content-Length: 10\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "0\r\n\r\n";

    ParseResult res = parser.Parse(raw);
    EXPECT_TRUE(res.IsError());
    EXPECT_EQ(res.status, ParseStatus::ErrorBadRequest);
    EXPECT_EQ(res.error_code, 400);
}

TEST(HttpParserRequestSmugglingRejection, ConflictingTransferEncodingAndContentLength_TE_First) {
    HttpParser parser;
    std::string raw =
        "POST / HTTP/1.1\r\n"
        "Host: victim.com\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Content-Length: 10\r\n"
        "\r\n"
        "0\r\n\r\n";

    ParseResult res = parser.Parse(raw);
    EXPECT_TRUE(res.IsError());
    EXPECT_EQ(res.status, ParseStatus::ErrorBadRequest);
    EXPECT_EQ(res.error_code, 400);
}

TEST(HttpParserRequestSmugglingRejection, DuplicateConflictingContentLengthHeaders) {
    HttpParser parser;
    std::string raw =
        "POST / HTTP/1.1\r\n"
        "Host: victim.com\r\n"
        "Content-Length: 5\r\n"
        "Content-Length: 10\r\n"
        "\r\n"
        "12345";

    ParseResult res = parser.Parse(raw);
    EXPECT_TRUE(res.IsError());
    EXPECT_EQ(res.status, ParseStatus::ErrorBadRequest);
    EXPECT_EQ(res.error_code, 400);
}

TEST(HttpParserRequestSmugglingRejection, SingleContentLengthConflictingCommaSeparated) {
    HttpParser parser;
    std::string raw =
        "POST / HTTP/1.1\r\n"
        "Host: victim.com\r\n"
        "Content-Length: 5, 10\r\n"
        "\r\n"
        "12345";

    ParseResult res = parser.Parse(raw);
    EXPECT_TRUE(res.IsError());
    EXPECT_EQ(res.status, ParseStatus::ErrorBadRequest);
    EXPECT_EQ(res.error_code, 400);
}

TEST(HttpParserRequestSmugglingRejection, DuplicateIdenticalContentLengthAllowed) {
    HttpParser parser;
    std::string raw =
        "POST / HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Content-Length: 5, 5\r\n"
        "\r\n"
        "hello";

    ParseResult res = parser.Parse(raw);
    EXPECT_TRUE(res.IsComplete());
    EXPECT_EQ(parser.GetRequest().content_length, 5u);
    EXPECT_EQ(parser.GetBody(), "hello");
}

TEST(HttpParserRequestSmugglingRejection, InvalidNegativeContentLength) {
    HttpParser parser;
    std::string raw =
        "POST / HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Content-Length: -5\r\n"
        "\r\n";

    ParseResult res = parser.Parse(raw);
    EXPECT_TRUE(res.IsError());
    EXPECT_EQ(res.status, ParseStatus::ErrorBadRequest);
}

TEST(HttpParserRequestSmugglingRejection, InvalidPlusPrefixedContentLength) {
    HttpParser parser;
    std::string raw =
        "POST / HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Content-Length: +5\r\n"
        "\r\n";

    ParseResult res = parser.Parse(raw);
    EXPECT_TRUE(res.IsError());
    EXPECT_EQ(res.status, ParseStatus::ErrorBadRequest);
}

TEST(HttpParserRequestSmugglingRejection, InvalidAlphanumericContentLength) {
    HttpParser parser;
    std::string raw =
        "POST / HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Content-Length: 5abc\r\n"
        "\r\n";

    ParseResult res = parser.Parse(raw);
    EXPECT_TRUE(res.IsError());
    EXPECT_EQ(res.status, ParseStatus::ErrorBadRequest);
}

TEST(HttpParserRequestSmugglingRejection, WhitespacePrecedingHeaderColon_Space) {
    // RFC 9112 §5.1: Space preceding colon MUST be rejected with 400
    HttpParser parser;
    std::string raw =
        "GET / HTTP/1.1\r\n"
        "Host : example.com\r\n"
        "\r\n";

    ParseResult res = parser.Parse(raw);
    EXPECT_TRUE(res.IsError());
    EXPECT_EQ(res.status, ParseStatus::ErrorBadRequest);
    EXPECT_EQ(res.error_code, 400);
}

TEST(HttpParserRequestSmugglingRejection, WhitespacePrecedingHeaderColon_Tab) {
    // RFC 9112 §5.1: Tab preceding colon MUST be rejected with 400
    HttpParser parser;
    std::string raw =
        "GET / HTTP/1.1\r\n"
        "Host\t: example.com\r\n"
        "\r\n";

    ParseResult res = parser.Parse(raw);
    EXPECT_TRUE(res.IsError());
    EXPECT_EQ(res.status, ParseStatus::ErrorBadRequest);
    EXPECT_EQ(res.error_code, 400);
}

TEST(HttpParserRequestSmugglingRejection, WhitespacePrecedingHeaderColon_TransferEncoding) {
    HttpParser parser;
    std::string raw =
        "POST / HTTP/1.1\r\n"
        "Transfer-Encoding : chunked\r\n"
        "\r\n"
        "0\r\n\r\n";

    ParseResult res = parser.Parse(raw);
    EXPECT_TRUE(res.IsError());
    EXPECT_EQ(res.status, ParseStatus::ErrorBadRequest);
}

TEST(HttpParserRequestSmugglingRejection, WhitespacePrecedingHeaderColon_ContentLength) {
    HttpParser parser;
    std::string raw =
        "POST / HTTP/1.1\r\n"
        "Content-Length : 10\r\n"
        "\r\n";

    ParseResult res = parser.Parse(raw);
    EXPECT_TRUE(res.IsError());
    EXPECT_EQ(res.status, ParseStatus::ErrorBadRequest);
}

TEST(HttpParserRequestSmugglingRejection, ObsFoldWithoutPrecedingHeader) {
    HttpParser parser;
    std::string raw =
        "GET / HTTP/1.1\r\n"
        "  Host: example.com\r\n"
        "\r\n";

    ParseResult res = parser.Parse(raw);
    EXPECT_TRUE(res.IsError());
    EXPECT_EQ(res.status, ParseStatus::ErrorBadRequest);
}

TEST(HttpParserRequestSmugglingRejection, NullByteInHeader) {
    HttpParser parser;
    const char raw_data[] = "GET / HTTP/1.1\r\nHost: exam\0ple.com\r\n\r\n";
    std::string_view raw(raw_data, sizeof(raw_data) - 1);
    ParseResult res = parser.Parse(raw);
    EXPECT_TRUE(res.IsError());
    EXPECT_EQ(res.status, ParseStatus::ErrorBadRequest);
}

TEST(HttpParserRequestSmugglingRejection, ForbiddenTransferEncodingIdentity) {
    HttpParser parser;
    std::string raw =
        "POST / HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Transfer-Encoding: identity\r\n"
        "\r\n";

    ParseResult res = parser.Parse(raw);
    EXPECT_TRUE(res.IsError());
    EXPECT_EQ(res.status, ParseStatus::ErrorBadRequest);
}

TEST(HttpParserRequestSmugglingRejection, TransferEncodingNotEndingWithChunked) {
    HttpParser parser;
    std::string raw =
        "POST / HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Transfer-Encoding: chunked, identity\r\n"
        "\r\n";

    ParseResult res = parser.Parse(raw);
    EXPECT_TRUE(res.IsError());
    EXPECT_EQ(res.status, ParseStatus::ErrorBadRequest);
}

TEST(HttpParserRequestSmugglingRejection, MaxHeaderCountExceeded) {
    HttpParser parser;
    std::string raw = "GET / HTTP/1.1\r\nHost: example.com\r\n";
    for (int i = 0; i < 70; ++i) {
        raw += "X-Custom-" + std::to_string(i) + ": val\r\n";
    }
    raw += "\r\n";

    ParseResult res = parser.Parse(raw);
    EXPECT_TRUE(res.IsError());
    EXPECT_EQ(res.status, ParseStatus::ErrorHeaderTooLarge);
    EXPECT_EQ(res.error_code, 431);
}

// ============================================================================
// 3. Streaming Chunked Transfer-Encoding
// ============================================================================

TEST(HttpParserChunkedTransferEncoding, ValidSingleChunkFraming) {
    HttpParser parser;
    std::string raw =
        "POST /upload HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5\r\n"
        "hello\r\n"
        "0\r\n"
        "\r\n";

    ParseResult res = parser.Parse(raw);
    EXPECT_TRUE(res.IsComplete());
    EXPECT_EQ(res.bytes_consumed, raw.size());
    EXPECT_TRUE(parser.GetRequest().is_chunked);
    EXPECT_EQ(parser.GetDecodedBody(), "hello");
}

TEST(HttpParserChunkedTransferEncoding, MultiChunkStreaming) {
    HttpParser parser;
    std::string raw =
        "POST /wiki HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "4\r\n"
        "Wiki\r\n"
        "5\r\n"
        "pedia\r\n"
        "f\r\n"
        " in \r\n\r\nchunks.\r\n"
        "0\r\n"
        "\r\n";

    ParseResult res = parser.Parse(raw);
    EXPECT_TRUE(res.IsComplete());
    EXPECT_EQ(parser.GetDecodedBody(), "Wikipedia in \r\n\r\nchunks.");
}

TEST(HttpParserChunkedTransferEncoding, ChunkExtensionsAndTrailers) {
    HttpParser parser;
    std::string raw =
        "POST /ext HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "4;ext_key=ext_val\r\n"
        "test\r\n"
        "0;final_ext=1\r\n"
        "Trailer-Header: checksum_xyz\r\n"
        "\r\n";

    ParseResult res = parser.Parse(raw);
    EXPECT_TRUE(res.IsComplete());
    EXPECT_EQ(parser.GetDecodedBody(), "test");
}

TEST(HttpParserChunkedTransferEncoding, MalformedChunkSizeNonHex) {
    HttpParser parser;
    std::string raw =
        "POST /upload HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "z\r\n"
        "hello\r\n"
        "0\r\n\r\n";

    ParseResult res = parser.Parse(raw);
    EXPECT_TRUE(res.IsError());
    EXPECT_EQ(res.status, ParseStatus::ErrorBadRequest);
}

TEST(HttpParserChunkedTransferEncoding, MalformedMissingCrlfAfterData) {
    HttpParser parser;
    std::string raw =
        "POST /upload HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "4\r\n"
        "testXX";

    ParseResult res = parser.Parse(raw);
    EXPECT_TRUE(res.IsError());
    EXPECT_EQ(res.status, ParseStatus::ErrorBadRequest);
}

TEST(HttpParserChunkedTransferEncoding, StreamingBodyCallback) {
    HttpParser parser;
    std::vector<std::string> chunks_received;
    parser.SetBodyCallback([&](std::string_view chunk) {
        chunks_received.emplace_back(chunk);
    });

    std::string raw =
        "POST /stream HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "3\r\nfoo\r\n"
        "3\r\nbar\r\n"
        "3\r\nbaz\r\n"
        "0\r\n\r\n";

    ParseResult res = parser.Parse(raw);
    EXPECT_TRUE(res.IsComplete());
    ASSERT_EQ(chunks_received.size(), 3u);
    EXPECT_EQ(chunks_received[0], "foo");
    EXPECT_EQ(chunks_received[1], "bar");
    EXPECT_EQ(chunks_received[2], "baz");
    EXPECT_EQ(parser.GetDecodedBody(), "foobarbaz");
}

// ============================================================================
// 4. Partial Stream Reassembly
// ============================================================================

TEST(HttpParserPartialStreamReassembly, SingleByteFragmentedGet) {
    HttpParser parser;
    std::string raw =
        "GET /fragmented-index.html HTTP/1.1\r\n"
        "Host: sub.domain.org\r\n"
        "X-Custom-Header: value_12345\r\n"
        "\r\n";

    for (size_t i = 0; i < raw.size(); ++i) {
        std::string_view single_char(&raw[i], 1);
        ParseResult res = parser.Parse(single_char);

        if (i == raw.size() - 1) {
            EXPECT_TRUE(res.IsComplete());
            EXPECT_EQ(res.status, ParseStatus::Complete);
        } else {
            EXPECT_FALSE(res.IsComplete());
            EXPECT_TRUE(res.IsIncomplete());
        }
    }

    const auto& req = parser.GetRequest();
    EXPECT_EQ(req.method, HttpMethod::GET);
    EXPECT_EQ(req.path, "/fragmented-index.html");
    EXPECT_EQ(req.GetHeader("Host"), "sub.domain.org");
    EXPECT_EQ(req.GetHeader("X-Custom-Header"), "value_12345");
}

TEST(HttpParserPartialStreamReassembly, SingleByteFragmentedPostWithContentLength) {
    HttpParser parser;
    std::string body = "Deterministic payload reassembly";
    std::string raw =
        "POST /submit HTTP/1.1\r\n"
        "Host: test.io\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "\r\n" + body;

    for (size_t i = 0; i < raw.size(); ++i) {
        std::string_view single_char(&raw[i], 1);
        ParseResult res = parser.Parse(single_char);

        if (i == raw.size() - 1) {
            EXPECT_TRUE(res.IsComplete());
            EXPECT_EQ(res.status, ParseStatus::Complete);
        } else {
            EXPECT_FALSE(res.IsComplete());
        }
    }

    const auto& req = parser.GetRequest();
    EXPECT_EQ(req.method, HttpMethod::POST);
    EXPECT_EQ(req.content_length, body.size());
    EXPECT_EQ(parser.GetBody(), body);
}

TEST(HttpParserPartialStreamReassembly, SingleByteFragmentedChunkedStream) {
    HttpParser parser;
    std::string raw =
        "POST /chunked-byte HTTP/1.1\r\n"
        "Host: stream.org\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "4\r\nWiki\r\n"
        "5\r\npedia\r\n"
        "0\r\n\r\n";

    for (size_t i = 0; i < raw.size(); ++i) {
        std::string_view single_char(&raw[i], 1);
        ParseResult res = parser.Parse(single_char);

        if (i == raw.size() - 1) {
            EXPECT_TRUE(res.IsComplete());
            EXPECT_EQ(res.status, ParseStatus::Complete);
        } else {
            EXPECT_FALSE(res.IsComplete());
        }
    }

    EXPECT_EQ(parser.GetDecodedBody(), "Wikipedia");
}

TEST(HttpParserPartialStreamReassembly, FragmentedChunkBoundaries) {
    HttpParser parser;
    // Feed headers first
    std::string headers =
        "POST /split HTTP/1.1\r\n"
        "Host: stream.org\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n";
    ParseResult r1 = parser.Parse(headers);
    EXPECT_EQ(r1.status, ParseStatus::HeadersComplete);

    // Split 1: chunk size across boundary ("1" then "0\r\n" = 16 bytes)
    parser.Parse("1");
    parser.Parse("0\r\n");

    // Split 2: chunk data across 3 fragments (5 bytes + 6 bytes + 5 bytes = 16 bytes)
    parser.Parse("12345");
    parser.Parse("67890a");
    parser.Parse("bcdef");

    // Split 3: chunk CRLF across boundary ("\r" then "\n")
    parser.Parse("\r");
    parser.Parse("\n");

    // Split 4: Terminal chunk
    ParseResult rFinal = parser.Parse("0\r\n\r\n");
    EXPECT_TRUE(rFinal.IsComplete());
    EXPECT_EQ(parser.GetDecodedBody(), "1234567890abcdef");
}

TEST(HttpParserPartialStreamReassembly, PipelinedRequestsInSingleBuffer) {
    HttpParser parser;
    std::string req1 =
        "GET /request-one HTTP/1.1\r\n"
        "Host: pipeline.com\r\n"
        "\r\n";
    std::string req2 =
        "GET /request-two HTTP/1.1\r\n"
        "Host: pipeline.com\r\n"
        "\r\n";
    std::string combined = req1 + req2;

    // Parse request 1
    ParseResult res1 = parser.Parse(combined);
    EXPECT_TRUE(res1.IsComplete());
    EXPECT_EQ(res1.bytes_consumed, req1.size());
    EXPECT_EQ(parser.GetRequest().path, "/request-one");

    // Advance buffer and reset parser
    std::string_view remainder(combined.data() + res1.bytes_consumed, combined.size() - res1.bytes_consumed);
    parser.Reset();

    // Parse request 2
    ParseResult res2 = parser.Parse(remainder);
    EXPECT_TRUE(res2.IsComplete());
    EXPECT_EQ(res2.bytes_consumed, req2.size());
    EXPECT_EQ(parser.GetRequest().path, "/request-two");
}

TEST(HttpParserPartialStreamReassembly, MoveConstructionRebasing) {
    HttpParser p1;
    // Feed partial headers to trigger reassembly storage
    p1.Parse("GET /moved HTTP/1.1\r\n");
    p1.Parse("Host: example.org\r\n\r\n");
    EXPECT_TRUE(p1.GetRequest().is_complete);

    // Move construct p2 from p1
    HttpParser p2(std::move(p1));
    EXPECT_EQ(p2.GetRequest().path, "/moved");
    EXPECT_EQ(p2.GetRequest().GetHeader("Host"), "example.org");
}
