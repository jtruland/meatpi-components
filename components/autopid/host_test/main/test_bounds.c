/**
 * @file test_bounds.c
 * @brief A parameter's min / max on a decoded value (ap_bounds_apply,
 *        autopid_bounds.c) and the SAE rows that decode inside their own
 *        range at full scale (ap_std_expression_bounded, the table):
 *        issue #1004.
 */
#include <math.h>
#include <string.h>

#include "unity.h"

#include "autopid_private.h"
#include "expression_parser.h"
#include "obd2_standard_pids.h"

static double apply(double v, float lo, float hi, bool clamp,
                    ap_bounds_verdict_t want)
{
    double x = v;

    TEST_ASSERT_EQUAL(want, ap_bounds_apply(&x, lo, hi, clamp));
    return x;
}

void test_bounds_the_report(void)
{
    /* 0xFF through the alfa-04 row's B2*0.3921568692 against a max of 100:
       100.000001646. A standard row clamps; a custom row with the same
       float noise clamps too */
    double v = 255 * 0.3921568692;

    TEST_ASSERT_TRUE(v > 100.0);
    TEST_ASSERT_EQUAL_DOUBLE(100.0, apply(v, 0, 100, true, AP_BOUNDS_CLAMPED));
    TEST_ASSERT_EQUAL_DOUBLE(100.0, apply(v, 0, 100, false,
                                          AP_BOUNDS_CLAMPED));

    /* the reporter's controlled tests on a raw 0xFD */
    TEST_ASSERT_EQUAL_DOUBLE(100.0, apply(253 * 0.39525692, 0, 100, false,
                                          AP_BOUNDS_CLAMPED)); /* 100.00000076 */
    TEST_ASSERT_EQUAL_DOUBLE(253 * 0.39525691,
                             apply(253 * 0.39525691, 0, 100, false,
                                   AP_BOUNDS_KEPT));         /* 99.99999823 */

    /* 101.2: beyond the band. A custom row drops it (its bounds filter), a
       standard row clamps (the table's range is its display range, as the
       legacy firmware did) */
    apply(253 * 0.4, 0, 100, false, AP_BOUNDS_DROPPED);
    TEST_ASSERT_EQUAL_DOUBLE(100.0, apply(253 * 0.4, 0, 100, true,
                                          AP_BOUNDS_CLAMPED));
}

void test_bounds_rule(void)
{
    /* no bounds: everything kept */
    TEST_ASSERT_EQUAL_DOUBLE(1e9, apply(1e9, NAN, NAN, false, AP_BOUNDS_KEPT));
    TEST_ASSERT_EQUAL_DOUBLE(-5, apply(-5, NAN, 100, false, AP_BOUNDS_KEPT));

    /* on the bound: kept as decoded */
    TEST_ASSERT_EQUAL_DOUBLE(100, apply(100, 0, 100, false, AP_BOUNDS_KEPT));
    TEST_ASSERT_EQUAL_DOUBLE(0, apply(0, 0, 100, false, AP_BOUNDS_KEPT));

    /* the tolerance: 1e-6 of the bound's magnitude, 1e-6 at least */
    TEST_ASSERT_EQUAL_DOUBLE(100, apply(100 + 9e-5, 0, 100, false,
                                        AP_BOUNDS_CLAMPED));
    apply(100 + 2e-4, 0, 100, false, AP_BOUNDS_DROPPED);
    TEST_ASSERT_EQUAL_DOUBLE(0, apply(-5e-7, 0, 100, false,
                                      AP_BOUNDS_CLAMPED));
    apply(-2e-6, 0, 100, false, AP_BOUNDS_DROPPED);
    TEST_ASSERT_EQUAL_DOUBLE(-40, apply(-40 - 1e-5, -40, 215, false,
                                        AP_BOUNDS_CLAMPED));

    /* a J1939 not-available raw (FF.. decoded far above max) drops */
    apply(6553.5, 0, 100, false, AP_BOUNDS_DROPPED);

    /* a standard row clamps both ways */
    TEST_ASSERT_EQUAL_DOUBLE(-40, apply(-100, -40, 215, true,
                                        AP_BOUNDS_CLAMPED));
    TEST_ASSERT_EQUAL_DOUBLE(215, apply(300, -40, 215, true,
                                        AP_BOUNDS_CLAMPED));

    /* NaN never reaches the cache */
    double x = NAN;

    TEST_ASSERT_EQUAL(AP_BOUNDS_DROPPED, ap_bounds_apply(&x, 0, 100, true));
    TEST_ASSERT_EQUAL(AP_BOUNDS_DROPPED, ap_bounds_apply(NULL, 0, 100, true));
}

void test_bounds_exact_full_scale_literal(void)
{
    char buf[AP_EXPR_LEN];
    double fscale = (double)0.3921568627f;  /* the table's percent scale */

    /* the expressions count the frame as the chip prints it with headers
       on, `03 41 2F FF` (2026-10-24, issue #1014). A percent row: the
       ratio, so 0xFF decodes to exactly 100 */
    TEST_ASSERT_EQUAL(ESP_OK, ap_std_expression_bounded(31, 8, fscale, 0, 0,
                                                        100, 0, buf,
                                                        sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("B3*100/255", buf);

    /* AbsLoadValue: 16 bits, max 25700 */
    TEST_ASSERT_EQUAL(ESP_OK, ap_std_expression_bounded(31, 16, fscale, 0, 0,
                                                        25700, 0, buf,
                                                        sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("[B3:B4]*25700/65535", buf);

    /* a uds dialect's shift keeps the form */
    TEST_ASSERT_EQUAL(ESP_OK, ap_std_expression_bounded(31, 8, fscale, 0, 0,
                                                        100, 1, buf,
                                                        sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("B4*100/255", buf);

    /* RPM: 65535 * 0.25 = 16383.75 is not the max 16384: the decimal stays */
    TEST_ASSERT_EQUAL(ESP_OK, ap_std_expression_bounded(31, 16, 0.25, 0, 0,
                                                        16384, 0, buf,
                                                        sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("[B3:B4]*0.25", buf);

    /* an offset row: unchanged */
    TEST_ASSERT_EQUAL(ESP_OK, ap_std_expression_bounded(31, 8, 1.0, -40, -40,
                                                        215, 0, buf,
                                                        sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("B3-40", buf);

    /* a fuel trim: 0.78125 is exact in binary and 99.21875 - (-100) is no
       integer: the decimal */
    TEST_ASSERT_EQUAL(ESP_OK, ap_std_expression_bounded(31, 8, 0.78125, -100,
                                                        -100, 99.21875, 0,
                                                        buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("B3*0.78125-100", buf);

    /* no range: the plain literal, as before */
    TEST_ASSERT_EQUAL(ESP_OK, ap_std_expression_bounded(31, 8, fscale, 0, NAN,
                                                        NAN, 0, buf,
                                                        sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("B3*0.3921568692", buf);

    /* a range below zero with no offset: two's complement (the evaporative
       system vapour pressure) */
    TEST_ASSERT_EQUAL(ESP_OK, ap_std_expression_bounded(31, 16, 0.25, 0,
                                                        -8192, 8192, 0, buf,
                                                        sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("[S3:S4]*0.25", buf);

    /* the ratio decodes exactly through the real parser, on the frame */
    uint8_t ff[8] = { 0x03, 0x41, 0x2F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    double v = 0;

    TEST_ASSERT_EQUAL(ESP_OK, expression_parser_eval("B3*100/255", ff,
                                                     sizeof(ff), 12.0, &v));
    TEST_ASSERT_EQUAL_DOUBLE(100.0, v);
    TEST_ASSERT_EQUAL(ESP_OK, expression_parser_eval("[B3:B4]*25700/65535",
                                                     ff, sizeof(ff), 12.0,
                                                     &v));
    TEST_ASSERT_EQUAL_DOUBLE(25700.0, v);
}

/** Every ranged row of the table, decoded at its extremes through the real
 *  parser, lands inside its own range (within the bounds' tolerance): the
 *  table used to say 99 where a fuel trim reaches 99.22, 1 where a sensor
 *  reaches 1.275, 2 where a wide-range sensor reaches 8 (issue #1004). */
void test_bounds_whole_table_inside_its_range(void)
{
    static const uint8_t PATTERN[4] = { 0xFF, 0x00, 0x7F, 0x80 };
    int checked = 0;

    for (int pid = 0; pid < 256; pid++)
    {
        const std_pid_t *info = get_pid((uint8_t)pid);

        if (info == NULL || info->base_name == NULL)
        {
            continue;
        }

        for (int i = 0; i < info->num_params; i++)
        {
            const std_parameter_t *prm = &info->params[i];
            char expr[AP_EXPR_LEN];

            if (prm->name == NULL || (prm->min == 0.0f && prm->max == 0.0f) ||
                ap_std_expression_bounded(prm->bit_start, prm->bit_length,
                                          (double)prm->scale,
                                          (double)prm->offset,
                                          (double)prm->min,
                                          (double)prm->max, 0, expr,
                                          sizeof(expr)) != ESP_OK)
            {
                continue;
            }

            /* no fmax(): the IDF linux target links no libm (2026-10-21) */
            double lo_tol = fabs(prm->min) * AP_BOUNDS_TOL_REL;
            double hi_tol = fabs(prm->max) * AP_BOUNDS_TOL_REL;

            if (lo_tol < AP_BOUNDS_TOL_ABS)
            {
                lo_tol = AP_BOUNDS_TOL_ABS;
            }

            if (hi_tol < AP_BOUNDS_TOL_ABS)
            {
                hi_tol = AP_BOUNDS_TOL_ABS;
            }

            for (size_t p = 0; p < sizeof(PATTERN); p++)
            {
                uint8_t payload[12];
                double v = 0;
                char msg[160];

                memset(payload, PATTERN[p], sizeof(payload));
                TEST_ASSERT_EQUAL_MESSAGE(
                    ESP_OK, expression_parser_eval(expr, payload,
                                                   sizeof(payload), 12.0,
                                                   &v), expr);
                snprintf(msg, sizeof(msg), "%s: %s on %02X = %f not in "
                         "%g..%g", prm->name, expr, PATTERN[p], v,
                         (double)prm->min, (double)prm->max);
                TEST_ASSERT_TRUE_MESSAGE(v >= (double)prm->min - lo_tol &&
                                         v <= (double)prm->max + hi_tol, msg);
            }

            checked++;
        }
    }

    TEST_ASSERT_TRUE(checked > 100); /* 135 ranged rows on 2026-10-20 */
}

void run_bounds_tests(void)
{
    RUN_TEST(test_bounds_the_report);
    RUN_TEST(test_bounds_rule);
    RUN_TEST(test_bounds_exact_full_scale_literal);
    RUN_TEST(test_bounds_whole_table_inside_its_range);
}
