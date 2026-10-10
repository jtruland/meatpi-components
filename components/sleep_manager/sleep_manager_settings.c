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
 * @file sleep_manager_settings.c
 * @brief settings_manager descriptor for sleep_manager: field-table
 *        schema (source of truth for shape/ranges/defaults) and
 *        on_apply (parses into the boot-applied policy config).
 *
 * Symbol note: the getters use a `sleep_` prefix, not the component's
 * usual `sm_`, socket_manager already links sm_settings_*.
 */
#include "esp_log.h"

#include "settings_manager.h"

#include "sleep_manager.h" /* sleep_manager_register_cli (settings-gated) */
#include "sleep_manager_private.h"

static const char *TAG = "sleep_manager";

#define SLEEP_WAKE_MV_MIN 12100
#define SLEEP_WAKE_MV_MAX 15000

static const settings_field_t FIELDS[] =
{
    /* shipping default ON, 5 min delay (meatpi 2026-07-18): a parked
       device must not drain the car battery out of the box */
    SETTINGS_BOOL("enabled", true),
    /* user-facing ranges (meatpi 2026-09-06): a 12 V vehicle battery,
       below 12 V it is flat (meatpi: 12.0 V floor), above ~14 V the engine
       is charging; a
       sleep delay beyond 30 min only drains the battery; a periodic
       check-in faster than 5 min is a wake-up storm */
    SETTINGS_INT("sleep_mv", 12000, 14000, 13100),
    /* v3 (2026-10-01, Quick Setup's Battery and sleep step): the wake
       voltage is the user's own number instead of sleep + 0.1 V. A wider
       band than the old derived 0.1 V stops a battery resting just above
       the sleep voltage from never sleeping, and surface charge after
       key-off from waking the device. Ceiling 15 V: above 14 V (the sleep
       maximum) a wake voltage only makes sense under the charging voltage
       of the car. The resolver pulls it to at least sleep + 0.1 V.
       Literal bounds: the web preview's schema extractor reads this table
       (SLEEP_WAKE_MV_MIN/MAX above carry the same numbers for the migration). */
    SETTINGS_INT("wake_mv", 12100, 15000, 13200),
    /* v4 (2026-10-01, Ali): "wake up after": how long the battery must stay
       above the wake voltage before the wake reboot. Replaces the fixed 1 s
       window; 0.1 to 5 s, default 0.5 s. Longer ignores short spikes (a door
       light, a remote unlock), shorter wakes faster. */
    SETTINGS_INT("wake_delay_ms", 100, 5000, 500),
    SETTINGS_INT("sleep_delay_min", 1, 30, 5),
    SETTINGS_BOOL("periodic_wakeup", false),
    SETTINGS_INT("wakeup_interval_min", 5, 1440, 30),
    /* bitz fork: IMU motion wakes a sleeping device and holds an awake
       one up while the vehicle moves. Voltage cannot tell parked from
       driving on a regulated-voltage alternator; motion can. 125 mg is the
       v4 fork's wom_threshold 32 x 3.9 mg, per axis between 2 s naps. */
    SETTINGS_BOOL("motion_wake", true),
    SETTINGS_INT("motion_mg", 30, 2000, 125),
    SETTINGS_BOOL("cli", true),
};

static sm_cfg_t s_cfg;
static bool s_enabled;
static bool s_configured;

const sm_cfg_t *sleep_settings_config(void)
{
    return &s_cfg;
}

bool sleep_settings_enabled(void)
{
    return s_enabled;
}

bool sleep_settings_is_configured(void)
{
    return s_configured;
}

static esp_err_t on_apply(const cJSON *settings)
{
    const cJSON *item;

    s_enabled = cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(settings, "enabled"));
    item = cJSON_GetObjectItemCaseSensitive(settings, "sleep_mv");

    int sleep_mv = cJSON_IsNumber(item) ? item->valueint : 13100;

    item = cJSON_GetObjectItemCaseSensitive(settings, "wake_mv");

    int wake_mv = cJSON_IsNumber(item) ? item->valueint : sleep_mv + 100;

    if (sm_resolve_thresholds(sleep_mv, wake_mv, &s_cfg.sleep_v,
                              &s_cfg.wake_v))
    {
        ESP_LOGW(TAG, "wake_mv %d is not above sleep_mv %d; waking at "
                 "%.2f V", wake_mv, sleep_mv, s_cfg.wake_v);
    }
    item = cJSON_GetObjectItemCaseSensitive(settings, "wake_delay_ms");
    s_cfg.wake_hold_ms = (uint32_t)(cJSON_IsNumber(item) ? item->valueint
                                                        : (int)SM_WAKE_DELAY_MS);
    item = cJSON_GetObjectItemCaseSensitive(settings, "sleep_delay_min");
    s_cfg.delay_ms = 60000u *
        (uint32_t)(cJSON_IsNumber(item) ? item->valueint : 5);
    s_cfg.periodic = cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(settings, "periodic_wakeup"));
    item = cJSON_GetObjectItemCaseSensitive(settings,
                                            "wakeup_interval_min");
    s_cfg.interval_ms = 60000u *
        (uint32_t)(cJSON_IsNumber(item) ? item->valueint : 30);
    s_cfg.motion_wake = !cJSON_IsFalse(
        cJSON_GetObjectItemCaseSensitive(settings, "motion_wake"));
    item = cJSON_GetObjectItemCaseSensitive(settings, "motion_mg");
    s_cfg.motion_g = (float)(cJSON_IsNumber(item) ? item->valueint : 125) /
                     1000.0f;

    if (!cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(settings, "cli")))
    {
        static bool s_cli_registered;

        if (!s_cli_registered && sleep_manager_register_cli() == ESP_OK)
        {
            s_cli_registered = true;
        }
    }

    s_configured = true;
    return ESP_OK;
}


/* Clamp a stored integer into the current schema range (migration helper). */
static void clamp_int(cJSON *settings, const char *key, int lo, int hi)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(settings, key);

    if (cJSON_IsNumber(v) && (v->valueint < lo || v->valueint > hi))
    {
        cJSON_SetNumberValue(v, (v->valueint < lo) ? lo : hi);
    }
}

static esp_err_t sleep_settings_migrate(uint32_t from_version, cJSON *settings)
{
    /* v1 -> v2 (2026-09-06): tighter user-facing ranges, clamp what a
       device has stored instead of degrading it */
    if (from_version < 2 && settings != NULL)
    {
        clamp_int(settings, "sleep_mv", 12000, 14000);
        clamp_int(settings, "sleep_delay_min", 1, 30);
        clamp_int(settings, "wakeup_interval_min", 5, 1440);
    }

    /* v2 -> v3 (2026-10-01): wake_mv becomes a setting; a document without
       one keeps the band it had (sleep + 100 mV, the pure rule above) so
       nothing changes for a configured device */
    if (from_version < 3 && settings != NULL &&
        cJSON_GetObjectItemCaseSensitive(settings, "wake_mv") == NULL)
    {
        cJSON *sleep = cJSON_GetObjectItemCaseSensitive(settings, "sleep_mv");
        int sleep_mv = cJSON_IsNumber(sleep) ? sleep->valueint : 13100;

        cJSON_AddNumberToObject(settings, "wake_mv",
                                sm_migrated_wake_mv(sleep_mv,
                                                    SLEEP_WAKE_MV_MIN,
                                                    SLEEP_WAKE_MV_MAX));
    }

    /* v3 -> v4 (2026-10-01): wake_delay_ms becomes a setting (the pure rule
       says what a document without one gets) */
    if (settings != NULL)
    {
        int add = sm_migrated_wake_delay_ms(
            from_version,
            cJSON_GetObjectItemCaseSensitive(settings, "wake_delay_ms") != NULL);

        if (add > 0)
        {
            cJSON_AddNumberToObject(settings, "wake_delay_ms", add);
        }
    }

    return ESP_OK;
}

esp_err_t sleep_settings_register(void)
{
    static const settings_descriptor_t DESC =
    {
        .name = "sleep_manager",
        .version = 4, /* v2: sane ranges (2026-09-06); v3: wake_mv; v4: wake_delay_ms (2026-10-01);
                         bitz motion_wake/motion_mg: non-breaking additions, no bump */
        .fields = FIELDS,
        .field_count = sizeof(FIELDS) / sizeof(FIELDS[0]),
        .on_apply = on_apply,
        .on_migrate = sleep_settings_migrate,
    };

    return settings_manager_register(&DESC);
}
