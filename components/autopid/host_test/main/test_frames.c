/**
 * @file test_frames.c
 * @brief Host suite for the frame of reference of a reply (issue #1014,
 *        2026-10-24): user expressions index the reply as the chip prints
 *        it with headers on (ap_resp_frames), the way the legacy firmware
 *        and the published vehicle profiles count. The vectors are the
 *        chip's own prints from the bench (an extended-addressed BMW SME
 *        played on the PCAN) and from Car Scanner logs (a Hyundai Ioniq
 *        2021 BMS, a Nissan Leaf AZE0, a BMW i3), plus the MEB 29-bit
 *        print of issue #914. Also here: the extended address byte before
 *        the PCI, the hinted 29-bit id, the headers-off rebuild, and the
 *        print-format commands dropped from an init chain. Run from
 *        test_main.c's app_main.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

#include "autopid_private.h"
#include "expression_parser.h"

static double eval(const char *expr, const uint8_t *bytes, size_t n)
{
    double v = 0;

    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK,
                              expression_parser_eval(expr, bytes, n, 0.0,
                                                     &v),
                              expr);
    return v;
}

static size_t frames(const char *resp, uint8_t *out, size_t cap)
{
    size_t n = 0;

    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, ap_resp_frames(resp, out, cap, &n),
                              resp);
    return n;
}

static size_t payload(const char *resp, uint8_t *out, size_t cap)
{
    size_t n = 0;

    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK,
                              ap_resp_to_payload(resp, out, cap, &n), resp);
    return n;
}

void test_frames_bmw_extended_addressing(void)
{
    /* the chip's own print, bench 2026-10-24 (an SME at address 07 played
       on the PCAN; the iX3 reply of issue #886): the address byte F1
       first, then the PCI; B0 is the address byte, as the legacy firmware
       and the published BMW rows count */
    uint8_t f[AP_FRAMES_MAX], p[AP_PAYLOAD_MAX];
    size_t n = frames("607 F1 10 09 62 E5 CE 20 6E \r"
                      "607 F1 21 20 59 20 81 AA AA \r", f, sizeof(f));
    const uint8_t want[] =
    {
        0xF1, 0x10, 0x09, 0x62, 0xE5, 0xCE, 0x20, 0x6E,
        0xF1, 0x21, 0x20, 0x59, 0x20, 0x81, 0xAA, 0xAA,
    };

    TEST_ASSERT_EQUAL_size_t(sizeof(want), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, f, sizeof(want));
    TEST_ASSERT_DOUBLE_WITHIN(0.001, 83.02, eval("[B6:B7]/100", f, n));

    /* the framing-free payload behind the address byte: the parsers and
       the cross-talk guard */
    const uint8_t pl[] = { 0x62, 0xE5, 0xCE, 0x20, 0x6E, 0x20, 0x59, 0x20,
                           0x81 };

    n = payload("607 F1 10 09 62 E5 CE 20 6E \r"
                "607 F1 21 20 59 20 81 AA AA \r", p, sizeof(p));
    TEST_ASSERT_EQUAL_size_t(sizeof(pl), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(pl, p, sizeof(pl));
    TEST_ASSERT_TRUE(ap_payload_matches_cmd("22E5CE", p, n));

    /* a response-pending frame behind the address byte is dropped, the
       single-frame answer stays (the chip trims it to its length) */
    const uint8_t sf[] = { 0xF1, 0x05, 0x62, 0xDD, 0xBC, 0x02, 0x33 };

    n = frames("607 F1 03 7F 22 78 \r607 F1 05 62 DD BC 02 33 \r", f,
               sizeof(f));
    TEST_ASSERT_EQUAL_size_t(sizeof(sf), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(sf, f, sizeof(sf));
    TEST_ASSERT_DOUBLE_WITHIN(0.001, 56.3, eval("[B5:B6]/10", f, n));

    n = payload("607 F1 03 7F 22 78 \r607 F1 05 62 DD BC 02 33 \r", p,
                sizeof(p));
    TEST_ASSERT_EQUAL_size_t(5, n);
    TEST_ASSERT_EQUAL_HEX8(0x62, p[0]);
    TEST_ASSERT_EQUAL_HEX8(0x33, p[4]);

    /* the i3's DDBC from a Car Scanner log (June 2024, spaces added): a
       pending frame, then the two-frame answer. The owner verified the
       SOC as the first two data bytes (issue #929): [B6:B7]/10 = 72.6 %.
       The published i3 row reads [B11:B12]/10, which is the SOC only when
       the pending frame is counted, as the legacy firmware did: here it
       is something else */
    n = frames("607 F1 03 7F 22 78 \r"
               "607 F1 10 09 62 DD BC 02 D6 \r"
               "607 F1 21 03 1C 00 50 FF FF \r", f, sizeof(f));
    TEST_ASSERT_EQUAL_size_t(16, n);
    TEST_ASSERT_EQUAL_HEX8(0xF1, f[0]);
    TEST_ASSERT_EQUAL_HEX8(0x10, f[1]);
    TEST_ASSERT_DOUBLE_WITHIN(0.001, 72.6, eval("[B6:B7]/10", f, n));
    TEST_ASSERT_DOUBLE_WITHIN(0.001, 716.8, eval("[B11:B12]/10", f, n));
}

void test_frames_hyundai_bms_nine_frames(void)
{
    /* a Hyundai Ioniq 2021 (38 kWh) answering 220101, from a Car Scanner
       log (spaces added): nine frames, 62 bytes. The published Hyundai
       rows: SOC B10/2 = 75.0 %, HV_V [B19:B20]/10 = 341.9 V */
    const char *resp =
        "7EC 10 3E 62 01 01 FF FD E7 \r"
        "7EC 21 FF 96 00 00 00 00 83 \r"
        "7EC 22 02 80 0D 5B 1F 1C 1E \r"
        "7EC 23 1E 1C 1C 1D 00 1B C2 \r"
        "7EC 24 0C C2 1F 00 00 8E 00 \r"
        "7EC 25 00 68 D5 00 00 68 F4 \r"
        "7EC 26 00 00 23 ED 00 00 22 \r"
        "7EC 27 E3 00 20 94 CD 0D 01 \r"
        "7EC 28 55 16 F4 16 F4 0B B8 \r";
    uint8_t f[AP_FRAMES_MAX], p[AP_PAYLOAD_MAX];
    size_t n = frames(resp, f, sizeof(f));

    TEST_ASSERT_EQUAL_size_t(72, n);                /* 9 frames of 8 */
    TEST_ASSERT_EQUAL_HEX8(0x10, f[0]);
    TEST_ASSERT_EQUAL_HEX8(0x3E, f[1]);
    TEST_ASSERT_EQUAL_HEX8(0x21, f[8]);
    TEST_ASSERT_EQUAL_HEX8(0x96, f[10]);
    TEST_ASSERT_EQUAL_DOUBLE(75.0, eval("B10/2", f, n));
    TEST_ASSERT_DOUBLE_WITHIN(0.001, 341.9, eval("[B19:B20]/10", f, n));
    TEST_ASSERT_EQUAL_HEX8(0x28, f[64]);
    TEST_ASSERT_EQUAL_HEX8(0xB8, f[71]);

    n = payload(resp, p, sizeof(p));
    TEST_ASSERT_EQUAL_size_t(62, n);
    TEST_ASSERT_EQUAL_HEX8(0x62, p[0]);
    TEST_ASSERT_EQUAL_HEX8(0xB8, p[61]);
}

void test_frames_leaf_aze0_padding_kept(void)
{
    /* a Nissan Leaf AZE0 answering 2102 (cell voltages), from a Car Scanner
       log: the last frame's zero padding is printed and counted; the
       framing-free payload is trimmed to the announced 0x26 bytes */
    const char *resp =
        "7EA 10 26 61 02 FF FF FF FF \r"
        "7EA 21 C4 C5 C5 C5 C5 C4 C5 \r"
        "7EA 22 C5 C5 C4 C5 C5 C5 C5 \r"
        "7EA 23 C5 C5 C5 C5 C5 C5 C5 \r"
        "7EA 24 C5 C5 C5 C5 C5 C5 C5 \r"
        "7EA 25 C4 C5 C5 C5 00 00 00 \r";
    uint8_t f[AP_FRAMES_MAX], p[AP_PAYLOAD_MAX];
    size_t n = frames(resp, f, sizeof(f));

    TEST_ASSERT_EQUAL_size_t(48, n);
    TEST_ASSERT_EQUAL_HEX8(0x25, f[40]);
    TEST_ASSERT_EQUAL_HEX8(0xC5, f[44]);
    TEST_ASSERT_EQUAL_HEX8(0x00, f[47]);

    n = payload(resp, p, sizeof(p));
    TEST_ASSERT_EQUAL_size_t(0x26, n);
}

void test_frames_29bit_id_with_hint(void)
{
    /* the MEB print of issue #914 (4.51 on a Skoda Enyaq): the id is not a
       legislated one, so without a hint the line is data */
    uint8_t f[AP_FRAMES_MAX], p[AP_PAYLOAD_MAX];
    size_t n;

    ap_resp_set_id29_hint(0, false);
    n = frames("17 FE 00 7B 04 62 02 8C D2 \r", f, sizeof(f));
    TEST_ASSERT_EQUAL_HEX8(0x10, f[0]);     /* rebuilt as 9 plain bytes */
    TEST_ASSERT_EQUAL_HEX8(0x17, f[2]);

    /* the row's receive filter names it: the four tokens are the id, B4
       is the first data byte (the published MEB SOC row: B4/2.5) */
    const uint8_t want[] = { 0x04, 0x62, 0x02, 0x8C, 0xD2 };

    ap_resp_set_id29_hint(0x17FE007B, false);
    n = frames("17 FE 00 7B 04 62 02 8C D2 \r", f, sizeof(f));
    TEST_ASSERT_EQUAL_size_t(sizeof(want), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, f, sizeof(want));
    TEST_ASSERT_EQUAL_DOUBLE(84.0, eval("B4/2.5", f, n));

    n = payload("17 FE 00 7B 04 62 02 8C D2 \r", p, sizeof(p));
    TEST_ASSERT_EQUAL_size_t(4, n);
    TEST_ASSERT_EQUAL_HEX8(0x62, p[0]);

    /* a 29-bit protocol in effect: any id, with a PCI behind it */
    ap_resp_set_id29_hint(0, true);
    n = frames("17 FE 00 7B 10 08 62 1E 3D 00 02 49 \r"
               "17 FE 00 7B 21 F0 43 AA AA AA AA AA \r", f, sizeof(f));
    TEST_ASSERT_EQUAL_size_t(16, n);
    TEST_ASSERT_EQUAL_HEX8(0x10, f[0]);
    TEST_ASSERT_EQUAL_HEX8(0x21, f[8]);
    /* the HV current as the reporter decoded it on 4.51: B5 B6 B7 B9 */
    TEST_ASSERT_EQUAL_DOUBLE(
        0.0, eval("(B5*16777216+B6*65536+B7*256+B9-150000)/100", f, n));

    /* a plain headers-off reply is not mistaken for an id even then */
    n = frames("62 F4 A6 00 12 D6 87 \r", f, sizeof(f));
    TEST_ASSERT_EQUAL_size_t(8, n);
    TEST_ASSERT_EQUAL_HEX8(0x07, f[0]);
    TEST_ASSERT_EQUAL_HEX8(0x62, f[1]);

    ap_resp_set_id29_hint(0, false);

    /* the legislated id needs no hint, as before */
    const uint8_t rpm[] = { 0x05, 0x62, 0xF4, 0x0C, 0x13, 0x88 };

    n = frames("18 DA F1 00 05 62 F4 0C 13 88 \r", f, sizeof(f));
    TEST_ASSERT_EQUAL_size_t(sizeof(rpm), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(rpm, f, sizeof(rpm));
}

void test_frames_headers_off_rebuilt(void)
{
    /* a reply without ids (a K-line protocol's prelude keeps headers off):
       the frames the chip would have printed are rebuilt, so [B3:B4]/4 is
       the RPM there too */
    uint8_t f[AP_FRAMES_MAX];
    const uint8_t rpm[] = { 0x04, 0x41, 0x0C, 0x1A, 0xF8 };
    size_t n = frames("41 0C 1A F8\r", f, sizeof(f));

    TEST_ASSERT_EQUAL_size_t(sizeof(rpm), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(rpm, f, sizeof(rpm));
    TEST_ASSERT_EQUAL_DOUBLE(1726.0, eval("[B3:B4]/4", f, n));

    /* ISO-TP rows: first frame of six, consecutive frames of seven, the
       last padded with zeros */
    const uint8_t vin[] =
    {
        0x10, 0x14, 0x49, 0x02, 0x01, 0x31, 0x34, 0x33,
        0x21, 0x31, 0x39, 0x32, 0x30, 0x33, 0x37, 0x38,
        0x22, 0x39, 0x33, 0x34, 0x35, 0x36, 0x00, 0x00,
    };

    n = frames("014\r0: 49 02 01 31 34 33\r1: 31 39 32 30 33 37 38\r"
               "2: 39 33 34 35 36 00 00\r", f, sizeof(f));
    TEST_ASSERT_EQUAL_size_t(sizeof(vin), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(vin, f, sizeof(vin));

    /* a reply over 16 rows: the rows in arrival order (the chip prints
       `0:` to `F:` and wraps), 28 rows of the Leaf ZE1's 2102 */
    static char text[1200];
    size_t w = 0;
    uint8_t want[195];

    for (size_t i = 0; i < sizeof(want); i++)
    {
        want[i] = (uint8_t)i;
    }

    w += (size_t)snprintf(text + w, sizeof(text) - w, "0C3\r");

    size_t at = 0;

    for (int row = 0; row < 28; row++)
    {
        size_t take = (row == 0) ? 6 : 7;

        w += (size_t)snprintf(text + w, sizeof(text) - w, "%X:",
                              row & 0x0F);

        for (size_t j = 0; j < take; j++)
        {
            w += (size_t)snprintf(text + w, sizeof(text) - w, " %02X",
                                  want[at++]);
        }

        w += (size_t)snprintf(text + w, sizeof(text) - w, "\r");
    }

    static uint8_t out[256];

    n = payload(text, out, sizeof(out));
    TEST_ASSERT_EQUAL_size_t(195, n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, out, sizeof(want));

    /* the rebuild works from the parsers' 128-byte payload: 18 frames */
    n = frames(text, f, sizeof(f));
    TEST_ASSERT_EQUAL_size_t(8 + 18 * 8, n);
    TEST_ASSERT_EQUAL_HEX8(0x10, f[0]);
    TEST_ASSERT_EQUAL_HEX8(0x21, f[8]);
    TEST_ASSERT_EQUAL_HEX8(0x20, f[8 + 15 * 8]);    /* the 16th wraps */
    TEST_ASSERT_EQUAL_HEX8(want[6 + 15 * 7], f[8 + 15 * 8 + 1]);

    /* a missing row: the assembly stops at the gap */
    n = payload("01B\r0: 62 01 01 00 01 02\r1: 03 04 05 06 07 08 09\r"
                "3: 11 12 13 14 15 16 17\r", out, sizeof(out));
    TEST_ASSERT_EQUAL_size_t(13, n);
}

void test_frames_lowest_responder(void)
{
    /* two ECUs answer a functional request: the lowest id's frame alone,
       as the framing-free payload does (the legacy firmware's rule once
       more than two frames came from different ids) */
    uint8_t f[AP_FRAMES_MAX];
    const uint8_t ecm[] = { 0x06, 0x41, 0x00, 0xBE, 0x3F, 0xA8, 0x13 };
    size_t n = frames("7E9 06 41 00 80 00 00 01 \r"
                      "7E8 06 41 00 BE 3F A8 13 \r", f, sizeof(f));

    TEST_ASSERT_EQUAL_size_t(sizeof(ecm), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ecm, f, sizeof(ecm));

    /* a plain pending frame with headers on is dropped too */
    const uint8_t rpm[] = { 0x04, 0x41, 0x0C, 0x1A, 0xF8 };

    n = frames("7E8 03 7F 01 78 \r7E8 04 41 0C 1A F8 \r", f, sizeof(f));
    TEST_ASSERT_EQUAL_size_t(sizeof(rpm), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(rpm, f, sizeof(rpm));

    /* an error line is an error, an empty reply nothing */
    size_t m = 0;

    TEST_ASSERT_EQUAL(ESP_FAIL, ap_resp_frames("NO DATA\r", f, sizeof(f),
                                               &m));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, ap_resp_frames("\r", f, sizeof(f),
                                                        &m));
}

void test_chain_strip_print_and_cra(void)
{
    /* the Mini Cooper SE init: ATS0 goes, the rest stays in order */
    char chain[AP_INIT_LEN];

    strcpy(chain, "ATSP6;ATS0;ATM0;ATAT1;ATCEA07;ATTAF1;ATFCSH6F1;ATSH6F1;"
                  "ATFCSD07300000;ATFCSM1;ATCRA607;");
    TEST_ASSERT_EQUAL(1, ap_chain_strip_print(chain));
    TEST_ASSERT_EQUAL_STRING("ATSP6;ATM0;ATAT1;ATCEA07;ATTAF1;ATFCSH6F1;"
                             "ATSH6F1;ATFCSD07300000;ATFCSM1;ATCRA607;",
                             chain);

    /* the old std_init advice ended in ATH0: the last element goes with
       the separator before it */
    strcpy(chain, "ATSP6;ATSH7DF;ATCRA;ATH0");
    TEST_ASSERT_EQUAL(1, ap_chain_strip_print(chain));
    TEST_ASSERT_EQUAL_STRING("ATSP6;ATSH7DF;ATCRA", chain);

    /* spaces and case as the chip reads them; ATH1, ATS1, ATE0 stay */
    strcpy(chain, "at h0 ;ATE1; ats0;ATH1;ATS1;ATE0");
    TEST_ASSERT_EQUAL(3, ap_chain_strip_print(chain));
    TEST_ASSERT_EQUAL_STRING("ATH1;ATS1;ATE0", chain);

    strcpy(chain, "ATH0");
    TEST_ASSERT_EQUAL(1, ap_chain_strip_print(chain));
    TEST_ASSERT_EQUAL_STRING("", chain);

    strcpy(chain, "ATH01;ATS00;ATSH7E0");
    TEST_ASSERT_EQUAL(0, ap_chain_strip_print(chain));
    TEST_ASSERT_EQUAL_STRING("ATH01;ATS00;ATSH7E0", chain);
    TEST_ASSERT_EQUAL(0, ap_chain_strip_print(NULL));

    /* the receive filter a chain sets */
    uint32_t id = 0;
    bool ext = false;

    TEST_ASSERT_TRUE(ap_chain_cra_id("ATSP7;ATCP17;ATSHFC007B;ATFCSH17FC007B;"
                                     "ATCRA17FE007B;", &id, &ext));
    TEST_ASSERT_EQUAL_HEX32(0x17FE007B, id);
    TEST_ASSERT_TRUE(ext);

    TEST_ASSERT_TRUE(ap_chain_cra_id("ATSH7E0; at cra 7e8", &id, &ext));
    TEST_ASSERT_EQUAL_HEX32(0x7E8, id);
    TEST_ASSERT_FALSE(ext);

    TEST_ASSERT_FALSE(ap_chain_cra_id("ATCRA7E8;ATCRA", &id, &ext));
    TEST_ASSERT_FALSE(ap_chain_cra_id("ATSH6F1;ATCEA60", &id, &ext));
    TEST_ASSERT_FALSE(ap_chain_cra_id("", &id, &ext));
    TEST_ASSERT_FALSE(ap_chain_cra_id(NULL, &id, &ext));
}

void test_frames_std_table_numbering(void)
{
    /* the standard table counts the printed frame: RPM `04 41 0C 1A F8`
       is [B3:B4]*0.25, coolant `03 41 05 7B` is B3-40 */
    char expr[AP_EXPR_LEN];
    const uint8_t rpm[] = { 0x04, 0x41, 0x0C, 0x1A, 0xF8 };
    const uint8_t ect[] = { 0x03, 0x41, 0x05, 0x7B };

    TEST_ASSERT_EQUAL(ESP_OK, ap_std_expression(31, 16, 0.25, 0, expr,
                                                sizeof(expr)));
    TEST_ASSERT_EQUAL_STRING("[B3:B4]*0.25", expr);
    TEST_ASSERT_EQUAL_DOUBLE(1726.0, eval(expr, rpm, sizeof(rpm)));

    TEST_ASSERT_EQUAL(ESP_OK, ap_std_expression(31, 8, 1.0, -40, expr,
                                                sizeof(expr)));
    TEST_ASSERT_EQUAL_STRING("B3-40", expr);
    TEST_ASSERT_EQUAL_DOUBLE(83.0, eval(expr, ect, sizeof(ect)));
}

void test_frames_std_rows_refreshed(void)
{
    /* a config stored by a build before 2026-10-24: its standard rows count
       the framing-free payload. At load they get the table's current
       expressions; a custom row and an unknown command are left alone */
    const char *json =
        "{\"pids\":["
        "{\"name\":\"EngineRPM\",\"type\":\"std\",\"cmd\":\"010C\","
        "\"parameters\":[{\"name\":\"EngineRPM\",\"expression\":\"[B2:B3]*0.25\","
        "\"unit\":\"rpm\",\"min\":0,\"max\":16384}]},"
        "{\"name\":\"EngineCoolantTemp\",\"type\":\"std\",\"cmd\":\"01051\","
        "\"parameters\":[{\"name\":\"EngineCoolantTemp\",\"expression\":\"B2-40\"}]},"
        "{\"name\":\"EngineRPM_uds\",\"type\":\"std\",\"cmd\":\"22F40C\","
        "\"parameters\":[{\"name\":\"EngineRPM_2\",\"expression\":\"[B3:B4]*0.25\"}]},"
        "{\"name\":\"soc\",\"type\":\"custom\",\"cmd\":\"220101\","
        "\"parameters\":[{\"name\":\"SOC\",\"expression\":\"B2/2\"}]},"
        "{\"name\":\"odd\",\"type\":\"std\",\"cmd\":\"PGN:F004\","
        "\"parameters\":[{\"name\":\"EngineRPM\",\"expression\":\"B3\"}]}"
        "]}";
    int changed = 0;
    char *out = ap_config_refresh_std(json, &changed);

    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQUAL(3, changed);
    TEST_ASSERT_NOT_NULL(strstr(out, "\"expression\":\"[B3:B4]*0.25\""));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"expression\":\"B3-40\""));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"expression\":\"[B4:B5]*0.25\""));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"expression\":\"B2/2\""));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"expression\":\"B3\"}"));
    TEST_ASSERT_NULL(strstr(out, "[B2:B3]"));

    /* a second pass finds nothing to change */
    int again = 0;
    char *out2 = ap_config_refresh_std(out, &again);

    TEST_ASSERT_NULL(out2);
    TEST_ASSERT_EQUAL(0, again);
    free(out);

    /* not JSON: not ours to judge */
    TEST_ASSERT_NULL(ap_config_refresh_std("{\"pids\": [", &again));
    TEST_ASSERT_EQUAL(0, again);
}

void run_frames_tests(void)
{
    RUN_TEST(test_frames_std_rows_refreshed);
    RUN_TEST(test_frames_bmw_extended_addressing);
    RUN_TEST(test_frames_hyundai_bms_nine_frames);
    RUN_TEST(test_frames_leaf_aze0_padding_kept);
    RUN_TEST(test_frames_29bit_id_with_hint);
    RUN_TEST(test_frames_headers_off_rebuilt);
    RUN_TEST(test_frames_lowest_responder);
    RUN_TEST(test_chain_strip_print_and_cra);
    RUN_TEST(test_frames_std_table_numbering);
}
