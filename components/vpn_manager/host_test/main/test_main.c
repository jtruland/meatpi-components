/**
 * @file test_main.c
 * @brief Host tests for vpn_manager's pure config checks: WireGuard
 *        key shape, endpoint sanity, the enabled-config ladder, and
 *        the tailscale validation branch.
 *        Plus the bitz fork split-tunnel route parser and the home-SSID
 *        list match.
 *        Expected output: 12 Tests 0 Failures 0 Ignored.
 */
#include <string.h>

#include "unity.h"

#include "vpn_manager_private.h"

/* a shape-valid (not cryptographically meaningful) 44-char b64 key */
#define GOOD_KEY "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOP8="

static vpn_config_t s_cfg;

void setUp(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.enabled = true;
    strcpy(s_cfg.private_key, GOOD_KEY);
    strcpy(s_cfg.peer_public_key, GOOD_KEY);
    strcpy(s_cfg.address, "10.6.0.2");
    strcpy(s_cfg.allowed_ip, "10.6.0.0");
    strcpy(s_cfg.allowed_ip_mask, "255.255.255.0");
    strcpy(s_cfg.endpoint, "vpn.example.com");
    s_cfg.port = 51820;
    s_cfg.keepalive_s = 25;
}

void tearDown(void)
{
}

static void test_key_shape(void)
{
    TEST_ASSERT_TRUE(vpn_check_wg_key(GOOD_KEY));
    TEST_ASSERT_FALSE(vpn_check_wg_key(NULL));
    TEST_ASSERT_FALSE(vpn_check_wg_key(""));
    TEST_ASSERT_FALSE(vpn_check_wg_key("too-short="));
    /* right length, missing '=' terminator */
    TEST_ASSERT_FALSE(vpn_check_wg_key(
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOP88"));
    /* illegal char in the body */
    TEST_ASSERT_FALSE(vpn_check_wg_key(
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMN!P8="));
}

static void test_endpoint_shape(void)
{
    TEST_ASSERT_TRUE(vpn_check_endpoint("vpn.example.com"));
    TEST_ASSERT_TRUE(vpn_check_endpoint("192.168.1.10"));
    TEST_ASSERT_FALSE(vpn_check_endpoint(""));
    TEST_ASSERT_FALSE(vpn_check_endpoint(NULL));
    TEST_ASSERT_FALSE(vpn_check_endpoint("bad host"));
    TEST_ASSERT_FALSE(vpn_check_endpoint("host;rm"));
}

static void test_disabled_config_always_ok(void)
{
    vpn_config_t cfg;

    memset(&cfg, 0, sizeof(cfg)); /* empty AND disabled */
    TEST_ASSERT_NULL(vpn_check_config(&cfg));
}

static void test_enabled_config_valid(void)
{
    TEST_ASSERT_NULL(vpn_check_config(&s_cfg));
    /* preshared key is optional... */
    s_cfg.preshared_key[0] = '\0';
    TEST_ASSERT_NULL(vpn_check_config(&s_cfg));
    /* ...but must be well-formed when present */
    strcpy(s_cfg.preshared_key, "nonsense");
    TEST_ASSERT_NOT_NULL(vpn_check_config(&s_cfg));
}

static void test_enabled_config_requires_keys(void)
{
    s_cfg.private_key[0] = '\0';
    TEST_ASSERT_NOT_NULL(vpn_check_config(&s_cfg));
    setUp();
    s_cfg.peer_public_key[0] = '\0';
    TEST_ASSERT_NOT_NULL(vpn_check_config(&s_cfg));
}

static void test_enabled_config_requires_endpoint_and_address(void)
{
    s_cfg.endpoint[0] = '\0';
    TEST_ASSERT_NOT_NULL(vpn_check_config(&s_cfg));
    setUp();
    s_cfg.address[0] = '\0';
    TEST_ASSERT_NOT_NULL(vpn_check_config(&s_cfg));
    setUp();
    s_cfg.port = 0;
    TEST_ASSERT_NOT_NULL(vpn_check_config(&s_cfg));
}

static void ts_setup(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.enabled = true;
    s_cfg.tailscale = true;
    strcpy(s_cfg.ts_auth_key,
           "tskey-auth-k0123456789abcdef0123456789abcdef");
}

static void test_tailscale_valid_configs(void)
{
    /* official coordinator: auth key alone is enough, no WG fields */
    ts_setup();
    TEST_ASSERT_NULL(vpn_check_config(&s_cfg));

    /* headscale hex preauth key */
    ts_setup();
    strcpy(s_cfg.ts_auth_key,
           "1f34a1e2b3c4d5e6f708192a3b4c5d6e7f8091a2b3c4d5e6");
    TEST_ASSERT_NULL(vpn_check_config(&s_cfg));

    /* custom coordinator: bare host, host:port, bare IP:port */
    ts_setup();
    strcpy(s_cfg.ts_control_url, "headscale.example.com");
    TEST_ASSERT_NULL(vpn_check_config(&s_cfg));
    strcpy(s_cfg.ts_control_url, "10.42.0.1:8080");
    TEST_ASSERT_NULL(vpn_check_config(&s_cfg));

    /* device name is free-form/optional */
    ts_setup();
    strcpy(s_cfg.ts_device_name, "wican-garage");
    TEST_ASSERT_NULL(vpn_check_config(&s_cfg));
}

static void test_tailscale_rejects(void)
{
    /* auth key required (shape check: min length) */
    ts_setup();
    s_cfg.ts_auth_key[0] = '\0';
    TEST_ASSERT_NOT_NULL(vpn_check_config(&s_cfg));
    ts_setup();
    strcpy(s_cfg.ts_auth_key, "short");
    TEST_ASSERT_NOT_NULL(vpn_check_config(&s_cfg));

    /* control_url must be a clean host[:port]: no scheme, no spaces */
    ts_setup();
    strcpy(s_cfg.ts_control_url, "http://10.42.0.1:8080");
    TEST_ASSERT_NOT_NULL(vpn_check_config(&s_cfg));
    ts_setup();
    strcpy(s_cfg.ts_control_url, "bad host");
    TEST_ASSERT_NOT_NULL(vpn_check_config(&s_cfg));

    /* type=tailscale must NOT demand WG fields, and a disabled
     * tailscale config is always storable */
    ts_setup();
    s_cfg.enabled = false;
    s_cfg.ts_auth_key[0] = '\0';
    TEST_ASSERT_NULL(vpn_check_config(&s_cfg));
}

/* ---- bitz fork: split-tunnel routes + home SSIDs ---- */

static void test_routes_parse(void)
{
    vpn_route_t r[VPN_MAX_ROUTES];

    TEST_ASSERT_EQUAL_INT(0, vpn_parse_routes("", r, VPN_MAX_ROUTES));
    TEST_ASSERT_EQUAL_INT(0, vpn_parse_routes(" , ", r, VPN_MAX_ROUTES));
    TEST_ASSERT_EQUAL_INT(2, vpn_parse_routes(
        " 192.168.86.0/24 ,10.200.0.1/24", r, VPN_MAX_ROUTES));
    TEST_ASSERT_EQUAL_HEX32(0xC0A85600, r[0].net);
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFF00, r[0].mask);
    TEST_ASSERT_EQUAL_HEX32(0x0AC80000, r[1].net); /* host bits dropped */

    TEST_ASSERT_TRUE(vpn_route_match(r, 2, 0xC0A85609));   /* .86.9  */
    TEST_ASSERT_TRUE(vpn_route_match(r, 2, 0x0AC80001));   /* 10.200.0.1 */
    TEST_ASSERT_FALSE(vpn_route_match(r, 2, 0x68150390)); /* public */

    /* a bare address is a /32; /0 matches everything */
    TEST_ASSERT_EQUAL_INT(1, vpn_parse_routes("1.2.3.4", r, VPN_MAX_ROUTES));
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFFFF, r[0].mask);
    TEST_ASSERT_EQUAL_INT(1, vpn_parse_routes("0.0.0.0/0", r, VPN_MAX_ROUTES));
    TEST_ASSERT_TRUE(vpn_route_match(r, 1, 0x08080808));
}

static void test_routes_reject(void)
{
    vpn_route_t r[2];

    TEST_ASSERT_EQUAL_INT(-1, vpn_parse_routes("192.168.86/24", r, 2));
    TEST_ASSERT_EQUAL_INT(-1, vpn_parse_routes("192.168.86.0/33", r, 2));
    TEST_ASSERT_EQUAL_INT(-1, vpn_parse_routes("256.1.1.1", r, 2));
    TEST_ASSERT_EQUAL_INT(-1, vpn_parse_routes("10.0.0.0/", r, 2));
    TEST_ASSERT_EQUAL_INT(-1, vpn_parse_routes("10.0.0.0/8x", r, 2));
    TEST_ASSERT_EQUAL_INT(-1, vpn_parse_routes("fd00::/8", r, 2));
    TEST_ASSERT_EQUAL_INT(-1, vpn_parse_routes("1.1.1.1,2.2.2.2,3.3.3.3", r, 2));
}

static void test_routes_in_config_check(void)
{
    strcpy(s_cfg.routes, "192.168.86.0/24, 10.200.0.0/24");
    TEST_ASSERT_NULL(vpn_check_config(&s_cfg));
    strcpy(s_cfg.routes, "192.168.86.0/24, bogus");
    TEST_ASSERT_NOT_NULL(vpn_check_config(&s_cfg));
    s_cfg.routes[0] = '\0';
    s_cfg.home_bypass = true;
    TEST_ASSERT_NOT_NULL(vpn_check_config(&s_cfg)); /* needs SSIDs */
    strcpy(s_cfg.home_ssids, "Bitztead");
    TEST_ASSERT_NULL(vpn_check_config(&s_cfg));
}

static void test_ssid_list(void)
{
    TEST_ASSERT_TRUE(vpn_ssid_in_list("Bitztead", "Bitztead"));
    TEST_ASSERT_TRUE(vpn_ssid_in_list("Bitz IoT", " Bitztead , Bitz IoT "));
    TEST_ASSERT_FALSE(vpn_ssid_in_list("bitztead", "Bitztead"));
    TEST_ASSERT_FALSE(vpn_ssid_in_list("Bitz", "Bitztead"));
    TEST_ASSERT_FALSE(vpn_ssid_in_list("", "Bitztead,"));
    TEST_ASSERT_FALSE(vpn_ssid_in_list("Bitztead", ""));
}

void app_main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_key_shape);
    RUN_TEST(test_endpoint_shape);
    RUN_TEST(test_disabled_config_always_ok);
    RUN_TEST(test_enabled_config_valid);
    RUN_TEST(test_enabled_config_requires_keys);
    RUN_TEST(test_enabled_config_requires_endpoint_and_address);
    RUN_TEST(test_tailscale_valid_configs);
    RUN_TEST(test_tailscale_rejects);
    RUN_TEST(test_routes_parse);
    RUN_TEST(test_routes_reject);
    RUN_TEST(test_routes_in_config_check);
    RUN_TEST(test_ssid_list);
    UNITY_END();
}
