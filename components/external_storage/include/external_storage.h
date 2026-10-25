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
 * @file external_storage.h
 * @brief WiCAN SD/MMC card owner (HAL component, Architecture §4).
 *
 * Owns the SD card as a DEVICE: card-detect supervision (hot-plug), the
 * SDMMC 4-bit host, and mounting the card's FAT filesystem at the `/sd`
 * VFS root. It knows nothing about what is stored: file access goes
 * through the `filesystem` component (`/sd/...` logical paths), which the
 * composition root wires to this component's mount events.
 *
 * Hot-plug: a detect task debounces the card-detect pin; insertion probes
 * the card (what filesystem it holds, BEFORE any mount: an exFAT card, the
 * factory format of every card over 32 GB, is reported instead of failing
 * loudly) and mounts a FAT card (+ publishes DEV_STATUS_BIT_SDCARD_MOUNTED
 * + fires the event callback); removal unmounts and clears. A yanked card
 * degrades /sd operations to ESP_ERR_INVALID_STATE: never a crash.
 *
 * A card that answers but holds no FAT volume (2026-10-25) can be formatted
 * on request: FAT32, one partition, everything on it erased. The request
 * is answered at once and the detect task does the work; the status says
 * how it went (`GET /api/sdcard`, `sdcard --format --yes`).
 *
 * WiCAN Pro wiring (SDMMC slot, GPIO-matrix routed):
 *   CLK=21 CMD=47 D0=14 D1=13 D2=12 D3=48, card-detect=GPIO40.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Mount-state change notification. Runs in the detect task: keep it
 *  short (the composition root uses it to flip filesystem's /sd backend). */
typedef void (*external_storage_event_cb_t)(bool mounted);

/** What the slot holds. */
typedef enum
{
    EXTERNAL_STORAGE_ABSENT = 0,  /**< no card (the detect pin)               */
    EXTERNAL_STORAGE_MOUNTED,     /**< a FAT card, mounted at /sd             */
    EXTERNAL_STORAGE_UNREADABLE,  /**< the card answers but holds no FAT
                                       volume WiCAN can mount: a format is
                                       offered (`reason` says what it holds) */
    EXTERNAL_STORAGE_FAILED,      /**< the card does not answer the host
                                       (`reason` = the esp_err name)         */
    EXTERNAL_STORAGE_FORMATTING,  /**< a format is running                   */
} external_storage_state_t;

typedef enum
{
    EXTERNAL_STORAGE_FORMAT_IDLE = 0, /**< no format since boot              */
    EXTERNAL_STORAGE_FORMAT_RUNNING,
    EXTERNAL_STORAGE_FORMAT_DONE,
    EXTERNAL_STORAGE_FORMAT_FAILED,   /**< `format.error` = the esp_err name */
} external_storage_format_state_t;

typedef struct
{
    external_storage_state_t state;
    const char *reason;        /**< UNREADABLE: "exfat" | "ntfs" | "blank" |
                                    "gpt" | "unknown" | "fat_corrupt";
                                    FAILED: the esp_err name; else ""      */
    bool        card_known;    /**< the card answered: the fields below hold */
    char        name[8];       /**< CID product name                        */
    const char *type;          /**< "SDHC/SDXC" | "SDSC" | "MMC" | "SDIO"   */
    uint64_t    capacity_bytes;
    uint32_t    sector_size;
    uint32_t    speed_khz;
    uint32_t    serial;        /**< CID serial: tells one card from another */
    uint32_t    cluster_bytes; /**< the allocation unit a format would use  */
    struct
    {
        external_storage_format_state_t state;
        const char *error;     /**< FAILED: the esp_err name, else ""      */
        uint32_t    took_ms;
        uint32_t    age_s;     /**< since the last format ended            */
    } format;
} external_storage_status_t;

/** GPIO + log setup; no card access. */
esp_err_t external_storage_init(void);

/** Start the detect task; probes and mounts immediately if a card is present. */
esp_err_t external_storage_start(void);

/** Unmount (if mounted) and stop the detect task. */
esp_err_t external_storage_stop(void);

/** Card physically present (debounced detect pin). */
bool external_storage_is_present(void);

/** Card mounted at /sd right now. */
bool external_storage_is_mounted(void);

/** The slot's state in full (the API, the CLI, the status summary). */
esp_err_t external_storage_status(external_storage_status_t *out);

/** The wire spelling of a state: "absent" | "mounted" | "unreadable" |
 *  "failed" | "formatting" (pure). */
const char *external_storage_state_str(external_storage_state_t s);

/** "idle" | "running" | "done" | "failed" (pure). */
const char *external_storage_format_state_str(external_storage_format_state_t s);

/**
 * Format the card in the slot as FAT32 (one partition, everything erased)
 * and mount it. Answers at once; the detect task does the work within its
 * next 250 ms lap and the status reports `formatting`, then `mounted` or
 * `unreadable` with `format.state` done / failed.
 *
 * @return ESP_OK started; ESP_ERR_INVALID_STATE no card, a card that does
 *         not answer, or a mounted card (nothing to fix: this never erases
 *         a card WiCAN can read); ESP_ERR_NOT_FINISHED a format is running.
 */
esp_err_t external_storage_format_request(void);

/** Register the ONE mount-event callback (composition root). */
esp_err_t external_storage_set_callback(external_storage_event_cb_t cb);

/** Register the `sdcard` CLI command with cmdline_manager. Called
 *  INTERNALLY on the settings boot apply when the `cli` setting is true
 *  (default): main no longer wires it. */
esp_err_t external_storage_register_cli(void);

/** Register GET /api/sdcard and POST /api/sdcard/format with
 *  http_server_manager (main, HTTP compositions). */
esp_err_t external_storage_register_http(void);

/** Register the settings descriptor ({cli}). Init runs before
 *  settings_manager_init, so the composition root calls this separately
 *  (the log_manager_register_settings pattern). */
esp_err_t external_storage_register_settings(void);

#ifdef __cplusplus
}
#endif
