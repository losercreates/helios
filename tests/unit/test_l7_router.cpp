#include <gtest/gtest.h>
#include "routing/l7_router.hpp"

using namespace helios;

TEST(L7RouterTest, ExactHostAndPathMatching) {
    L7Router router;
    router.AddRoute(L7RouteConfig{
        .name = "api_route",
        .host = "api.example.com",
        .path_prefix = "/api",
        .target_pool = "api_pool",
        .strip_prefix = false
    });
    router.AddRoute(L7RouteConfig{
        .name = "web_route",
        .host = "example.com",
        .path_prefix = "/",
        .target_pool = "web_pool",
        .strip_prefix = false
    });

    auto m1 = router.Match("api.example.com", "/api/v1/users");
    ASSERT_TRUE(m1.has_value());
    EXPECT_EQ(m1->target_pool, "api_pool");
    EXPECT_EQ(m1->rewritten_path, "/api/v1/users");

    auto m2 = router.Match("example.com", "/index.html");
    ASSERT_TRUE(m2.has_value());
    EXPECT_EQ(m2->target_pool, "web_pool");
}

TEST(L7RouterTest, WildcardHostMatching) {
    L7Router router;
    router.AddRoute(L7RouteConfig{
        .name = "wildcard_subdomains",
        .host = "*.example.com",
        .path_prefix = "/",
        .target_pool = "subdomain_pool"
    });

    auto m1 = router.Match("foo.example.com", "/hello");
    ASSERT_TRUE(m1.has_value());
    EXPECT_EQ(m1->target_pool, "subdomain_pool");

    auto m2 = router.Match("bar.baz.example.com:8080", "/world");
    ASSERT_TRUE(m2.has_value());
    EXPECT_EQ(m2->target_pool, "subdomain_pool");

    // Other domain should not match
    auto m3 = router.Match("example.org", "/hello");
    EXPECT_FALSE(m3.has_value());
}

TEST(L7RouterTest, LongestPathPrefixPrecedence) {
    L7Router router;
    router.AddRoute(L7RouteConfig{
        .name = "root_route",
        .host = "*",
        .path_prefix = "/",
        .target_pool = "default_pool"
    });
    router.AddRoute(L7RouteConfig{
        .name = "api_route",
        .host = "*",
        .path_prefix = "/api",
        .target_pool = "api_pool"
    });
    router.AddRoute(L7RouteConfig{
        .name = "api_v2_route",
        .host = "*",
        .path_prefix = "/api/v2",
        .target_pool = "api_v2_pool"
    });

    auto m1 = router.Match("example.com", "/api/v2/items");
    ASSERT_TRUE(m1.has_value());
    EXPECT_EQ(m1->target_pool, "api_v2_pool");

    auto m2 = router.Match("example.com", "/api/v1/items");
    ASSERT_TRUE(m2.has_value());
    EXPECT_EQ(m2->target_pool, "api_pool");

    auto m3 = router.Match("example.com", "/about");
    ASSERT_TRUE(m3.has_value());
    EXPECT_EQ(m3->target_pool, "default_pool");

    // Distinct prefix boundaries: /apidoc should NOT match /api prefix
    auto m4 = router.Match("example.com", "/apidoc");
    ASSERT_TRUE(m4.has_value());
    EXPECT_EQ(m4->target_pool, "default_pool");
}

TEST(L7RouterTest, StripPrefixBehavior) {
    L7Router router;
    router.AddRoute(L7RouteConfig{
        .name = "api_strip",
        .host = "*",
        .path_prefix = "/api/v1",
        .target_pool = "backend_pool",
        .strip_prefix = true
    });

    auto m1 = router.Match("example.com", "/api/v1/users/42");
    ASSERT_TRUE(m1.has_value());
    EXPECT_EQ(m1->rewritten_path, "/users/42");

    auto m2 = router.Match("example.com", "/api/v1");
    ASSERT_TRUE(m2.has_value());
    EXPECT_EQ(m2->rewritten_path, "/");

    auto m3 = router.Match("example.com", "/api/v1?query=test");
    ASSERT_TRUE(m3.has_value());
    EXPECT_EQ(m3->rewritten_path, "/?query=test");
}

TEST(L7RouterTest, FallbackToDefaultPool) {
    L7Router router;
    router.AddRoute(L7RouteConfig{
        .name = "api_route",
        .host = "api.example.com",
        .path_prefix = "/api",
        .target_pool = "api_pool"
    });
    router.SetDefaultPool("fallback_pool");

    // Matches route
    auto m1 = router.Match("api.example.com", "/api/test");
    ASSERT_TRUE(m1.has_value());
    EXPECT_EQ(m1->target_pool, "api_pool");

    // Unmatched host falls back to default pool
    auto m2 = router.Match("other.com", "/hello");
    ASSERT_TRUE(m2.has_value());
    EXPECT_EQ(m2->target_pool, "fallback_pool");
    EXPECT_EQ(m2->rewritten_path, "/hello");
}

TEST(L7RouterTest, NoMatchWithoutDefaultReturnsNullopt) {
    L7Router router;
    router.AddRoute(L7RouteConfig{
        .name = "api_route",
        .host = "api.example.com",
        .path_prefix = "/api",
        .target_pool = "api_pool"
    });

    auto m = router.Match("unknown.com", "/unknown");
    EXPECT_FALSE(m.has_value());
}
