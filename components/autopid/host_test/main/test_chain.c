/**
 * @file test_chain.c
 * @brief What a chain of the tables sets on the chip beyond the baseline
 *        (ap_chain_sets) and the chain that puts the baseline back
 *        (ap_chain_restore): autopid_chain.c, issue #964.
 */
#include <string.h>

#include "unity.h"

#include "autopid_private.h"

/* the baseline prelude of an 11-bit 500 kbit/s car (ap_veh_prelude_for;
   headers on since 2026-10-24, issue #1014) */
#define PRELUDE6 "ATS1;ATH1;ATST96;ATTP6;ATSH7DF;ATCRA"

/* the odometer row of issue #964 (a 2018 BMW 640i GT, the gateway behind
   extended addressing) */
#define BMW_INIT "ATSH6F1;ATCEA60;ATCRA660;ATFCSH6F1;ATFCSD60300000;ATFCSM1"

void test_chain_the_report(void)
{
    unsigned sets = ap_chain_sets(BMW_INIT);

    TEST_ASSERT_EQUAL_HEX(AP_SETS_HEADER | AP_SETS_EXT_ADDR |
                          AP_SETS_FILTER | AP_SETS_FLOW_CTL, sets);

    char out[AP_RESTORE_LEN];
    size_t n = ap_chain_restore(sets, PRELUDE6, out, sizeof(out));

    TEST_ASSERT_EQUAL_STRING(PRELUDE6 ";ATCEA;ATFCSM0", out);
    TEST_ASSERT_EQUAL(strlen(out), n);
}

void test_chain_published_profiles(void)
{
    /* BMW i3 (vehicle_profiles.json on main): adaptive timing off, the
       extended address, a typo (ATTAF1 is nobody's command), flow control,
       the header, the filter */
    unsigned i3 = ap_chain_sets("ATAT0;ATCEA07;ATTAF1;ATFCSH6F1;ATSH6F1;"
                                "ATFCSD07300000;ATFCSM1;ATCRA607;");

    TEST_ASSERT_EQUAL_HEX(AP_SETS_TIMING | AP_SETS_EXT_ADDR |
                          AP_SETS_FLOW_CTL | AP_SETS_HEADER | AP_SETS_FILTER,
                          i3);

    char out[AP_RESTORE_LEN];

    (void)ap_chain_restore(i3, PRELUDE6, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING(PRELUDE6 ";ATCEA;ATFCSM0;ATAT1", out);

    /* Mini Cooper SE: the protocol, spaces OFF (the parser's enemy), the
       memory switch (nothing of the baseline), adaptive timing ON (the
       default: nothing) */
    unsigned mini = ap_chain_sets("ATSP6;ATS0;ATM0;ATAT1;ATCEA07;ATTAF1;"
                                  "ATFCSH6F1;ATSH6F1;ATFCSD07300000;ATFCSM1;"
                                  "ATCRA607;");

    TEST_ASSERT_EQUAL_HEX(AP_SETS_PROTOCOL | AP_SETS_PRINT | AP_SETS_EXT_ADDR |
                          AP_SETS_FLOW_CTL | AP_SETS_HEADER | AP_SETS_FILTER,
                          mini);

    /* Fiat e-Ulysse: the protocol, the timeout, the header, flow control */
    TEST_ASSERT_EQUAL_HEX(AP_SETS_PROTOCOL | AP_SETS_PRINT | AP_SETS_HEADER |
                          AP_SETS_FLOW_CTL,
                          ap_chain_sets("ATSP6;ATST96;ATSH79B;ATFCSH79B;"
                                        "ATFCSD300000;ATFCSM1;"));

    /* a uds car's standard row: the header alone */
    TEST_ASSERT_EQUAL_HEX(AP_SETS_HEADER, ap_chain_sets("ATSH18DA58F1"));
}

void test_chain_reads_like_the_chip(void)
{
    /* spaces and case do not matter, nor the separators around */
    TEST_ASSERT_EQUAL_HEX(AP_SETS_HEADER | AP_SETS_EXT_ADDR,
                          ap_chain_sets(" at sh 6f1 ; ; AT cea 60;"));
    TEST_ASSERT_EQUAL_HEX(AP_SETS_HEADER, ap_chain_sets("atsh7e0"));

    /* the off / reset forms set nothing: they ARE the baseline (headers ON
       since 2026-10-24, issue #1014: ATH1 is the baseline's) */
    TEST_ASSERT_EQUAL_HEX(0, ap_chain_sets("ATCEA"));
    TEST_ASSERT_EQUAL_HEX(0, ap_chain_sets("ATCRA"));
    TEST_ASSERT_EQUAL_HEX(0, ap_chain_sets("ATFCSM0"));
    TEST_ASSERT_EQUAL_HEX(0, ap_chain_sets("ATCFC1;ATCAF1;ATAT1;ATE0;ATL0;"
                                           "ATR1;ATV0;ATD0;ATH1;ATS1"));

    /* the on forms do */
    TEST_ASSERT_EQUAL_HEX(AP_SETS_FC_OFF, ap_chain_sets("ATCFC0"));
    TEST_ASSERT_EQUAL_HEX(AP_SETS_RAW, ap_chain_sets("ATCAF0"));
    TEST_ASSERT_EQUAL_HEX(AP_SETS_TIMING, ap_chain_sets("ATAT2"));
    TEST_ASSERT_EQUAL_HEX(AP_SETS_PRIORITY, ap_chain_sets("ATCP1C"));
    TEST_ASSERT_EQUAL_HEX(AP_SETS_ECHO | AP_SETS_LINEFEED,
                          ap_chain_sets("ATE1;ATL1"));
    TEST_ASSERT_EQUAL_HEX(AP_SETS_QUIET | AP_SETS_DLC,
                          ap_chain_sets("ATR0;ATV1"));
    TEST_ASSERT_EQUAL_HEX(AP_SETS_DLC, ap_chain_sets("ATD1"));
    TEST_ASSERT_EQUAL_HEX(AP_SETS_PRINT, ap_chain_sets("ATH0"));
    TEST_ASSERT_EQUAL_HEX(AP_SETS_PRINT, ap_chain_sets("ATST32"));
    TEST_ASSERT_EQUAL_HEX(AP_SETS_FILTER, ap_chain_sets("ATCM7FF;ATCF7E8"));
    TEST_ASSERT_EQUAL_HEX(AP_SETS_EXT_ADDR, ap_chain_sets("ATCERF1"));
    TEST_ASSERT_EQUAL_HEX(AP_SETS_PROTOCOL, ap_chain_sets("ATTP7"));
    TEST_ASSERT_EQUAL_HEX(AP_SETS_PROTOCOL, ap_chain_sets("ATSPA6"));

    /* the resets, the displays, the reads: not the baseline's business
       (the bus guard and the chip driver's guard rule on those) */
    TEST_ASSERT_EQUAL_HEX(0, ap_chain_sets("ATZ;ATD;ATWS;ATDP;ATDPN;ATRV;"
                                           "ATI;ATM1;ATPB0000;ATCSM0"));

    /* no AT command at all, a request, nothing, NULL */
    TEST_ASSERT_EQUAL_HEX(0, ap_chain_sets("22D107"));
    TEST_ASSERT_EQUAL_HEX(0, ap_chain_sets("010C"));
    TEST_ASSERT_EQUAL_HEX(0, ap_chain_sets("XATSH7E0"));
    TEST_ASSERT_EQUAL_HEX(0, ap_chain_sets(""));
    TEST_ASSERT_EQUAL_HEX(0, ap_chain_sets(NULL));

    /* a command longer than any of the chip's is none of the table's */
    TEST_ASSERT_EQUAL_HEX(0, ap_chain_sets("ATSH0123456789012345678901234567"
                                           "89ABCDEF"));
}

void test_chain_restore_builds(void)
{
    char out[AP_RESTORE_LEN];

    /* nothing beyond what the prelude covers: the prelude alone */
    size_t n = ap_chain_restore(AP_SETS_HEADER | AP_SETS_FILTER |
                                AP_SETS_PROTOCOL | AP_SETS_PRINT,
                                PRELUDE6, out, sizeof(out));

    TEST_ASSERT_EQUAL_STRING(PRELUDE6, out);
    TEST_ASSERT_EQUAL(strlen(PRELUDE6), n);

    /* every bit: the longest restore fits AP_RESTORE_LEN with room */
    unsigned all = AP_SETS_HEADER | AP_SETS_FILTER | AP_SETS_PROTOCOL |
                   AP_SETS_PRINT | AP_SETS_EXT_ADDR | AP_SETS_FLOW_CTL |
                   AP_SETS_FC_OFF | AP_SETS_TIMING | AP_SETS_RAW |
                   AP_SETS_PRIORITY | AP_SETS_ECHO | AP_SETS_LINEFEED |
                   AP_SETS_QUIET | AP_SETS_DLC;
    const char *prelude9 = "ATS1;ATH1;ATST96;ATTP9;ATSH18DB33F1;ATCRA";

    n = ap_chain_restore(all, prelude9, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("ATS1;ATH1;ATST96;ATTP9;ATSH18DB33F1;ATCRA;"
                             "ATCEA;ATFCSM0;ATCFC1;ATAT1;ATCAF1;ATCP18;"
                             "ATE0;ATL0;ATR1;ATV0;ATD0", out);
    TEST_ASSERT_EQUAL(strlen(out), n);
    TEST_ASSERT_TRUE(n + 1 < AP_RESTORE_LEN);

    /* no prelude (a protocol without one): the undo commands alone */
    n = ap_chain_restore(AP_SETS_EXT_ADDR | AP_SETS_TIMING, "", out,
                         sizeof(out));
    TEST_ASSERT_EQUAL_STRING("ATCEA;ATAT1", out);
    TEST_ASSERT_EQUAL(strlen(out), n);

    /* a buffer too small: cut short, terminated, the length says so */
    char small[12];

    n = ap_chain_restore(all, PRELUDE6, small, sizeof(small));
    TEST_ASSERT_EQUAL(sizeof(small) - 1, n);
    TEST_ASSERT_EQUAL(sizeof(small) - 1, strlen(small));

    /* no room at all */
    TEST_ASSERT_EQUAL(0, ap_chain_restore(all, PRELUDE6, NULL, 0));
}

void run_chain_tests(void)
{
    RUN_TEST(test_chain_the_report);
    RUN_TEST(test_chain_published_profiles);
    RUN_TEST(test_chain_reads_like_the_chip);
    RUN_TEST(test_chain_restore_builds);
}
