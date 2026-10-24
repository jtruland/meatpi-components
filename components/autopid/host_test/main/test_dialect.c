/**
 * @file test_dialect.c
 * @brief Host suite for autopid_dialect.c (TASK_j1939_wwh.md phase 3):
 *        what an OBD dialect asks, who answered, how one responder is
 *        addressed (the reply parser's own cases of that phase are in
 *        test_resp_lines.c). The reply texts are
 *        the OBD chip's own, captured on the bench 2026-10-03 against a
 *        two-ECU ISO 27145 vehicle on 29-bit ids (trailing spaces
 *        included), plus the lines of a Mercedes Sprinter VS30 from the
 *        field report. Run from test_main.c's app_main.
 */
#include <string.h>

#include "unity.h"

#include "autopid_private.h"

/* the bench vehicle's VIN, 1WCANWWH0TRUCK001, as the chip prints it */
#define WWH_VIN "1WCANWWH0TRUCK001"

static const ap_bus_t SILENT = { AP_BUS_SILENT, 0 };
static const ap_bus_t LIVE_500 = { AP_BUS_LIVE, 500 };
static const ap_bus_t LIVE_250 = { AP_BUS_LIVE, 250 };
static const ap_bus_t LIVE_125 = { AP_BUS_LIVE, 125 };
static const ap_bus_t UNREADABLE = { AP_BUS_UNREADABLE, 0 };

/* ---- names and requests ------------------------------------------------------ */

void test_dialect_names(void)
{
    TEST_ASSERT_EQUAL_STRING("obd2", ap_dialect_name(AP_DIALECT_OBD2));
    TEST_ASSERT_EQUAL_STRING("uds", ap_dialect_name(AP_DIALECT_UDS));
    TEST_ASSERT_EQUAL_STRING("j1939", ap_dialect_name(AP_DIALECT_J1939));

    TEST_ASSERT_EQUAL(AP_DIALECT_UDS, ap_dialect_from_name("uds"));
    TEST_ASSERT_EQUAL(AP_DIALECT_J1939, ap_dialect_from_name("j1939"));
    TEST_ASSERT_EQUAL(AP_DIALECT_OBD2, ap_dialect_from_name("obd2"));

    /* a store written before dialects existed, or a hand edit */
    TEST_ASSERT_EQUAL(AP_DIALECT_OBD2, ap_dialect_from_name(""));
    TEST_ASSERT_EQUAL(AP_DIALECT_OBD2, ap_dialect_from_name(NULL));
    TEST_ASSERT_EQUAL(AP_DIALECT_OBD2, ap_dialect_from_name("UDS"));
    TEST_ASSERT_EQUAL(AP_DIALECT_OBD2, ap_dialect_from_name("wwh"));
}

void test_dialect_requests(void)
{
    char cmd[12];

    TEST_ASSERT_EQUAL_size_t(4, ap_dialect_pid_cmd(AP_DIALECT_OBD2, 0x0C,
                                                  cmd, sizeof(cmd)));
    TEST_ASSERT_EQUAL_STRING("010C", cmd);
    TEST_ASSERT_EQUAL_size_t(6, ap_dialect_pid_cmd(AP_DIALECT_UDS, 0x0C, cmd,
                                                  sizeof(cmd)));
    TEST_ASSERT_EQUAL_STRING("22F40C", cmd);
    TEST_ASSERT_EQUAL_size_t(6, ap_dialect_pid_cmd(AP_DIALECT_UDS, 0xE0, cmd,
                                                  sizeof(cmd)));
    TEST_ASSERT_EQUAL_STRING("22F4E0", cmd);

    /* no request rows on J1939; a small buffer is refused, not cut */
    TEST_ASSERT_EQUAL_size_t(0, ap_dialect_pid_cmd(AP_DIALECT_J1939, 0x0C,
                                                  cmd, sizeof(cmd)));
    TEST_ASSERT_EQUAL_STRING("", cmd);
    TEST_ASSERT_EQUAL_size_t(0, ap_dialect_pid_cmd(AP_DIALECT_UDS, 0x0C, cmd,
                                                  6));
    TEST_ASSERT_EQUAL_STRING("", cmd);
    TEST_ASSERT_EQUAL_size_t(0, ap_dialect_pid_cmd(AP_DIALECT_UDS, 0x0C,
                                                  NULL, 0));

    TEST_ASSERT_EQUAL_STRING("0902", ap_dialect_vin_cmd(AP_DIALECT_OBD2));
    TEST_ASSERT_EQUAL_STRING("22F802", ap_dialect_vin_cmd(AP_DIALECT_UDS));
    TEST_ASSERT_EQUAL_STRING("", ap_dialect_vin_cmd(AP_DIALECT_J1939));

    TEST_ASSERT_EQUAL(6, ap_dialect_ranges(AP_DIALECT_OBD2));
    TEST_ASSERT_EQUAL(8, ap_dialect_ranges(AP_DIALECT_UDS));
    TEST_ASSERT_EQUAL(0, ap_dialect_ranges(AP_DIALECT_J1939));

    TEST_ASSERT_EQUAL(0, ap_dialect_data_shift(AP_DIALECT_OBD2));
    TEST_ASSERT_EQUAL(1, ap_dialect_data_shift(AP_DIALECT_UDS));

    TEST_ASSERT_TRUE(ap_dialect_has_requests(AP_DIALECT_OBD2));
    TEST_ASSERT_TRUE(ap_dialect_has_requests(AP_DIALECT_UDS));
    TEST_ASSERT_FALSE(ap_dialect_has_requests(AP_DIALECT_J1939));
}

void test_dialect_expression_shift(void)
{
    char buf[AP_EXPR_LEN];

    /* EngineRPM in the table: bit_start 31, 16 bits, x0.25. The frame
       `04 41 0C A B` reads [B3:B4]; `05 62 F4 0C A B` reads one byte
       further (headers-on numbering, 2026-10-24) */
    TEST_ASSERT_EQUAL(ESP_OK, ap_std_expression_shift(31, 16, 0.25, 0, 0,
                                                      buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("[B3:B4]*0.25", buf);
    TEST_ASSERT_EQUAL(ESP_OK, ap_std_expression_shift(31, 16, 0.25, 0, 1,
                                                      buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("[B4:B5]*0.25", buf);

    /* coolant: one byte with an offset */
    TEST_ASSERT_EQUAL(ESP_OK, ap_std_expression_shift(31, 8, 1.0, -40, 1,
                                                      buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("B4-40", buf);

    /* placeholder rows and a negative shift are refused */
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED,
                      ap_std_expression_shift(0, 0, 1.0, 0, 1, buf,
                                              sizeof(buf)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      ap_std_expression_shift(31, 8, 1.0, 0, -1, buf,
                                              sizeof(buf)));
}

/* ---- support bitmaps: who answered ------------------------------------------- */

void test_dialect_bitmaps_obd2(void)
{
    ap_veh_ecu_t e[AP_VEH_ECUS_MAX];

    /* 11-bit, headers on, two responders */
    int n = ap_dialect_bitmaps(
        AP_DIALECT_OBD2,
        "SEARCHING...\r7E8 06 41 00 BE 7F B8 13\r7E9 06 41 00 80 00 00 01\r",
        0x00, e, AP_VEH_ECUS_MAX);

    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_EQUAL_HEX32(0x7E8, e[0].id);
    TEST_ASSERT_EQUAL_HEX32(0xBE7FB813, e[0].bitmap);
    TEST_ASSERT_EQUAL_HEX32(0x7E9, e[1].id);

    /* 29-bit as the chip prints it: the id is four byte tokens. Until
       2026-10-03 this line was one headers-off row with no id */
    n = ap_dialect_bitmaps(AP_DIALECT_OBD2,
                           "18 DA F1 10 06 41 00 BE 3F A8 13 \r"
                           "18 DA F1 18 06 41 00 80 00 00 01 \r",
                           0x00, e, AP_VEH_ECUS_MAX);
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_EQUAL_HEX32(0x18DAF110, e[0].id);
    TEST_ASSERT_EQUAL_HEX32(0xBE3FA813, e[0].bitmap);
    TEST_ASSERT_EQUAL_HEX32(0x18DAF118, e[1].id);

    /* headers off: one row without an id, every line OR-merged */
    n = ap_dialect_bitmaps(AP_DIALECT_OBD2,
                           "41 00 BE 7F B8 13\r41 00 00 00 00 01\r", 0x00, e,
                           AP_VEH_ECUS_MAX);
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_EQUAL_HEX32(UINT32_MAX, e[0].id);
    TEST_ASSERT_EQUAL_HEX32(0xBE7FB813, e[0].bitmap);

    /* another range than the one asked for is not an answer */
    TEST_ASSERT_EQUAL(0, ap_dialect_bitmaps(AP_DIALECT_OBD2,
                                            "7E8 06 41 20 80 00 00 01\r",
                                            0x00, e, AP_VEH_ECUS_MAX));
    TEST_ASSERT_EQUAL(1, ap_dialect_bitmaps(AP_DIALECT_OBD2,
                                            "7E8 06 41 20 80 00 00 01\r",
                                            0x20, e, AP_VEH_ECUS_MAX));

    /* a WWH answer is not an OBD-II one */
    TEST_ASSERT_EQUAL(0, ap_dialect_bitmaps(AP_DIALECT_OBD2,
                                            "62 F4 00 98 1B 80 03 \r", 0x00,
                                            e, AP_VEH_ECUS_MAX));
}

void test_dialect_bitmaps_uds_captured(void)
{
    ap_veh_ecu_t e[AP_VEH_ECUS_MAX];

    /* bench capture, headers on: 22F400 answered by 3D, then 00 */
    int n = ap_dialect_bitmaps(AP_DIALECT_UDS,
                               "18 DA F1 3D 07 62 F4 00 80 00 00 01 \r"
                               "18 DA F1 00 07 62 F4 00 98 1B 80 03 \r",
                               0x00, e, AP_VEH_ECUS_MAX);

    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_EQUAL_HEX32(0x18DAF13D, e[0].id);
    TEST_ASSERT_EQUAL_HEX32(0x80000001, e[0].bitmap);
    TEST_ASSERT_EQUAL_HEX32(0x18DAF100, e[1].id);
    TEST_ASSERT_EQUAL_HEX32(0x981B8003, e[1].bitmap);

    /* the same answers with headers off: nobody can be told apart */
    n = ap_dialect_bitmaps(AP_DIALECT_UDS,
                           "62 F4 00 98 1B 80 03 \r62 F4 00 80 00 00 01 \r",
                           0x00, e, AP_VEH_ECUS_MAX);
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_EQUAL_HEX32(UINT32_MAX, e[0].id);
    TEST_ASSERT_EQUAL_HEX32(0x981B8003, e[0].bitmap);

    /* the second range */
    n = ap_dialect_bitmaps(AP_DIALECT_UDS,
                           "18 DA F1 00 07 62 F4 20 00 02 20 01 \r"
                           "18 DA F1 3D 07 62 F4 20 00 00 00 11 \r",
                           0x20, e, AP_VEH_ECUS_MAX);
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_EQUAL_HEX32(0x00022001, e[0].bitmap);
    TEST_ASSERT_EQUAL_HEX32(0x00000011, e[1].bitmap);

    /* bench capture: an ECU's LATE answer to the previous request stands
       in front of this one. Asked for range 00 it is the only answer;
       asked for anything else it is nobody's */
    const char *stale = "18 DA F1 3D 07 62 F4 00 80 00 00 01 \r"
                        "18 DA F1 3D 05 62 F4 3C 10 68 \r";

    TEST_ASSERT_EQUAL(1, ap_dialect_bitmaps(AP_DIALECT_UDS, stale, 0x00, e,
                                            AP_VEH_ECUS_MAX));
    TEST_ASSERT_EQUAL(0, ap_dialect_bitmaps(AP_DIALECT_UDS, stale, 0x20, e,
                                            AP_VEH_ECUS_MAX));

    /* 11-bit (an SAE J1979-2 car) */
    n = ap_dialect_bitmaps(AP_DIALECT_UDS, "7E8 07 62 F4 00 98 1B 80 03\r",
                           0x00, e, AP_VEH_ECUS_MAX);
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_EQUAL_HEX32(0x7E8, e[0].id);
}

void test_dialect_bitmaps_sprinter(void)
{
    ap_veh_ecu_t e[AP_VEH_ECUS_MAX];

    /* the field report's 22F400 answers (ids in one token there) */
    int n = ap_dialect_bitmaps(AP_DIALECT_UDS,
                               "18DAF158 07 62 F4 00 98 18 A0 13\r"
                               "18DAF159 07 62 F4 00 98 18 00 01\r"
                               "18DAF15D 07 62 F4 00 98 18 00 01\r",
                               0x00, e, AP_VEH_ECUS_MAX);

    TEST_ASSERT_EQUAL(3, n);
    TEST_ASSERT_EQUAL_HEX32(0x18DAF158, e[0].id);
    TEST_ASSERT_EQUAL_HEX32(0x9818A013, e[0].bitmap);
    TEST_ASSERT_EQUAL_HEX32(0x18DAF159, e[1].id);
    TEST_ASSERT_EQUAL_HEX32(0x18DAF15D, e[2].id);
    TEST_ASSERT_EQUAL_HEX32(0x98180001, e[2].bitmap);
}

void test_dialect_bitmaps_noise_and_bounds(void)
{
    ap_veh_ecu_t e[AP_VEH_ECUS_MAX];

    TEST_ASSERT_EQUAL(0, ap_dialect_bitmaps(AP_DIALECT_UDS, "NO DATA\r", 0,
                                            e, AP_VEH_ECUS_MAX));
    TEST_ASSERT_EQUAL(0, ap_dialect_bitmaps(AP_DIALECT_UDS, "7F 22 31 \r", 0,
                                            e, AP_VEH_ECUS_MAX));
    TEST_ASSERT_EQUAL(0, ap_dialect_bitmaps(AP_DIALECT_UDS,
                                            "18 DA F1 00 03 7F 22 31 \r", 0,
                                            e, AP_VEH_ECUS_MAX));
    TEST_ASSERT_EQUAL(0, ap_dialect_bitmaps(AP_DIALECT_UDS,
                                            "UNABLE TO CONNECT\r", 0, e,
                                            AP_VEH_ECUS_MAX));
    TEST_ASSERT_EQUAL(0, ap_dialect_bitmaps(AP_DIALECT_UDS, "", 0, e,
                                            AP_VEH_ECUS_MAX));

    /* a bitmap cut short is not a bitmap */
    TEST_ASSERT_EQUAL(0, ap_dialect_bitmaps(AP_DIALECT_UDS,
                                            "62 F4 00 98 1B 80 \r", 0, e,
                                            AP_VEH_ECUS_MAX));

    /* no request rows on J1939, and the arguments are checked */
    TEST_ASSERT_EQUAL(0, ap_dialect_bitmaps(AP_DIALECT_J1939,
                                            "62 F4 00 98 1B 80 03 \r", 0, e,
                                            AP_VEH_ECUS_MAX));
    TEST_ASSERT_EQUAL(0, ap_dialect_bitmaps(AP_DIALECT_UDS, NULL, 0, e,
                                            AP_VEH_ECUS_MAX));
    TEST_ASSERT_EQUAL(0, ap_dialect_bitmaps(AP_DIALECT_UDS,
                                            "62 F4 00 98 1B 80 03 \r", 0,
                                            NULL, AP_VEH_ECUS_MAX));

    /* more responders than room: the first ones are kept */
    int n = ap_dialect_bitmaps(AP_DIALECT_UDS,
                               "18 DA F1 58 07 62 F4 00 98 18 A0 13 \r"
                               "18 DA F1 59 07 62 F4 00 98 18 00 01 \r"
                               "18 DA F1 5D 07 62 F4 00 98 18 00 01 \r",
                               0x00, e, 2);

    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_EQUAL_HEX32(0x18DAF159, e[1].id);

    /* the same id twice is one responder */
    n = ap_dialect_bitmaps(AP_DIALECT_UDS,
                           "18 DA F1 58 07 62 F4 00 98 00 00 00 \r"
                           "18 DA F1 58 07 62 F4 00 00 18 00 01 \r",
                           0x00, e, AP_VEH_ECUS_MAX);
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_EQUAL_HEX32(0x98180001, e[0].bitmap);
}

/* ---- VIN and protocol identification ----------------------------------------- */

void test_dialect_vin_captured(void)
{
    char vin[AP_VIN_LEN];

    /* headers off, one responder: the multi-frame rows */
    TEST_ASSERT_TRUE(ap_dialect_vin(
        AP_DIALECT_UDS,
        "014 \r0: 62 F8 02 31 57 43 \r1: 41 4E 57 57 48 30 54 \r"
        "2: 52 55 43 4B 30 30 31 \r",
        vin));
    TEST_ASSERT_EQUAL_STRING(WWH_VIN, vin);

    /* the ECU asked for more time first (7F 22 78) */
    TEST_ASSERT_TRUE(ap_dialect_vin(
        AP_DIALECT_UDS,
        "7F 22 78 \r014 \r0: 62 F8 02 31 57 43 \r1: 41 4E 57 57 48 30 54 \r"
        "2: 52 55 43 4B 30 30 31 \r",
        vin));
    TEST_ASSERT_EQUAL_STRING(WWH_VIN, vin);

    /* headers on, two responders interleaved, both pending first: the
       lowest responder's text */
    TEST_ASSERT_TRUE(ap_dialect_vin(
        AP_DIALECT_UDS,
        "18 DA F1 3D 03 7F 22 78 \r18 DA F1 00 03 7F 22 78 \r"
        "18 DA F1 00 10 14 62 F8 02 31 57 43 \r"
        "18 DA F1 3D 10 14 62 F8 02 31 57 43 \r"
        "18 DA F1 00 21 41 4E 57 57 48 30 54 \r"
        "18 DA F1 00 22 52 55 43 4B 30 30 31 \r"
        "18 DA F1 3D 21 41 4E 57 57 48 30 54 \r"
        "18 DA F1 3D 22 52 55 43 4B 30 30 31 \r",
        vin));
    TEST_ASSERT_EQUAL_STRING(WWH_VIN, vin);

    /* a count byte before the text, the 0902 habit (21 bytes) */
    TEST_ASSERT_TRUE(ap_dialect_vin(
        AP_DIALECT_UDS,
        "015 \r0: 62 F8 02 01 31 57 \r1: 43 41 4E 57 57 48 30 \r"
        "2: 54 52 55 43 4B 30 30 \r3: 31 \r",
        vin));
    TEST_ASSERT_EQUAL_STRING(WWH_VIN, vin);

    /* not supported, nothing, another identifier */
    TEST_ASSERT_FALSE(ap_dialect_vin(AP_DIALECT_UDS, "7F 22 31 \r", vin));
    TEST_ASSERT_EQUAL_STRING("", vin);
    TEST_ASSERT_FALSE(ap_dialect_vin(AP_DIALECT_UDS, "NO DATA\r", vin));
    TEST_ASSERT_FALSE(ap_dialect_vin(
        AP_DIALECT_UDS,
        "014 \r0: 62 F1 90 31 57 43 \r1: 41 4E 57 57 48 30 54 \r"
        "2: 52 55 43 4B 30 30 31 \r",
        vin));

    /* the other dialects */
    TEST_ASSERT_TRUE(ap_dialect_vin(
        AP_DIALECT_OBD2,
        "014\r0: 49 02 01 31 57 43\r1: 41 4E 57 57 48 30 54\r"
        "2: 52 55 43 4B 30 30 31\r",
        vin));
    TEST_ASSERT_EQUAL_STRING(WWH_VIN, vin);
    TEST_ASSERT_FALSE(ap_dialect_vin(AP_DIALECT_J1939, "anything", vin));
    TEST_ASSERT_EQUAL_STRING("", vin);
}

void test_dialect_protocol_id(void)
{
    uint8_t id = 0xEE;

    TEST_ASSERT_TRUE(ap_dialect_uds_protocol_id(
        "62 F8 10 01 \r62 F8 10 01 \r", &id));
    TEST_ASSERT_EQUAL_HEX8(0x01, id);

    id = 0xEE;
    TEST_ASSERT_TRUE(ap_dialect_uds_protocol_id(
        "18 DA F1 00 04 62 F8 10 02 \r18 DA F1 3D 04 62 F8 10 01 \r", &id));
    TEST_ASSERT_EQUAL_HEX8(0x02, id);       /* the first responder's word */

    id = 0xEE;
    TEST_ASSERT_FALSE(ap_dialect_uds_protocol_id("NO DATA\r", &id));
    TEST_ASSERT_EQUAL_HEX8(0xEE, id);
    TEST_ASSERT_FALSE(ap_dialect_uds_protocol_id("62 F8 10 \r", &id));
    TEST_ASSERT_FALSE(ap_dialect_uds_protocol_id(NULL, &id));
    TEST_ASSERT_FALSE(ap_dialect_uds_protocol_id("62 F8 10 01 \r", NULL));
}

/* ---- the scan's table -------------------------------------------------------- */

void test_dialect_table_owner(void)
{
    ap_dialect_ecu_t tab[4];
    int n = 0;

    /* the Sprinter: 58 (engine), 59 and 5D answer range 00 */
    const ap_veh_ecu_t r0[] =
    {
        { 0x18DAF158, 0x9818A013 }, { 0x18DAF159, 0x98180001 },
        { 0x18DAF15D, 0x98180001 },
    };
    /* range 80: only the SCR unit has PID 85 (the field report's F485) */
    const ap_veh_ecu_t r4[] = { { 0x18DAF15D, 0x08000000 } };

    n = ap_dialect_table_add(tab, n, 4, 0, r0, 3);
    TEST_ASSERT_EQUAL(3, n);
    n = ap_dialect_table_add(tab, n, 4, 4, r4, 1);
    TEST_ASSERT_EQUAL(3, n);

    /* everybody has 0C: the lowest id, the engine, serves it */
    TEST_ASSERT_TRUE(ap_dialect_table_has(tab, n, 0x0C));
    TEST_ASSERT_EQUAL_HEX32(0x18DAF158, ap_dialect_pid_owner(tab, n, 0x0C));
    /* 11 is the engine's alone, 85 the SCR unit's alone */
    TEST_ASSERT_EQUAL_HEX32(0x18DAF158, ap_dialect_pid_owner(tab, n, 0x11));
    TEST_ASSERT_EQUAL_HEX32(0x18DAF15D, ap_dialect_pid_owner(tab, n, 0x85));
    /* nobody */
    TEST_ASSERT_FALSE(ap_dialect_table_has(tab, n, 0x33));
    TEST_ASSERT_EQUAL_HEX32(UINT32_MAX, ap_dialect_pid_owner(tab, n, 0x33));

    /* the bitmap PIDs are the walk's, never a row: 20 is announced by all
       three (bit 0) and still nobody "has" it */
    TEST_ASSERT_TRUE(ap_dialect_table_more(tab, n, 0));
    TEST_ASSERT_FALSE(ap_dialect_table_has(tab, n, 0x20));
    TEST_ASSERT_FALSE(ap_dialect_table_has(tab, n, 0x00));
    TEST_ASSERT_FALSE(ap_dialect_table_more(tab, n, 4));
    TEST_ASSERT_FALSE(ap_dialect_table_more(tab, n, 8));    /* no such range */

    /* 5A turns up after the engine start with its own (unscaled) 0C: the
       engine still serves it */
    const ap_veh_ecu_t late[] = { { 0x18DAF15A, 0x00100000 } };

    n = ap_dialect_table_add(tab, n, 4, 0, late, 1);
    TEST_ASSERT_EQUAL(4, n);
    TEST_ASSERT_EQUAL_HEX32(0x18DAF158, ap_dialect_pid_owner(tab, n, 0x0C));

    /* the table is full: a fifth responder is not kept, the rest stays */
    const ap_veh_ecu_t fifth[] = { { 0x18DAF110, 0xFFFFFFFF } };

    n = ap_dialect_table_add(tab, n, 4, 0, fifth, 1);
    TEST_ASSERT_EQUAL(4, n);
    TEST_ASSERT_EQUAL_HEX32(0x18DAF158, ap_dialect_pid_owner(tab, n, 0x0C));

    /* a range that does not exist changes nothing */
    TEST_ASSERT_EQUAL(4, ap_dialect_table_add(tab, n, 4, 8, r4, 1));
    TEST_ASSERT_EQUAL(4, ap_dialect_table_add(tab, n, 4, -1, r4, 1));
}

/* ---- addressing -------------------------------------------------------------- */

void test_dialect_addressing(void)
{
    uint32_t req = 0;
    char cmd[AP_DIALECT_HDR_CMD_LEN];

    TEST_ASSERT_TRUE(ap_dialect_request_id(0x7E8, &req));
    TEST_ASSERT_EQUAL_HEX32(0x7E0, req);
    TEST_ASSERT_TRUE(ap_dialect_request_id(0x7EF, &req));
    TEST_ASSERT_EQUAL_HEX32(0x7E7, req);
    TEST_ASSERT_TRUE(ap_dialect_request_id(0x18DAF158, &req));
    TEST_ASSERT_EQUAL_HEX32(0x18DA58F1, req);
    TEST_ASSERT_TRUE(ap_dialect_request_id(0x18DAF100, &req));
    TEST_ASSERT_EQUAL_HEX32(0x18DA00F1, req);

    /* not a legislated responder: the functional ids, a request id, a
       headers-off row */
    TEST_ASSERT_FALSE(ap_dialect_request_id(0x7DF, &req));
    TEST_ASSERT_FALSE(ap_dialect_request_id(0x7E7, &req));
    TEST_ASSERT_FALSE(ap_dialect_request_id(0x7F0, &req));
    TEST_ASSERT_FALSE(ap_dialect_request_id(0x18DB33F1, &req));
    TEST_ASSERT_FALSE(ap_dialect_request_id(0x18DA58F1, &req));
    TEST_ASSERT_FALSE(ap_dialect_request_id(UINT32_MAX, &req));
    TEST_ASSERT_FALSE(ap_dialect_request_id(0x7E8, NULL));

    TEST_ASSERT_EQUAL_size_t(7, ap_dialect_header_cmd(0x7E8, cmd,
                                                     sizeof(cmd)));
    TEST_ASSERT_EQUAL_STRING("ATSH7E0", cmd);
    TEST_ASSERT_EQUAL_size_t(12, ap_dialect_header_cmd(0x18DAF158, cmd,
                                                      sizeof(cmd)));
    TEST_ASSERT_EQUAL_STRING("ATSH18DA58F1", cmd);
    TEST_ASSERT_EQUAL_size_t(12, ap_dialect_header_cmd(0x18DAF100, cmd,
                                                      sizeof(cmd)));
    TEST_ASSERT_EQUAL_STRING("ATSH18DA00F1", cmd);

    TEST_ASSERT_EQUAL_size_t(0, ap_dialect_header_cmd(UINT32_MAX, cmd,
                                                     sizeof(cmd)));
    TEST_ASSERT_EQUAL_STRING("", cmd);
    TEST_ASSERT_EQUAL_size_t(0, ap_dialect_header_cmd(0x18DAF158, cmd, 12));
    TEST_ASSERT_EQUAL_STRING("", cmd);
    TEST_ASSERT_EQUAL_size_t(0, ap_dialect_header_cmd(0x7E8, NULL, 0));

    /* the functional header a row gives back */
    TEST_ASSERT_EQUAL_STRING("ATSH7DF", ap_veh_func_header('6'));
    TEST_ASSERT_EQUAL_STRING("ATSH7DF", ap_veh_func_header('8'));
    TEST_ASSERT_EQUAL_STRING("ATSH18DB33F1", ap_veh_func_header('7'));
    TEST_ASSERT_EQUAL_STRING("ATSH18DB33F1", ap_veh_func_header('9'));
    TEST_ASSERT_NULL(ap_veh_func_header('0'));
    TEST_ASSERT_NULL(ap_veh_func_header('A'));
    TEST_ASSERT_NULL(ap_veh_func_header('\0'));
}

void test_dialect_init_sets_header(void)
{
    TEST_ASSERT_TRUE(ap_dialect_init_sets_header("ATSH18DA58F1"));
    TEST_ASSERT_TRUE(ap_dialect_init_sets_header("atsh7e0"));
    TEST_ASSERT_TRUE(ap_dialect_init_sets_header("AT SH 7E0"));
    TEST_ASSERT_TRUE(ap_dialect_init_sets_header("ATST96;ATSH7E0"));
    TEST_ASSERT_TRUE(ap_dialect_init_sets_header("ATTP7 ; at sh 18DB33F1;"));

    TEST_ASSERT_FALSE(ap_dialect_init_sets_header("ATCRA7E8"));
    TEST_ASSERT_FALSE(ap_dialect_init_sets_header("ATS1;ATH0;ATST96"));
    TEST_ASSERT_FALSE(ap_dialect_init_sets_header("22F190"));
    TEST_ASSERT_FALSE(ap_dialect_init_sets_header("XATSH7E0"));
    TEST_ASSERT_FALSE(ap_dialect_init_sets_header(""));
    TEST_ASSERT_FALSE(ap_dialect_init_sets_header(NULL));
}

void test_dialect_can_candidates(void)
{
    char c[AP_DIALECT_CAND_LEN];

    /* a gatewayed port is silent until asked: all four, 500 first */
    TEST_ASSERT_EQUAL(4, ap_dialect_can_candidates(&SILENT, c));
    TEST_ASSERT_EQUAL_STRING("6789", c);
    TEST_ASSERT_EQUAL(4, ap_dialect_can_candidates(NULL, c));
    TEST_ASSERT_EQUAL_STRING("6789", c);

    /* a bus that named its bitrate: only the protocols at that bitrate */
    TEST_ASSERT_EQUAL(2, ap_dialect_can_candidates(&LIVE_500, c));
    TEST_ASSERT_EQUAL_STRING("67", c);
    TEST_ASSERT_EQUAL(2, ap_dialect_can_candidates(&LIVE_250, c));
    TEST_ASSERT_EQUAL_STRING("89", c);

    /* nothing is transmitted onto a bus the chip cannot match */
    TEST_ASSERT_EQUAL(0, ap_dialect_can_candidates(&LIVE_125, c));
    TEST_ASSERT_EQUAL_STRING("", c);
    TEST_ASSERT_EQUAL(0, ap_dialect_can_candidates(&UNREADABLE, c));
    TEST_ASSERT_EQUAL_STRING("", c);
    TEST_ASSERT_EQUAL(0, ap_dialect_can_candidates(&SILENT, NULL));
}

void run_dialect_tests(void)
{
    RUN_TEST(test_dialect_names);
    RUN_TEST(test_dialect_requests);
    RUN_TEST(test_dialect_expression_shift);
    RUN_TEST(test_dialect_bitmaps_obd2);
    RUN_TEST(test_dialect_bitmaps_uds_captured);
    RUN_TEST(test_dialect_bitmaps_sprinter);
    RUN_TEST(test_dialect_bitmaps_noise_and_bounds);
    RUN_TEST(test_dialect_vin_captured);
    RUN_TEST(test_dialect_protocol_id);
    RUN_TEST(test_dialect_table_owner);
    RUN_TEST(test_dialect_addressing);
    RUN_TEST(test_dialect_init_sets_header);
    RUN_TEST(test_dialect_can_candidates);
}
