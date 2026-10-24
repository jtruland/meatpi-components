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
 * @file autopid_scan_walk.c
 * @brief The detection job's last phase (`pids`): walk the support
 *        bitmaps of the car's dialect and turn every supported PID of the
 *        SAE table into a config row. Split out of autopid_std_scan.c
 *        2026-10-03 (700-line rule) when the walk learned dialects
 *        (TASK_j1939_wwh.md phase 3). Scan-task context.
 *
 *   obd2   `0100`, `0120`, .. `01A0` with headers off: the answers of all
 *          ECUs OR-merge, the rows are asked functionally, exactly as
 *          before dialects.
 *   uds    `22F400` .. `22F4E0` with headers ON: every ECU's bitmaps are
 *          kept apart, and each row is given to the LOWEST responder that
 *          has the PID (ap_dialect_pid_owner), addressed physically.
 *
 * The rows as a new car's tables document live in autopid_scan_rows.c
 * (pure, host-tested) since 2026-10-09.
 */
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"

#include "cJSON.h"

#include "autopid_transport.h"
#include "autopid_private.h"

#define AP_WALK_REQ_TIMEOUT pdMS_TO_TICKS(10000) /* SEARCHING can be slow */
#define AP_WALK_AT_TIMEOUT  pdMS_TO_TICKS(2000)

static esp_err_t ask(const char *cmd, char *resp, size_t resp_len,
                     TickType_t timeout)
{
    resp[0] = '\0';
    return ap_be()->request(cmd, resp, resp_len, timeout);
}

bool ap_scan_walk(ap_dialect_t dialect, cJSON *supported, uint16_t *found,
                  char *resp, size_t resp_len)
{
    /* scan task only: every responder's bitmaps, every range */
    static ap_dialect_ecu_t tab[AP_VEH_ECUS_MAX] EXT_RAM_BSS_ATTR;
    int n_tab = 0;
    bool any_response = false;
    bool per_ecu = (dialect == AP_DIALECT_UDS);

    if (supported == NULL || found == NULL || resp == NULL)
    {
        return false;
    }

    /* the prelude's headers are on for every CAN protocol (2026-10-24,
       issue #1014): a `uds` dialect's bitmaps come apart per ECU by their
       ids without a toggle here */
    for (int range = 0; range < ap_dialect_ranges(dialect); range++)
    {
        uint8_t base = (uint8_t)(range * 0x20);
        char cmd[12];
        ap_veh_ecu_t row[AP_VEH_ECUS_MAX];

        if (ap_dialect_pid_cmd(dialect, base, cmd, sizeof(cmd)) == 0 ||
            ask(cmd, resp, resp_len, AP_WALK_REQ_TIMEOUT) != ESP_OK)
        {
            break;
        }

        int n_row = ap_dialect_bitmaps(dialect, resp, base, row,
                                       AP_VEH_ECUS_MAX);

        if (n_row == 0)
        {
            break;      /* NO DATA / noise: range unsupported */
        }

        any_response = true;
        n_tab = ap_dialect_table_add(tab, n_tab, AP_VEH_ECUS_MAX, range, row,
                                     n_row);

        if (!ap_dialect_table_more(tab, n_tab, range))
        {
            break;      /* next range not supported */
        }
    }

    for (int pid = 1; any_response && pid < 256; pid++)
    {
        if (!ap_dialect_table_has(tab, n_tab, (uint8_t)pid))
        {
            continue;
        }

        uint32_t owner = per_ecu
                             ? ap_dialect_pid_owner(tab, n_tab, (uint8_t)pid)
                             : UINT32_MAX;

        if (ap_std_entry_to_json(supported, (uint8_t)pid, dialect, owner))
        {
            (*found)++;
        }
    }

    return any_response;
}
