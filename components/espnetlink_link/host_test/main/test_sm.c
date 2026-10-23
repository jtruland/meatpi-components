/**
 * @file test_sm.c
 * @brief The zero-touch pairing state machine (espnetlink_link_core.c)
 *        driven exactly the way the engine drives it: one event in, one
 *        action out, outcomes fed back. Covers the contract's §3 paths:
 *        happy cut, unchanged vs changed key (reboot), 409 tether mode,
 *        foreign device, drop timeout -> VBUS recovery, recovery budget,
 *        re-enumeration after a cut, usb_ncm mode, AP-stale recovery,
 *        operator re-pair.
 */
#include "unity.h"

#include "espnetlink_link_core.h"

static espnl_sm_t sm;
static uint32_t now;

static espnl_sm_action_t step(espnl_sm_event_t ev)
{
    return espnl_sm_step(&sm, ev, now);
}

static espnl_sm_action_t tick(uint32_t ms)
{
    now += ms;
    return step(ESPNL_EV_TICK);
}

static void setup_wifi(void)
{
    now = 100000;
    espnl_sm_init(&sm, false, 2, now);
}

/* attach -> identify ok -> key ok -> cut posted (WAIT_DROP) */
static void drive_to_wait_drop(void)
{
    TEST_ASSERT_EQUAL(ESPNL_ACT_GET_INFO, step(ESPNL_EV_LINK_UP));
    TEST_ASSERT_EQUAL(ESPNL_SM_IDENTIFY, sm.state);
    TEST_ASSERT_EQUAL(ESPNL_ACT_GET_KEY, step(ESPNL_EV_OK));
    TEST_ASSERT_EQUAL(ESPNL_SM_READ_KEY, sm.state);
    TEST_ASSERT_EQUAL(ESPNL_ACT_POST_CUT, step(ESPNL_EV_OK));
    TEST_ASSERT_EQUAL(ESPNL_SM_CUT, sm.state);
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, step(ESPNL_EV_OK));
    TEST_ASSERT_EQUAL(ESPNL_SM_WAIT_DROP, sm.state);
}

static void test_sm_happy_unchanged_key(void)
{
    setup_wifi();
    TEST_ASSERT_EQUAL(ESPNL_SM_IDLE, sm.state);
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, tick(1000));

    drive_to_wait_drop();
    /* the device drops within 3 s: done, no reboot (key unchanged) */
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, tick(1000));
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, step(ESPNL_EV_LINK_DOWN));
    TEST_ASSERT_EQUAL(ESPNL_SM_DONE, sm.state);
    TEST_ASSERT_EQUAL_STRING("done", espnl_sm_state_str(sm.state));
    /* steady: ticks do nothing, the dongle stays cut */
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, tick(60000));
    TEST_ASSERT_EQUAL(ESPNL_SM_DONE, sm.state);
}

static void test_sm_changed_key_reboots_after_drop(void)
{
    setup_wifi();
    TEST_ASSERT_EQUAL(ESPNL_ACT_GET_INFO, step(ESPNL_EV_LINK_UP));
    TEST_ASSERT_EQUAL(ESPNL_ACT_GET_KEY, step(ESPNL_EV_OK));
    sm.reboot_pending = true; /* the engine stored a new key */
    TEST_ASSERT_EQUAL(ESPNL_ACT_POST_CUT, step(ESPNL_EV_OK));
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, step(ESPNL_EV_OK));
    /* reboot only once the cut has landed (the dongle must not see a
     * half-finished sequence) */
    TEST_ASSERT_EQUAL(ESPNL_ACT_REBOOT, step(ESPNL_EV_LINK_DOWN));
    TEST_ASSERT_EQUAL(ESPNL_SM_DONE, sm.state);
}

static void test_sm_drop_during_cut_post_counts(void)
{
    setup_wifi();
    TEST_ASSERT_EQUAL(ESPNL_ACT_GET_INFO, step(ESPNL_EV_LINK_UP));
    TEST_ASSERT_EQUAL(ESPNL_ACT_GET_KEY, step(ESPNL_EV_OK));
    TEST_ASSERT_EQUAL(ESPNL_ACT_POST_CUT, step(ESPNL_EV_OK));
    /* the drop can race the POST's response: still a cut */
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, step(ESPNL_EV_LINK_DOWN));
    TEST_ASSERT_EQUAL(ESPNL_SM_DONE, sm.state);
}

static void test_sm_tether_mode_409(void)
{
    setup_wifi();
    TEST_ASSERT_EQUAL(ESPNL_ACT_GET_INFO, step(ESPNL_EV_LINK_UP));
    TEST_ASSERT_EQUAL(ESPNL_ACT_GET_KEY, step(ESPNL_EV_OK));
    TEST_ASSERT_EQUAL(ESPNL_ACT_POST_CUT, step(ESPNL_EV_OK));
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, step(ESPNL_EV_CONFLICT));
    TEST_ASSERT_EQUAL(ESPNL_SM_TETHER, sm.state);
    /* leave the link alone: no retries, no cycles */
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, tick(30000));
    TEST_ASSERT_EQUAL(ESPNL_SM_TETHER, sm.state);
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, step(ESPNL_EV_LINK_DOWN));
    TEST_ASSERT_EQUAL(ESPNL_SM_IDLE, sm.state);
}

static void test_sm_foreign_device(void)
{
    setup_wifi();
    /* a USB-Ethernet adapter: never probed */
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, step(ESPNL_EV_LINK_UP_OTHER));
    TEST_ASSERT_EQUAL(ESPNL_SM_FOREIGN, sm.state);
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, tick(1000));
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, step(ESPNL_EV_LINK_DOWN));
    TEST_ASSERT_EQUAL(ESPNL_SM_IDLE, sm.state);

    /* right VID/PID but /api/info says something else */
    TEST_ASSERT_EQUAL(ESPNL_ACT_GET_INFO, step(ESPNL_EV_LINK_UP));
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, step(ESPNL_EV_NOT_ESPNETLINK));
    TEST_ASSERT_EQUAL(ESPNL_SM_FOREIGN, sm.state);
}

/* identify: retried every tick inside the budget, then the device is
 * `unreachable` (it IS an ESPNetLink by its USB identity), never
 * `foreign` (2026-10-23) */
static void test_sm_identify_retries_then_unreachable(void)
{
    setup_wifi();
    TEST_ASSERT_EQUAL(ESPNL_ACT_GET_INFO, step(ESPNL_EV_LINK_UP));
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, step(ESPNL_EV_FAIL));
    TEST_ASSERT_EQUAL(ESPNL_ACT_GET_INFO, tick(1000));
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, step(ESPNL_EV_FAIL));
    TEST_ASSERT_EQUAL(ESPNL_ACT_GET_INFO, tick(1000));
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, step(ESPNL_EV_FAIL));
    TEST_ASSERT_EQUAL(ESPNL_SM_IDENTIFY, sm.state);
    /* still inside the 15 s budget at 12 s */
    TEST_ASSERT_EQUAL(ESPNL_ACT_GET_INFO, tick(10000));
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, step(ESPNL_EV_FAIL));
    TEST_ASSERT_EQUAL(ESPNL_SM_IDENTIFY, sm.state);
    /* the budget gone: unreachable, not foreign, nothing fired yet */
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, tick(4000));
    TEST_ASSERT_EQUAL(ESPNL_SM_UNREACHABLE, sm.state);
    TEST_ASSERT_EQUAL_STRING("unreachable", espnl_sm_state_str(sm.state));
    TEST_ASSERT_EQUAL(0, sm.retries);

    /* a GET every 10 s, each failure leaves the state alone */
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, tick(5000));
    TEST_ASSERT_EQUAL(ESPNL_ACT_GET_INFO, tick(5000));
    TEST_ASSERT_EQUAL(1, sm.retries);
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, step(ESPNL_EV_FAIL));
    TEST_ASSERT_EQUAL(ESPNL_SM_UNREACHABLE, sm.state);
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, tick(9000));
    TEST_ASSERT_EQUAL(ESPNL_ACT_GET_INFO, tick(1000));
    TEST_ASSERT_EQUAL(2, sm.retries);

    /* it answers after all: the pairing goes on from identify */
    TEST_ASSERT_EQUAL(ESPNL_ACT_GET_KEY, step(ESPNL_EV_OK));
    TEST_ASSERT_EQUAL(ESPNL_SM_READ_KEY, sm.state);

    /* and a USB-Ethernet adapter still goes straight to foreign */
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, step(ESPNL_EV_LINK_DOWN));
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, step(ESPNL_EV_LINK_UP_OTHER));
    TEST_ASSERT_EQUAL(ESPNL_SM_FOREIGN, sm.state);
}

/* unreachable for two minutes: one VBUS cycle (budgeted like every other
 * recovery), the cadence slows to a GET every 60 s once the budget is
 * spent, a link drop clears it, the operator's re-pair cycles at once */
static void test_sm_unreachable_cycles_then_slows(void)
{
    setup_wifi();
    TEST_ASSERT_EQUAL(ESPNL_ACT_GET_INFO, step(ESPNL_EV_LINK_UP));
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, step(ESPNL_EV_FAIL));
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, tick(16000));
    TEST_ASSERT_EQUAL(ESPNL_SM_UNREACHABLE, sm.state);

    /* 2 min of failed GETs: the cycle fires on the next tick after 120 s */
    for (int i = 0; i < 11; i++)
    {
        TEST_ASSERT_EQUAL(ESPNL_ACT_GET_INFO, tick(10000));
        TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, step(ESPNL_EV_FAIL));
    }
    TEST_ASSERT_EQUAL(11, sm.retries);
    TEST_ASSERT_EQUAL(ESPNL_ACT_VBUS_CYCLE, tick(10000));
    TEST_ASSERT_EQUAL(ESPNL_SM_IDLE, sm.state);
    TEST_ASSERT_EQUAL(1, sm.cycles);

    /* the power cycle re-enumerates it: identify again; still silent */
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, step(ESPNL_EV_LINK_DOWN));
    TEST_ASSERT_EQUAL(ESPNL_ACT_GET_INFO, step(ESPNL_EV_LINK_UP));
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, step(ESPNL_EV_FAIL));
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, tick(16000));
    TEST_ASSERT_EQUAL(ESPNL_SM_UNREACHABLE, sm.state);
    TEST_ASSERT_EQUAL(0, sm.retries);

    /* two more cycles spend the budget; after that no VBUS cycle, a GET
     * every 60 s instead, forever */
    for (int c = 0; c < 2; c++)
    {
        for (int i = 0; i < 11; i++)
        {
            TEST_ASSERT_EQUAL(ESPNL_ACT_GET_INFO, tick(10000));
            TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, step(ESPNL_EV_FAIL));
        }
        TEST_ASSERT_EQUAL(ESPNL_ACT_VBUS_CYCLE, tick(10000));
        TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, step(ESPNL_EV_LINK_DOWN));
        TEST_ASSERT_EQUAL(ESPNL_ACT_GET_INFO, step(ESPNL_EV_LINK_UP));
        TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, step(ESPNL_EV_FAIL));
        TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, tick(16000));
        TEST_ASSERT_EQUAL(ESPNL_SM_UNREACHABLE, sm.state);
    }
    TEST_ASSERT_EQUAL(3, sm.cycles);
    for (int i = 0; i < 12; i++)
    {
        espnl_sm_action_t a = tick(10000);

        TEST_ASSERT_TRUE(a == ESPNL_ACT_NONE || a == ESPNL_ACT_GET_INFO);
        TEST_ASSERT_NOT_EQUAL(ESPNL_ACT_VBUS_CYCLE, a);
        TEST_ASSERT_NOT_EQUAL(ESPNL_ACT_GIVE_UP, a);
        if (a == ESPNL_ACT_GET_INFO)
        {
            TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, step(ESPNL_EV_FAIL));
        }
    }
    TEST_ASSERT_EQUAL(ESPNL_SM_UNREACHABLE, sm.state);
    TEST_ASSERT_EQUAL(2, sm.retries);   /* 120 s at one GET per 60 s */

    /* the operator's re-pair spends a cycle regardless of the budget */
    TEST_ASSERT_EQUAL(ESPNL_ACT_VBUS_CYCLE, step(ESPNL_EV_REPAIR));
    TEST_ASSERT_EQUAL(ESPNL_SM_IDLE, sm.state);

    /* a drop while unreachable: idle, the next link-up starts afresh */
    TEST_ASSERT_EQUAL(ESPNL_ACT_GET_INFO, step(ESPNL_EV_LINK_UP));
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, tick(16000));
    TEST_ASSERT_EQUAL(ESPNL_SM_UNREACHABLE, sm.state);
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, step(ESPNL_EV_LINK_DOWN));
    TEST_ASSERT_EQUAL(ESPNL_SM_IDLE, sm.state);
}

static void test_sm_cut_retry_then_recover(void)
{
    setup_wifi(); /* cut_retries = 2 */
    TEST_ASSERT_EQUAL(ESPNL_ACT_GET_INFO, step(ESPNL_EV_LINK_UP));
    TEST_ASSERT_EQUAL(ESPNL_ACT_GET_KEY, step(ESPNL_EV_OK));
    TEST_ASSERT_EQUAL(ESPNL_ACT_POST_CUT, step(ESPNL_EV_OK));
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, step(ESPNL_EV_FAIL));
    TEST_ASSERT_EQUAL(ESPNL_ACT_POST_CUT, tick(1000));  /* retry #2 */
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, step(ESPNL_EV_FAIL));
    /* retries exhausted: power-cycle the dongle and start over */
    TEST_ASSERT_EQUAL(ESPNL_ACT_VBUS_CYCLE, tick(1000));
    TEST_ASSERT_EQUAL(ESPNL_SM_IDLE, sm.state);
    TEST_ASSERT_EQUAL_INT(1, sm.cycles);
}

static void test_sm_drop_timeout_repost_then_recover(void)
{
    setup_wifi();
    drive_to_wait_drop();
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, tick(1000));
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, tick(1000));
    /* 3 s and still enumerated: one more POST */
    TEST_ASSERT_EQUAL(ESPNL_ACT_POST_CUT, tick(1000));
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, step(ESPNL_EV_OK));
    TEST_ASSERT_EQUAL(ESPNL_SM_WAIT_DROP, sm.state);
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, tick(1000));   /* 4 s: no 3rd POST */
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, tick(5000));   /* 9 s */
    /* 10 s: the cut never landed, VBUS cycle */
    TEST_ASSERT_EQUAL(ESPNL_ACT_VBUS_CYCLE, tick(1000));
    TEST_ASSERT_EQUAL(ESPNL_SM_IDLE, sm.state);

    /* the cycled dongle comes back and this time drops: budget resets */
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, step(ESPNL_EV_LINK_DOWN));
    now += 5000;
    drive_to_wait_drop();
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, step(ESPNL_EV_LINK_DOWN));
    TEST_ASSERT_EQUAL(ESPNL_SM_DONE, sm.state);
    TEST_ASSERT_EQUAL_INT(0, sm.cycles);
}

static void test_sm_recovery_budget(void)
{
    setup_wifi();

    for (int i = 0; i < ESPNL_SM_MAX_CYCLES; i++)
    {
        now += ESPNL_SM_CYCLE_GAP_MS;
        TEST_ASSERT_EQUAL(ESPNL_ACT_GET_INFO, step(ESPNL_EV_LINK_UP));
        TEST_ASSERT_EQUAL(ESPNL_ACT_GET_KEY, step(ESPNL_EV_OK));
        /* the key never comes: 5 s budget then a cycle */
        TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, step(ESPNL_EV_FAIL));
        TEST_ASSERT_EQUAL(ESPNL_ACT_VBUS_CYCLE, tick(5000));
        TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, step(ESPNL_EV_LINK_DOWN));
    }
    TEST_ASSERT_EQUAL_INT(ESPNL_SM_MAX_CYCLES, sm.cycles);

    /* the 4th failure run gives up instead of cycling forever */
    now += ESPNL_SM_CYCLE_GAP_MS;
    TEST_ASSERT_EQUAL(ESPNL_ACT_GET_INFO, step(ESPNL_EV_LINK_UP));
    TEST_ASSERT_EQUAL(ESPNL_ACT_GET_KEY, step(ESPNL_EV_OK));
    TEST_ASSERT_EQUAL(ESPNL_ACT_GIVE_UP, tick(5000));
    TEST_ASSERT_EQUAL(ESPNL_SM_FOREIGN, sm.state);

    /* two cycles closer than the gap: the second one is refused */
    setup_wifi();
    TEST_ASSERT_EQUAL(ESPNL_ACT_GET_INFO, step(ESPNL_EV_LINK_UP));
    TEST_ASSERT_EQUAL(ESPNL_ACT_GET_KEY, step(ESPNL_EV_OK));
    TEST_ASSERT_EQUAL(ESPNL_ACT_VBUS_CYCLE, tick(5000));
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, step(ESPNL_EV_LINK_DOWN));
    TEST_ASSERT_EQUAL(ESPNL_ACT_GET_INFO, step(ESPNL_EV_LINK_UP));
    TEST_ASSERT_EQUAL(ESPNL_ACT_GET_KEY, step(ESPNL_EV_OK));
    TEST_ASSERT_EQUAL(ESPNL_ACT_GIVE_UP, tick(5000));
}

static void test_sm_reenumeration_rereads_key(void)
{
    setup_wifi();
    drive_to_wait_drop();
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, step(ESPNL_EV_LINK_DOWN));
    TEST_ASSERT_EQUAL(ESPNL_SM_DONE, sm.state);

    /* the dongle power-cycled by itself (car ignition): the link comes
     * back and the whole sequence runs again, cheap and self-healing */
    now += 3600000;
    TEST_ASSERT_EQUAL(ESPNL_ACT_GET_INFO, step(ESPNL_EV_LINK_UP));
    TEST_ASSERT_EQUAL(ESPNL_SM_IDENTIFY, sm.state);
    TEST_ASSERT_FALSE(sm.reboot_pending);
}

static void test_sm_ap_stale_and_repair(void)
{
    setup_wifi();
    drive_to_wait_drop();
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, step(ESPNL_EV_LINK_DOWN));

    /* the engine decided the stored key is stale -> cycle */
    now += 180000;
    TEST_ASSERT_EQUAL(ESPNL_ACT_VBUS_CYCLE, step(ESPNL_EV_AP_STALE));
    TEST_ASSERT_EQUAL(ESPNL_SM_IDLE, sm.state);

    /* operator re-pair is never budget-refused */
    sm.cycles = ESPNL_SM_MAX_CYCLES;
    TEST_ASSERT_EQUAL(ESPNL_ACT_VBUS_CYCLE, step(ESPNL_EV_REPAIR));
    TEST_ASSERT_EQUAL_INT(1, sm.cycles);

    /* AP_STALE outside DONE is ignored */
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, step(ESPNL_EV_AP_STALE));
}

static void test_sm_ncm_mode(void)
{
    now = 5000;
    espnl_sm_init(&sm, true, 2, now);

    TEST_ASSERT_EQUAL(ESPNL_ACT_GET_INFO, step(ESPNL_EV_LINK_UP));
    /* no key, no cut: make sure the dongle shares its link over USB */
    TEST_ASSERT_EQUAL(ESPNL_ACT_ENSURE_SHARE, step(ESPNL_EV_OK));
    TEST_ASSERT_EQUAL(ESPNL_SM_NCM_SHARE, sm.state);
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, step(ESPNL_EV_OK));
    TEST_ASSERT_EQUAL(ESPNL_SM_NCM_UP, sm.state);
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, tick(60000));
    TEST_ASSERT_EQUAL(ESPNL_SM_NCM_UP, sm.state);

    /* the dongle rebooted to apply ncm_share: link down, then up again */
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, step(ESPNL_EV_LINK_DOWN));
    TEST_ASSERT_EQUAL(ESPNL_SM_IDLE, sm.state);
    TEST_ASSERT_EQUAL(ESPNL_ACT_GET_INFO, step(ESPNL_EV_LINK_UP));
    TEST_ASSERT_EQUAL(ESPNL_ACT_ENSURE_SHARE, step(ESPNL_EV_OK));

    /* settings unreachable: after 10 s run with what the dongle offers */
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, step(ESPNL_EV_FAIL));
    TEST_ASSERT_EQUAL(ESPNL_ACT_ENSURE_SHARE, tick(1000));
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, step(ESPNL_EV_FAIL));
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, tick(10000));
    TEST_ASSERT_EQUAL(ESPNL_SM_NCM_UP, sm.state);
}

/* The key was read but the WiCAN cannot store it now (factory AP
 * password, 2026-09-08 field-hit): park, never retry or power-cycle; the
 * link edges and the operator's re-pair are the only ways out. */
static void test_sm_hold_parks_without_churn(void)
{
    setup_wifi();
    TEST_ASSERT_EQUAL(ESPNL_ACT_GET_INFO, step(ESPNL_EV_LINK_UP));
    TEST_ASSERT_EQUAL(ESPNL_ACT_GET_KEY, step(ESPNL_EV_OK));
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, step(ESPNL_EV_HOLD));
    TEST_ASSERT_EQUAL(ESPNL_SM_HOLD, sm.state);
    TEST_ASSERT_EQUAL_STRING("hold", espnl_sm_state_str(sm.state));
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, tick(5000));
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, tick(60000));
    TEST_ASSERT_EQUAL(ESPNL_SM_HOLD, sm.state);
    TEST_ASSERT_EQUAL_INT(0, sm.cycles);
    /* the operator's re-pair still works (one cycle, re-read the key) */
    TEST_ASSERT_EQUAL(ESPNL_ACT_VBUS_CYCLE, step(ESPNL_EV_REPAIR));
    TEST_ASSERT_EQUAL(ESPNL_SM_IDLE, sm.state);
    /* and a re-plug simply tries again */
    TEST_ASSERT_EQUAL(ESPNL_ACT_GET_INFO, step(ESPNL_EV_LINK_UP));
    TEST_ASSERT_EQUAL(ESPNL_ACT_GET_KEY, step(ESPNL_EV_OK));
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, step(ESPNL_EV_HOLD));
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, step(ESPNL_EV_LINK_DOWN));
    TEST_ASSERT_EQUAL(ESPNL_SM_IDLE, sm.state);
}

/* A dongle whose firmware predates the WiFi-modem API (bench 2026-09-08:
 * a July test build answered /api/info but 404'd the credentials): say
 * so and wait for a firmware update, never "foreign", never a cycle. */
static void test_sm_unsupported_dongle_firmware(void)
{
    setup_wifi();
    TEST_ASSERT_EQUAL(ESPNL_ACT_GET_INFO, step(ESPNL_EV_LINK_UP));
    TEST_ASSERT_EQUAL(ESPNL_ACT_GET_KEY, step(ESPNL_EV_OK));
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, step(ESPNL_EV_UNSUPPORTED));
    TEST_ASSERT_EQUAL(ESPNL_SM_UNSUPPORTED, sm.state);
    TEST_ASSERT_EQUAL_STRING("unsupported", espnl_sm_state_str(sm.state));
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, tick(60000));
    TEST_ASSERT_EQUAL(ESPNL_SM_UNSUPPORTED, sm.state);
    TEST_ASSERT_EQUAL_INT(0, sm.cycles);
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, step(ESPNL_EV_LINK_DOWN));
    TEST_ASSERT_EQUAL(ESPNL_SM_IDLE, sm.state);

    /* the verdict can also come from /api/info itself (api_level) */
    TEST_ASSERT_EQUAL(ESPNL_ACT_GET_INFO, step(ESPNL_EV_LINK_UP));
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, step(ESPNL_EV_UNSUPPORTED));
    TEST_ASSERT_EQUAL(ESPNL_SM_UNSUPPORTED, sm.state);

    /* usb_ncm mode: no settings API = run with what the dongle offers */
    now = 5000;
    espnl_sm_init(&sm, true, 2, now);
    TEST_ASSERT_EQUAL(ESPNL_ACT_GET_INFO, step(ESPNL_EV_LINK_UP));
    TEST_ASSERT_EQUAL(ESPNL_ACT_ENSURE_SHARE, step(ESPNL_EV_OK));
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, step(ESPNL_EV_UNSUPPORTED));
    TEST_ASSERT_EQUAL(ESPNL_SM_NCM_UP, sm.state);
}

static void test_sm_init_clamps(void)
{
    espnl_sm_init(&sm, false, 0, 0);
    TEST_ASSERT_EQUAL_INT(1, sm.cut_retries);
    espnl_sm_init(&sm, false, 99, 0);
    TEST_ASSERT_EQUAL_INT(5, sm.cut_retries);
    TEST_ASSERT_EQUAL(ESPNL_ACT_NONE, espnl_sm_step(NULL, ESPNL_EV_TICK, 0));
    TEST_ASSERT_EQUAL_STRING("?", espnl_sm_state_str(ESPNL_SM_COUNT));
}

void run_sm_tests(void)
{
    RUN_TEST(test_sm_happy_unchanged_key);
    RUN_TEST(test_sm_changed_key_reboots_after_drop);
    RUN_TEST(test_sm_drop_during_cut_post_counts);
    RUN_TEST(test_sm_tether_mode_409);
    RUN_TEST(test_sm_foreign_device);
    RUN_TEST(test_sm_identify_retries_then_unreachable);
    RUN_TEST(test_sm_unreachable_cycles_then_slows);
    RUN_TEST(test_sm_cut_retry_then_recover);
    RUN_TEST(test_sm_drop_timeout_repost_then_recover);
    RUN_TEST(test_sm_recovery_budget);
    RUN_TEST(test_sm_reenumeration_rereads_key);
    RUN_TEST(test_sm_ap_stale_and_repair);
    RUN_TEST(test_sm_ncm_mode);
    RUN_TEST(test_sm_hold_parks_without_churn);
    RUN_TEST(test_sm_unsupported_dongle_firmware);
    RUN_TEST(test_sm_init_clamps);
}
