/**
 * @file test_vehicle.c
 * @brief Host suite for autopid_vehicle_core.c (TASK_quick_setup.md):
 *        ATDPN shapes, VIN from 0902 / 22F190 replies as the chip prints
 *        them, the responder fingerprint, the effective-protocol +
 *        prelude selection, the first-pass vehicle.json import and the
 *        ATSP-in-a-profile-chain guard rule. The store index has its own
 *        file (test_vehicle_index.c). Run from test_main.c's app_main.
 */
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "autopid_private.h"

/* the ECU simulator's VIN, 1WCAN0FW0P0000001, as 0902 / 22F190 bytes */
#define SIM_VIN_HEX "31 57 43 41 4E 30 46 57 30 50 30 30 30 30 30 30 31"
#define SIM_VIN     "1WCAN0FW0P0000001"

/* ---- ATDPN ------------------------------------------------------------------ */

void test_veh_dpn_shapes(void)
{
    char out[AP_VEH_PROTO_LEN];

    TEST_ASSERT_TRUE(ap_veh_parse_dpn("A6", out));
    TEST_ASSERT_EQUAL_STRING("6", out);
    TEST_ASSERT_TRUE(ap_veh_parse_dpn("6", out));
    TEST_ASSERT_EQUAL_STRING("6", out);
    TEST_ASSERT_TRUE(ap_veh_parse_dpn("A8\r>", out));
    TEST_ASSERT_EQUAL_STRING("8", out);
    TEST_ASSERT_TRUE(ap_veh_parse_dpn("\r\nA7\r\r", out));
    TEST_ASSERT_EQUAL_STRING("7", out);
    TEST_ASSERT_TRUE(ap_veh_parse_dpn("a9", out));          /* case */
    TEST_ASSERT_EQUAL_STRING("9", out);
    TEST_ASSERT_TRUE(ap_veh_parse_dpn("AA", out));          /* auto, A */
    TEST_ASSERT_EQUAL_STRING("A", out);
    TEST_ASSERT_TRUE(ap_veh_parse_dpn("C", out));
    TEST_ASSERT_EQUAL_STRING("C", out);
    TEST_ASSERT_TRUE(ap_veh_parse_dpn("1", out));

    /* rejected: a bare 'A' (no protocol after the auto flag), '?', 0,
       D+, garbage, trailing junk, empty, NULL */
    TEST_ASSERT_FALSE(ap_veh_parse_dpn("A", out));
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_FALSE(ap_veh_parse_dpn("?", out));
    TEST_ASSERT_FALSE(ap_veh_parse_dpn("0", out));
    TEST_ASSERT_FALSE(ap_veh_parse_dpn("A0", out));
    TEST_ASSERT_FALSE(ap_veh_parse_dpn("D", out));
    TEST_ASSERT_FALSE(ap_veh_parse_dpn("SEARCHING...", out));
    TEST_ASSERT_FALSE(ap_veh_parse_dpn("6X", out));
    TEST_ASSERT_FALSE(ap_veh_parse_dpn("NO DATA", out));
    TEST_ASSERT_FALSE(ap_veh_parse_dpn("", out));
    TEST_ASSERT_FALSE(ap_veh_parse_dpn(NULL, out));
}

/* ---- VIN --------------------------------------------------------------------- */

void test_veh_vin_valid(void)
{
    TEST_ASSERT_TRUE(ap_veh_vin_valid(SIM_VIN));
    TEST_ASSERT_TRUE(ap_veh_vin_valid("WVWZZZ1KZ7W000001"));
    TEST_ASSERT_FALSE(ap_veh_vin_valid("WVWZZZ1KZ7W00000"));    /* 16 */
    TEST_ASSERT_FALSE(ap_veh_vin_valid("WVWZZZ1KZ7W0000011"));  /* 18 */
    TEST_ASSERT_FALSE(ap_veh_vin_valid("WVWZZZ1KZ7W00000I"));   /* I */
    TEST_ASSERT_FALSE(ap_veh_vin_valid("WVWZZZ1KZ7W00000O"));   /* O */
    TEST_ASSERT_FALSE(ap_veh_vin_valid("WVWZZZ1KZ7W00000Q"));   /* Q */
    TEST_ASSERT_FALSE(ap_veh_vin_valid("wvwzzz1kz7w000001"));   /* case */
    TEST_ASSERT_FALSE(ap_veh_vin_valid("WVWZZZ1KZ7W00000-"));
    TEST_ASSERT_FALSE(ap_veh_vin_valid(""));
    TEST_ASSERT_FALSE(ap_veh_vin_valid(NULL));
}

void test_veh_vin_0902_single_line(void)
{
    char vin[AP_VIN_LEN];

    /* headers off, one line (a chip that joins the frames itself) */
    TEST_ASSERT_TRUE(ap_veh_parse_vin_0902("49 02 01 " SIM_VIN_HEX "\r",
                                           vin));
    TEST_ASSERT_EQUAL_STRING(SIM_VIN, vin);

    /* SEARCHING noise first */
    TEST_ASSERT_TRUE(ap_veh_parse_vin_0902(
        "SEARCHING...\r49 02 01 " SIM_VIN_HEX "\r\r", vin));
    TEST_ASSERT_EQUAL_STRING(SIM_VIN, vin);
}

void test_veh_vin_0902_isotp_shapes(void)
{
    char vin[AP_VIN_LEN];

    /* headers off, the ISO-TP rows as the chip prints them */
    const char *off =
        "014\r"
        "0: 49 02 01 31 57 43\r"
        "1: 41 4E 30 46 57 30 50\r"
        "2: 30 30 30 30 30 30 31\r";

    TEST_ASSERT_TRUE(ap_veh_parse_vin_0902(off, vin));
    TEST_ASSERT_EQUAL_STRING(SIM_VIN, vin);

    /* headers on, multi-frame from the engine ECU */
    const char *on =
        "7E8 10 14 49 02 01 31 57 43\r"
        "7E8 21 41 4E 30 46 57 30 50\r"
        "7E8 22 30 30 30 30 30 30 31\r";

    TEST_ASSERT_TRUE(ap_veh_parse_vin_0902(on, vin));
    TEST_ASSERT_EQUAL_STRING(SIM_VIN, vin);

    /* two ECUs answer: the lowest id (engine) wins */
    const char *two =
        "7E9 10 14 49 02 01 5A 5A 5A\r"
        "7E8 10 14 49 02 01 31 57 43\r"
        "7E9 21 5A 5A 5A 5A 5A 5A 5A\r"
        "7E8 21 41 4E 30 46 57 30 50\r"
        "7E9 22 5A 5A 5A 5A 5A 5A 5A\r"
        "7E8 22 30 30 30 30 30 30 31\r";

    TEST_ASSERT_TRUE(ap_veh_parse_vin_0902(two, vin));
    TEST_ASSERT_EQUAL_STRING(SIM_VIN, vin);
}

void test_veh_vin_0902_rejects(void)
{
    char vin[AP_VIN_LEN];

    TEST_ASSERT_FALSE(ap_veh_parse_vin_0902("NO DATA\r", vin));
    TEST_ASSERT_EQUAL_STRING("", vin);
    TEST_ASSERT_FALSE(ap_veh_parse_vin_0902("SEARCHING...\rNO DATA\r", vin));
    TEST_ASSERT_FALSE(ap_veh_parse_vin_0902("UNABLE TO CONNECT\r", vin));
    TEST_ASSERT_FALSE(ap_veh_parse_vin_0902("?\r", vin));
    TEST_ASSERT_FALSE(ap_veh_parse_vin_0902("", vin));
    TEST_ASSERT_FALSE(ap_veh_parse_vin_0902(NULL, vin));

    /* the old bench transcript: 15 chars + 00 padding, not a VIN */
    const char *padded =
        "014\r"
        "0: 49 02 01 31 34 33\r"
        "1: 31 39 32 30 33 37 38\r"
        "2: 39 33 34 35 36 00 00\r";

    TEST_ASSERT_FALSE(ap_veh_parse_vin_0902(padded, vin));

    /* non-ASCII bytes where the VIN should be */
    TEST_ASSERT_FALSE(ap_veh_parse_vin_0902(
        "49 02 01 FF FE 43 41 4E 30 46 57 30 50 30 30 30 30 30 30 31\r",
        vin));

    /* a letter the VIN alphabet excludes (I = 0x49) */
    TEST_ASSERT_FALSE(ap_veh_parse_vin_0902(
        "49 02 01 49 57 43 41 4E 30 46 57 30 50 30 30 30 30 30 30 31\r",
        vin));

    /* a different service echo (0902 asked, 0100 answered: cross-talk) */
    TEST_ASSERT_FALSE(ap_veh_parse_vin_0902("41 00 BE 7F B8 13\r", vin));

    /* too short */
    TEST_ASSERT_FALSE(ap_veh_parse_vin_0902("49 02 01 31 57 43\r", vin));
}

void test_veh_vin_22f190(void)
{
    char vin[AP_VIN_LEN];

    TEST_ASSERT_TRUE(ap_veh_parse_vin_22f190("62 F1 90 " SIM_VIN_HEX "\r",
                                             vin));
    TEST_ASSERT_EQUAL_STRING(SIM_VIN, vin);

    const char *off =
        "014\r"
        "0: 62 F1 90 31 57 43\r"
        "1: 41 4E 30 46 57 30 50\r"
        "2: 30 30 30 30 30 30 31\r";

    TEST_ASSERT_TRUE(ap_veh_parse_vin_22f190(off, vin));
    TEST_ASSERT_EQUAL_STRING(SIM_VIN, vin);

    /* NRC 0x31 requestOutOfRange (the simulator some days), NO DATA */
    TEST_ASSERT_FALSE(ap_veh_parse_vin_22f190("7F 22 31\r", vin));
    TEST_ASSERT_EQUAL_STRING("", vin);
    TEST_ASSERT_FALSE(ap_veh_parse_vin_22f190("NO DATA\r", vin));

    /* a 0902 shape is not a 22F190 answer and vice versa */
    TEST_ASSERT_FALSE(ap_veh_parse_vin_22f190("49 02 01 " SIM_VIN_HEX "\r",
                                              vin));
    TEST_ASSERT_FALSE(ap_veh_parse_vin_0902("62 F1 90 " SIM_VIN_HEX "\r",
                                            vin));
}

/* ---- fingerprint ------------------------------------------------------------------ */

void test_veh_ecus_from_0100(void)
{
    ap_veh_ecu_t ecus[AP_RESP_ECUS_MAX];

    /* two responders, headers on (11-bit) */
    int n = ap_veh_ecus_from_0100(
        "SEARCHING...\r7E8 06 41 00 BE 7F B8 13\r7E9 06 41 00 80 00 00 01\r",
        ecus, AP_RESP_ECUS_MAX);

    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_EQUAL_HEX32(0x7E8, ecus[0].id);
    TEST_ASSERT_EQUAL_HEX32(0xBE7FB813, ecus[0].bitmap);
    TEST_ASSERT_EQUAL_HEX32(0x7E9, ecus[1].id);
    TEST_ASSERT_EQUAL_HEX32(0x80000001, ecus[1].bitmap);

    /* 29-bit header */
    n = ap_veh_ecus_from_0100("18DAF110 06 41 00 BE 7F B8 13\r", ecus,
                              AP_RESP_ECUS_MAX);
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_EQUAL_HEX32(0x18DAF110, ecus[0].id);

    /* headers off: one UINT32_MAX entry, rows OR-merged */
    n = ap_veh_ecus_from_0100("41 00 BE 7F B8 13\r41 00 00 00 00 01\r",
                              ecus, AP_RESP_ECUS_MAX);
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_EQUAL_HEX32(UINT32_MAX, ecus[0].id);
    TEST_ASSERT_EQUAL_HEX32(0xBE7FB813, ecus[0].bitmap);

    /* noise only */
    TEST_ASSERT_EQUAL(0, ap_veh_ecus_from_0100("NO DATA\r", ecus,
                                               AP_RESP_ECUS_MAX));
    TEST_ASSERT_EQUAL(0, ap_veh_ecus_from_0100("7E8 03 41 05 5A\r", ecus,
                                               AP_RESP_ECUS_MAX));
    TEST_ASSERT_EQUAL(0, ap_veh_ecus_from_0100("", ecus, AP_RESP_ECUS_MAX));
}

void test_veh_fingerprint_deterministic_and_order_free(void)
{
    ap_veh_ecu_t a[] =
    {
        { 0x7E8, 0xBE7FB813 }, { 0x7E9, 0x80000001 }, { 0x7EA, 0x00000000 },
    };
    ap_veh_ecu_t b[] =
    {
        { 0x7EA, 0x00000000 }, { 0x7E8, 0xBE7FB813 }, { 0x7E9, 0x80000001 },
    };
    ap_veh_ecu_t dup[] =
    {
        { 0x7E9, 0x80000000 }, { 0x7E8, 0xBE7FB813 }, { 0x7E9, 0x00000001 },
        { 0x7EA, 0x00000000 },
    };
    char fa[AP_FP_LEN], fb[AP_FP_LEN], fd[AP_FP_LEN], f1[AP_FP_LEN];

    ap_veh_fingerprint(a, 3, fa);
    ap_veh_fingerprint(a, 3, fb);
    TEST_ASSERT_EQUAL(8, strlen(fa));
    TEST_ASSERT_EQUAL_STRING(fa, fb);           /* deterministic */

    ap_veh_fingerprint(b, 3, fb);
    TEST_ASSERT_EQUAL_STRING(fa, fb);           /* order independent */

    ap_veh_fingerprint(dup, 4, fd);
    TEST_ASSERT_EQUAL_STRING(fa, fd);           /* duplicate id merged */

    /* lowercase hex only */
    for (const char *p = fa; *p != '\0'; p++)
    {
        TEST_ASSERT_TRUE((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'));
    }

    /* a different car (one bitmap bit) = a different print */
    ap_veh_ecu_t c[] =
    {
        { 0x7E8, 0xBE7FB812 }, { 0x7E9, 0x80000001 }, { 0x7EA, 0x00000000 },
    };

    ap_veh_fingerprint(c, 3, f1);
    TEST_ASSERT_FALSE(strcmp(fa, f1) == 0);

    /* fewer ECUs = a different print too */
    ap_veh_fingerprint(a, 2, f1);
    TEST_ASSERT_FALSE(strcmp(fa, f1) == 0);

    /* nothing = "" */
    ap_veh_fingerprint(a, 0, f1);
    TEST_ASSERT_EQUAL_STRING("", f1);
    ap_veh_fingerprint(NULL, 3, f1);
    TEST_ASSERT_EQUAL_STRING("", f1);

    /* FNV-1a reference: one pair id=0, bitmap=0 hashes 8 zero bytes */
    ap_veh_ecu_t z[] = { { 0, 0 } };

    ap_veh_fingerprint(z, 1, f1);
    TEST_ASSERT_EQUAL_STRING("9be17165", f1);
}

/* ---- protocol selection + prelude --------------------------------------------------- */

void test_veh_effective_protocol_and_prelude(void)
{
    /* a pinned setting wins over the store, fallback or not */
    TEST_ASSERT_EQUAL('6', ap_veh_effective_protocol("6", "9", false));
    TEST_ASSERT_EQUAL('7', ap_veh_effective_protocol("7", "", true));

    /* "0": the current car's protocol, unless it went silent this boot */
    TEST_ASSERT_EQUAL('9', ap_veh_effective_protocol("0", "9", false));
    TEST_ASSERT_EQUAL('A', ap_veh_effective_protocol("0", "a", false));
    TEST_ASSERT_EQUAL('0', ap_veh_effective_protocol("0", "9", true));
    TEST_ASSERT_EQUAL('0', ap_veh_effective_protocol("0", "", false));
    TEST_ASSERT_EQUAL('0', ap_veh_effective_protocol("0", NULL, false));
    TEST_ASSERT_EQUAL('0', ap_veh_effective_protocol("", "", false));
    TEST_ASSERT_EQUAL('0', ap_veh_effective_protocol(NULL, NULL, false));
    TEST_ASSERT_EQUAL('0', ap_veh_effective_protocol("0", "D", false));
    TEST_ASSERT_EQUAL('0', ap_veh_effective_protocol("0", "0", false));

    /* the legacy map for 6..9 with headers ON (2026-10-24, issue #1014:
       the replies are read as the chip prints them with the id),
       protocol-only for the rest; K-line and the search keep headers off */
    TEST_ASSERT_EQUAL_STRING("ATS1;ATH1;ATST96;ATTP6;ATSH7DF;ATCRA",
                             ap_veh_prelude_for('6'));
    TEST_ASSERT_EQUAL_STRING("ATS1;ATH1;ATST96;ATTP7;ATSH18DB33F1;ATCRA",
                             ap_veh_prelude_for('7'));
    TEST_ASSERT_EQUAL_STRING("ATS1;ATH1;ATST96;ATTP8;ATSH7DF;ATCRA",
                             ap_veh_prelude_for('8'));
    TEST_ASSERT_EQUAL_STRING("ATS1;ATH1;ATST96;ATTP9;ATSH18DB33F1;ATCRA",
                             ap_veh_prelude_for('9'));
    TEST_ASSERT_EQUAL_STRING("ATS1;ATH0;ATST96;ATTP0",
                             ap_veh_prelude_for('0'));
    TEST_ASSERT_EQUAL_STRING("ATS1;ATH0;ATST96;ATTP0",
                             ap_veh_prelude_for('\0'));
    TEST_ASSERT_EQUAL_STRING("ATS1;ATH0;ATST96;ATTP3",
                             ap_veh_prelude_for('3'));
    TEST_ASSERT_EQUAL_STRING("ATS1;ATH1;ATST96;ATTPA",
                             ap_veh_prelude_for('A'));

    TEST_ASSERT_TRUE(ap_veh_proto_is_29bit('7'));
    TEST_ASSERT_TRUE(ap_veh_proto_is_29bit('9'));
    TEST_ASSERT_TRUE(ap_veh_proto_is_29bit('A'));
    TEST_ASSERT_FALSE(ap_veh_proto_is_29bit('6'));
    TEST_ASSERT_FALSE(ap_veh_proto_is_29bit('8'));
    TEST_ASSERT_FALSE(ap_veh_proto_is_29bit('0'));

    TEST_ASSERT_TRUE(ap_veh_proto_valid('1'));
    TEST_ASSERT_TRUE(ap_veh_proto_valid('9'));
    TEST_ASSERT_TRUE(ap_veh_proto_valid('C'));
    TEST_ASSERT_FALSE(ap_veh_proto_valid('0'));
    TEST_ASSERT_FALSE(ap_veh_proto_valid('D'));
    TEST_ASSERT_FALSE(ap_veh_proto_valid('a'));   /* callers upcase first */
}

/* ---- the first-pass vehicle.json (imported once into the store) ------------------ */

void test_veh_legacy_doc_import(void)
{
    ap_vehicle_doc_t back;

    /* a first-pass file as the device wrote it */
    TEST_ASSERT_TRUE(ap_veh_doc_from_json(
        "{\"version\":1,\"vin\":\"" SIM_VIN "\",\"protocol\":\"6\","
        "\"fingerprint\":\"deadbeef\",\"detected_ts\":1759300000,"
        "\"seen_vin\":\"WVWZZZ1KZ7W000001\",\"seen_fingerprint\":\"0badf00d\","
        "\"changed\":true}",
        &back));
    TEST_ASSERT_EQUAL_STRING(SIM_VIN, back.vin);
    TEST_ASSERT_EQUAL_STRING("6", back.protocol);
    TEST_ASSERT_EQUAL_STRING("deadbeef", back.fingerprint);
    TEST_ASSERT_EQUAL_INT64(1759300000, back.detected_ts);
    TEST_ASSERT_TRUE(back.changed);
    TEST_ASSERT_FALSE(ap_veh_doc_empty(&back));

    /* an empty first-pass document imports nothing */
    TEST_ASSERT_TRUE(ap_veh_doc_from_json(
        "{\"version\":1,\"vin\":\"\",\"protocol\":\"\",\"fingerprint\":\"\","
        "\"detected_ts\":0,\"seen_vin\":\"\",\"seen_fingerprint\":\"\","
        "\"changed\":false}",
        &back));
    TEST_ASSERT_TRUE(ap_veh_doc_empty(&back));
    TEST_ASSERT_TRUE(ap_veh_doc_empty(NULL));

    /* garbage / array / no version / version 2 / empty / NULL rejected */
    TEST_ASSERT_FALSE(ap_veh_doc_from_json("garbage", &back));
    TEST_ASSERT_TRUE(ap_veh_doc_empty(&back));
    TEST_ASSERT_FALSE(ap_veh_doc_from_json("[1,2]", &back));
    TEST_ASSERT_FALSE(ap_veh_doc_from_json("{\"vin\":\"" SIM_VIN "\"}",
                                           &back));
    TEST_ASSERT_FALSE(ap_veh_doc_from_json("{\"version\":2}", &back));
    TEST_ASSERT_FALSE(ap_veh_doc_from_json("", &back));
    TEST_ASSERT_FALSE(ap_veh_doc_from_json(NULL, &back));

    /* missing keys = "", a hand-edited bad VIN / protocol is dropped */
    TEST_ASSERT_TRUE(ap_veh_doc_from_json(
        "{\"version\":1,\"vin\":\"not a vin\",\"protocol\":\"X\"}", &back));
    TEST_ASSERT_EQUAL_STRING("", back.vin);
    TEST_ASSERT_EQUAL_STRING("", back.protocol);
    TEST_ASSERT_EQUAL_STRING("", back.fingerprint);
    TEST_ASSERT_EQUAL_INT64(0, back.detected_ts);
    TEST_ASSERT_FALSE(back.changed);

    /* a lowercase stored protocol survives (the prelude upcases) */
    TEST_ASSERT_TRUE(ap_veh_doc_from_json("{\"version\":1,\"protocol\":\"a\"}",
                                          &back));
    TEST_ASSERT_EQUAL_STRING("a", back.protocol);
}

/* ---- the EEPROM guard on a profile init chain (protocol policy) ------------------- */

void test_veh_init_chain_atsp_stays_ram_only(void)
{
    /* a mixed-protocol profile (VW MEB: standard PIDs on 11-bit, UDS on
       29-bit) names ATSP7 inside its per-PID init: the guard rewrites it
       to ATTP7 (RAM only) wherever it sits in the chain. The chip's base
       protocol is learned ONCE through obd_chip_protocol_save(), never
       through an init string. */
    char chain[AP_INIT_LEN];

    snprintf(chain, sizeof(chain), "ATSP7;ATSH17FC007B;ATCRA17FE007B;ATFCSM1");
    ap_init_sanitize(chain);
    TEST_ASSERT_EQUAL_STRING("ATTP7;ATSH17FC007B;ATCRA17FE007B;ATFCSM1",
                             chain);

    snprintf(chain, sizeof(chain), "ATSH7DF;ATCRA;atsp 7;ATM1");
    ap_init_sanitize(chain);
    TEST_ASSERT_EQUAL_STRING("ATSH7DF;ATCRA;ATTP 7;ATM0", chain);

    /* the std prelude's own ATTP6 and headers pass untouched */
    snprintf(chain, sizeof(chain), "%s", ap_veh_prelude_for('6'));
    ap_init_sanitize(chain);
    TEST_ASSERT_EQUAL_STRING(ap_veh_prelude_for('6'), chain);
}

/* ---- runner ------------------------------------------------------------------------ */

void run_vehicle_tests(void)
{
    RUN_TEST(test_veh_dpn_shapes);
    RUN_TEST(test_veh_vin_valid);
    RUN_TEST(test_veh_vin_0902_single_line);
    RUN_TEST(test_veh_vin_0902_isotp_shapes);
    RUN_TEST(test_veh_vin_0902_rejects);
    RUN_TEST(test_veh_vin_22f190);
    RUN_TEST(test_veh_ecus_from_0100);
    RUN_TEST(test_veh_fingerprint_deterministic_and_order_free);
    RUN_TEST(test_veh_effective_protocol_and_prelude);
    RUN_TEST(test_veh_legacy_doc_import);
    RUN_TEST(test_veh_init_chain_atsp_stays_ram_only);
}

