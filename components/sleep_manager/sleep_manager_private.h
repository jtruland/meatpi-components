/*
 * This file is part of the MeatPi components project.
 *
 * Copyright (C) 2022-2026 MeatPi Electronics.
 * Written by Ali Slim <ali@meatpi.com>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file sleep_manager_private.h
 * @brief Internals: the PURE policy ladder (host-tested) + glue hooks.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "sleep_manager.h"

/* legacy-carried constants */
#define SM_WAKE_DELTA_V     0.1f     /* minimum wake_v - sleep_v band  */
#define SM_CRITICAL_V       11.90f   /* below: no periodic wakeups, and the floor */
#define SM_CRITICAL_HYST_V  0.05f    /* the floor's timer resets above V + this */
/* the floor: this long under V = sleep. 5 min (Ali, 2026-10-06): the 2 min of
 * 2026-10-01 put a default device to sleep in the middle of a Quick Setup on
 * an 11.6 V supply, ahead of its own 5 min sleep delay */
#define SM_CRITICAL_DELAY_MS 300000u
#define SM_ERROR_V          12.10f   /* boot-loop guard voltage gate   */
#define SM_WAKE_DELAY_MS    500u     /* default wake_delay_ms (v4)     */
#define SM_BOOT_GRACE_MS    15000u   /* meatpi: flash window on loops  */
#define SM_NAP_US           (2u * 1000u * 1000u)
#define SM_RESLEEP_MAX      6        /* OBD re-sleeps before recovery  */
#define SM_AUTOPID_IDLE_MS  20000u

/* ---- sleep_manager_policy.c: PURE (host-tested) -------------------------- */

typedef enum
{
    SM_ACT_NONE = 0,
    SM_ACT_ENTER_SLEEP,     /* run the shutdown sequence, start napping */
    SM_ACT_WAKE_REBOOT,     /* voltage recovered: POWER_WAKE reboot     */
    SM_ACT_PERIODIC_WAKE,   /* periodic check-in: POWER_WAKE reboot     */
} sm_action_t;

typedef struct
{
    float    sleep_v;
    float    wake_v;
    uint32_t wake_hold_ms;   /* recovery must hold this long (v4 setting) */
    uint32_t delay_ms;       /* LOW_VOLTAGE countdown                  */
    bool     periodic;
    uint32_t interval_ms;    /* periodic check-in while SLEEPING       */
    bool     motion_wake;    /* bitz: IMU motion wakes / holds awake */
    float    motion_g;       /* per-axis delta between naps that counts */
} sm_cfg_t;

typedef struct
{
    sleep_manager_state_t state;
    uint32_t              t_low;      /* LOW_VOLTAGE deadline          */
    uint32_t              t_stable;   /* WAKE_PENDING deadline         */
    uint32_t              t_periodic; /* next periodic check-in        */
} sm_policy_t;

void sm_policy_init(sm_policy_t *p);

/* MOTION (bitz fork, v4 commit d01d388 ported to v6). A regulated-voltage
 * alternator (GM RVC on the Sierra) parks the bus inside the sleep/wake
 * band while driving, so voltage alone sleeps the device mid-trip and never
 * wakes it. Motion has no such ambiguity. Pure parts, host-tested: */

/** True when any axis moved more than @p limit_g between two readings. */
bool sm_motion_moved(const float prev[3], const float cur[3], float limit_g);

/** SLEEPING: wake on motion, gated on the critical floor exactly like the
 *  periodic check-in so a flat battery is never woken by a bump. */
bool sm_motion_wake_due(const sm_policy_t *p, const sm_cfg_t *cfg,
                        float volts, bool moved);

/** LOW_VOLTAGE while the IMU says ACTIVE: restart the sleep delay, so a
 *  moving vehicle never enters sleep. Returns true when it pushed. */
bool sm_motion_hold(sm_policy_t *p, const sm_cfg_t *cfg, bool active,
                    uint32_t now_ms);

/* The CRITICAL FLOOR (Ali, 2026-10-01): a battery that reads under
 * SM_CRITICAL_V for SM_CRITICAL_DELAY_MS puts the device to sleep
 * regardless of the `enabled` setting and without waiting out the
 * user's sleep delay. Pure tracker: arms on the first reading under the
 * floor, holds through the small hysteresis band, resets at or above
 * SM_CRITICAL_V + SM_CRITICAL_HYST_V, returns true ONCE when the delay
 * has elapsed (and re-arms on the next reading under the floor). */
typedef struct
{
    bool     armed;
    uint32_t t_trip;
} sm_critical_t;

void sm_critical_init(sm_critical_t *c);
bool sm_critical_eval(sm_critical_t *c, float volts, uint32_t now_ms);

/* The COUNTDOWN (2026-10-06): which rule puts an awake device to sleep
 * first, and its deadline (ms, the clock of @p now_ms; untouched with
 * NONE). The ladder counts only while it runs (@p ladder_on: sleep
 * enabled) and sits in LOW_VOLTAGE; the floor counts whenever it is
 * armed, and overtakes a longer sleep delay (when the floor was 2 min, a
 * default device at 11.6 V slept after 2 min and not after its 5 min
 * delay: the report that asked for this). The floor wins a tie, which a
 * default device under the floor is since the floor is 5 min too: the
 * task evaluates it first. NONE once asleep. */
sleep_manager_pending_t sm_pending_eval(const sm_policy_t *p, bool ladder_on,
                                        const sm_critical_t *c,
                                        uint32_t now_ms,
                                        uint32_t *deadline_ms);

/* The HOLD (Ali, 2026-10-06: "prompt the user to extend if he needs more
 * time"): a person at the web UI asks for more time before the entry, and
 * both deadlines, the ladder's and the floor's, move out to @p now_ms +
 * @p hold_ms where that is later than they are (a hold shorter than what is
 * left changes nothing). Only what is counting moves: the ladder when it
 * runs (@p ladder_on) and sits in LOW_VOLTAGE, the floor when it is armed.
 * Returns false when nothing counts (nothing to hold) or the device is
 * already asleep. States are untouched: a battery that recovers above the
 * wake voltage still ends the countdown. Not a setting: never saved, and a
 * restart drops it; the caller counts the presses (SM_HOLD_MAX per boot). */
#define SM_HOLD_MAX        3u
#define SM_HOLD_MIN_MIN    1u
#define SM_HOLD_MAX_MIN    30u
bool sm_hold_apply(sm_policy_t *p, bool ladder_on, sm_critical_t *c,
                   uint32_t now_ms, uint32_t hold_ms);

/* The countdown as ONE word, for a reader on another task: the cause in
 * the top two bits, the low 30 bits of the deadline below; and back, as
 * the seconds left at @p now_ms (rounded up, 0 when due and with NONE).
 * A deadline keeps the answer exact at the moment of the read, however
 * long ago it was published. 30 bits of ms are 12 days and a countdown is
 * 30 min at most: a deadline more than half that range ahead is one that
 * has passed. */
#define SM_PENDING_MS_MASK 0x3FFFFFFFu
uint32_t sm_pending_pack(sleep_manager_pending_t cause, uint32_t deadline_ms);
sleep_manager_pending_t sm_pending_unpack(uint32_t word, uint32_t now_ms,
                                          uint32_t *sleep_in_s);

/* Settings v3 (2026-10-01): the wake voltage is a setting of its own.
 * Resolve the stored millivolt pair into the policy thresholds; a wake
 * voltage not at least SM_WAKE_DELTA_V above sleep is pulled up to that
 * band. Returns true when it had to clamp (on_apply logs one W). */
bool sm_resolve_thresholds(int sleep_mv, int wake_mv,
                           float *sleep_v, float *wake_v);

/* v2 -> v3 migration rule: the wake_mv a document without one gets
 * (the old derived band, sleep + 100 mV) clamped into [lo, hi]. */
int sm_migrated_wake_mv(int sleep_mv, int lo, int hi);

/* v3 -> v4 migration rule (2026-10-01): the wake_delay_ms a document
 * without one gets (the old fixed window was 1 s; the new default is
 * SM_WAKE_DELAY_MS). Returns 0 when nothing has to be added. */
int sm_migrated_wake_delay_ms(uint32_t from_version, bool present);

/* one evaluation step; timestamps are ms and may wrap (deadline math
 * is subtraction-based) */
sm_action_t sm_policy_eval(sm_policy_t *p, const sm_cfg_t *cfg,
                           float volts, uint32_t now_ms);

/* ---- settings (sleep_manager_settings.c) -----------------------------------
 * `sleep_` prefix, not `sm_`: socket_manager already links sm_settings_*. */

/** Register the "sleep_manager" descriptor with settings_manager. */
esp_err_t sleep_settings_register(void);

/** Boot-applied config; valid once sleep_settings_is_configured(). */
const sm_cfg_t *sleep_settings_config(void);
bool sleep_settings_enabled(void);
bool sleep_settings_is_configured(void); /* boot apply ran (standard §4.3) */

/* ---- glue ----------------------------------------------------------------- */

void sm_events_register(void);
void sm_events_entering(float volts);           /* BEFORE teardown */
void sm_events_state(const char *state, float volts);
