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
 * @file usb_acm_cli_settings.c
 * @brief settings_manager descriptor for usb_acm_cli: field-table schema
 *        (source of truth for shape/defaults) and on_apply (stores the
 *        boot-applied enabled flag; registers the CLI, settings-gated per
 *        standard §6b).
 */
#include "settings_manager.h"

#include "usb_acm_cli_private.h"

static const settings_field_t FIELDS[] =
{
    /* bitz fork: default ON so a USB GPS (u-blox 7 / VK-162) feeds the
       fix to HA with no setup; the RX task only starts once a CDC-ACM
       device actually attaches, so this costs nothing without one */
    SETTINGS_BOOL("enabled", true),
    SETTINGS_BOOL("cli", true),
};

/* boot-applied settings */
static bool s_configured;
static bool s_enabled;

bool usb_acm_cli_settings_enabled(void)
{
    return s_enabled;
}

bool usb_acm_cli_settings_is_configured(void)
{
    return s_configured;
}

static esp_err_t on_apply(const cJSON *settings)
{
    s_enabled = cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(settings, "enabled"));

    if (!cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(settings, "cli")))
    {
        static bool reg;

        if (!reg && usb_acm_cli_register_cli() == ESP_OK)
        {
            reg = true;
        }
    }

    s_configured = true;
    return ESP_OK;
}

esp_err_t usb_acm_cli_settings_register(void)
{
    static const settings_descriptor_t DESC =
    {
        .name        = "usb_acm_cli",
        .version     = 1,
        .fields      = FIELDS,
        .field_count = sizeof(FIELDS) / sizeof(FIELDS[0]),
        .on_apply    = on_apply,
    };

    return settings_manager_register(&DESC);
}
