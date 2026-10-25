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
 * @file dev_status_sd.h
 * @brief The SD slot summary (2026-10-25): what card is in the slot and
 *        whether WiCAN can read it, for the web UI's notice bar and the
 *        pages that mention the card, on the status poll they make anyway
 *        (`sd` on GET /api/status; absent while the slot is empty). The
 *        composition root fills it from external_storage (dev_status_manager
 *        must not know the storage components).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    char     state[12];      /**< "mounted" | "unreadable" | "failed" |
                                  "formatting" (external_storage's words)  */
    char     reason[20];     /**< unreadable: what the card holds ("exfat",
                                  "ntfs", "blank", "gpt", "unknown",
                                  "fat_corrupt"); failed: the esp_err name */
    char     name[8];        /**< the card's CID name, "" when it never
                                  answered                                  */
    uint64_t capacity_bytes; /**< 0 when the card never answered           */
    uint32_t serial;         /**< the CID serial: one card from another    */
} dev_status_sd_t;

/** Fill @p out; false = no card in the slot (no `sd` object). */
typedef bool (*dev_status_sd_provider_t)(dev_status_sd_t *out);

#ifdef __cplusplus
}
#endif
