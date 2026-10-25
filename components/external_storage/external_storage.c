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
 * @file external_storage.c
 * @brief The SD/MMC device owner: detect-pin supervision (hot-plug),
 *        SDMMC 4-bit host bring-up, the probe of what the card holds,
 *        FATFS mount at /sd, the format on request (2026-10-25),
 *        dev-status bit + event callback. Settings live in
 *        external_storage_settings.c (standard §4.1), the routes in
 *        external_storage_http.c, the `sdcard` command in
 *        external_storage_cli.c.
 */
#include "external_storage.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sd_protocol_defs.h"
#include "sdmmc_cmd.h"

#include "dev_status_manager.h"
#include "log_manager.h"

#include "external_storage_private.h"

static const char *TAG = "external_storage";

/* WiCAN Pro wiring (meatpi 2026-07-04) */
#define SDCARD_CLK        21
#define SDCARD_CMD        47
#define SDCARD_D0         14
#define SDCARD_D1         13
#define SDCARD_D2         12
#define SDCARD_D3         48
#define SDCARD_DETECT_PIN 40 /* microSD socket switch: LOW = card present */

#define ES_MOUNT_POINT    "/sd" /* == filesystem's FS_PREFIX_SD           */
#define ES_POLL_MS        250
#define ES_MAX_FILES      8

/* everything the API reports, written by the detect task under s_lock */
typedef struct
{
    external_storage_state_t state;
    es_fs_kind_t             kind;      /* UNREADABLE: what the probe found */
    esp_err_t                fail;      /* FAILED: why the card init failed */
    bool                     card_known;
    es_card_details_t        card;
    uint32_t                 serial;
    external_storage_format_state_t fmt_state;
    esp_err_t                fmt_err;
    uint32_t                 fmt_took_ms;
    int64_t                  fmt_end_us;
} es_state_t;

static sdmmc_card_t *s_card;     /* the MOUNTED card (the IDF allocates it) */
static volatile bool s_mounted;
static volatile bool s_present;
static volatile bool s_format_req;
static external_storage_event_cb_t s_cb;
static bool s_inited;
static esp_err_t s_last_mount_err;

static es_state_t s_st;
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf; /* internal: FreeRTOS object */

/* the probe pass brings the card up without a filesystem: its card struct
 * and the one sector buffer the sdmmc driver reads into */
static sdmmc_card_t s_probe_card EXT_RAM_BSS_ATTR;
static uint8_t s_sector[ES_SECTOR_BYTES] __attribute__((aligned(4))); /* internal: DMA */

static TaskHandle_t s_task;
static StaticTask_t s_tcb; /* internal: FreeRTOS object */
/* SD I/O goes through the driver's own DMA buffers; this task performs no
 * flash-cache-off work, so a PSRAM stack is compliant (§2). 6 KB: the
 * probe keeps two sectors on it and a format runs FatFs's f_mkfs here
 * (its work buffer is heap); the headroom is logged after every format. */
static StackType_t s_stack[6144] EXT_RAM_BSS_ATTR;
static volatile bool s_running;

static void lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_lock);
}

/* ---- the host and the card --------------------------------------------------- */

static bool read_present(void)
{
    return gpio_get_level(SDCARD_DETECT_PIN) == 0; /* active low */
}

static void host_setup(sdmmc_host_t *host, sdmmc_slot_config_t *slot)
{
    *host = (sdmmc_host_t)SDMMC_HOST_DEFAULT();
    host->max_freq_khz = SDMMC_FREQ_HIGHSPEED; /* 40 MHz; driver derates */

    *slot = (sdmmc_slot_config_t)SDMMC_SLOT_CONFIG_DEFAULT();
    slot->clk = SDCARD_CLK;
    slot->cmd = SDCARD_CMD;
    slot->d0 = SDCARD_D0;
    slot->d1 = SDCARD_D1;
    slot->d2 = SDCARD_D2;
    slot->d3 = SDCARD_D3;
    slot->width = 4;
    slot->flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
}

static const char *card_type_str(const sdmmc_card_t *c)
{
    return c->is_sdio ? "SDIO"
           : c->is_mmc ? "MMC"
           : (c->ocr & SD_OCR_SDHC_CAP) ? "SDHC/SDXC" : "SDSC";
}

/** Remember the card's identity (under s_lock). */
static void note_card(const sdmmc_card_t *c)
{
    snprintf(s_st.card.name, sizeof(s_st.card.name), "%s", c->cid.name);
    s_st.card.type = card_type_str(c);
    s_st.card.capacity_bytes =
        (uint64_t)c->csd.capacity * (uint64_t)c->csd.sector_size;
    s_st.card.sector_size = c->csd.sector_size;
    s_st.card.speed_khz = (uint32_t)c->max_freq_khz;
    s_st.serial = c->cid.serial;
    s_st.card_known = true;
}

static bool probe_read(void *ctx, uint32_t lba, uint8_t *out)
{
    sdmmc_card_t *c = ctx;

    if (sdmmc_read_sectors(c, s_sector, lba, 1) != ESP_OK)
    {
        return false;
    }

    memcpy(out, s_sector, ES_SECTOR_BYTES);
    return true;
}

/**
 * Bring the card up WITHOUT a filesystem: host + slot + card init, its
 * identity, the probe of what it holds, the host down again. Two card
 * inits per insertion (this one and the mount's) cost about 100 ms and
 * keep every call on the public IDF API.
 */
static esp_err_t card_probe(es_probe_t *pr)
{
    sdmmc_host_t host;
    sdmmc_slot_config_t slot;

    host_setup(&host, &slot);

    esp_err_t err = sdmmc_host_init();

    if (err != ESP_OK)
    {
        return err;
    }

    err = sdmmc_host_init_slot(host.slot, &slot);

    if (err == ESP_OK)
    {
        err = sdmmc_card_init(&host, &s_probe_card);
    }

    if (err == ESP_OK)
    {
        lock();
        note_card(&s_probe_card);
        unlock();
        es_probe(probe_read, &s_probe_card, pr);
    }

    sdmmc_host_deinit();
    return err;
}

/** Mount the card at /sd; with @p format, a volume FatFs cannot mount is
 *  partitioned and formatted first (FM_ANY without exFAT = FAT32 for any
 *  card over 2 GB) and mounted then. */
static bool card_mount(bool format)
{
    if (s_mounted)
    {
        return true;
    }

    sdmmc_host_t host;
    sdmmc_slot_config_t slot;
    uint64_t capacity;

    host_setup(&host, &slot);
    lock();
    capacity = s_st.card.capacity_bytes;
    unlock();

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg =
    {
        .format_if_mount_failed = format, /* a USER'S card: only on request */
        .max_files = ES_MAX_FILES,
        .allocation_unit_size = es_format_cluster_bytes(capacity),
    };
    sdmmc_card_t *card = NULL;
    esp_err_t err = esp_vfs_fat_sdmmc_mount(ES_MOUNT_POINT, &host, &slot,
                                            &mount_cfg, &card);

    if (err != ESP_OK)
    {
        s_last_mount_err = err;
        return false;
    }

    s_card = card;
    s_mounted = true;
    lock();
    s_st.state = EXTERNAL_STORAGE_MOUNTED;
    s_st.kind = ES_FS_FAT;
    note_card(card);
    unlock();
    dev_status_manager_set(DEV_STATUS_BIT_SDCARD_MOUNTED);
    ESP_LOGI(TAG, "mounted %s: %s %lluMB", ES_MOUNT_POINT, card->cid.name,
             ((uint64_t)card->csd.capacity * card->csd.sector_size) /
                 (1024 * 1024));

    if (s_cb != NULL)
    {
        s_cb(true);
    }

    return true;
}

/** A card arrived (or was there at boot): probe it, mount it if it is FAT,
 *  otherwise say what it holds with ONE W line. No E line for a card that
 *  merely needs a format: the boot_errors net is for faults. */
static void card_arrived(void)
{
    es_probe_t pr;
    esp_err_t err = card_probe(&pr);

    if (err != ESP_OK)
    {
        lock();
        s_st.state = EXTERNAL_STORAGE_FAILED;
        s_st.fail = err;
        s_st.card_known = false;
        unlock();
        ESP_LOGW(TAG, "card present but it does not answer: %s",
                 esp_err_to_name(err));
        return;
    }

    char name[8];
    uint64_t mb;

    lock();
    snprintf(name, sizeof(name), "%s", s_st.card.name);
    mb = s_st.card.capacity_bytes / (1024 * 1024);
    unlock();

    if (pr.kind == ES_FS_FAT)
    {
        if (card_mount(false))
        {
            return;
        }

        lock();
        s_st.state = EXTERNAL_STORAGE_UNREADABLE;
        s_st.kind = ES_FS_FAT_CORRUPT;
        unlock();
        ESP_LOGW(TAG, "card %s %lluMB: a FAT volume the mount refused (%s); "
                 "format it from the web UI or `sdcard --format --yes`",
                 name, (unsigned long long)mb, esp_err_to_name(s_last_mount_err));
        return;
    }

    lock();
    s_st.state = EXTERNAL_STORAGE_UNREADABLE;
    s_st.kind = pr.kind;
    unlock();
    ESP_LOGW(TAG, "card %s %lluMB present but not readable: %s%s (format it "
             "from the web UI or `sdcard --format --yes`)",
             name, (unsigned long long)mb, es_fs_kind_str(pr.kind),
             pr.read_error ? ", read error" : "");
}

static void card_gone(void)
{
    if (s_mounted)
    {
        /* callback first: consumers stop routing /sd before the rug moves */
        if (s_cb != NULL)
        {
            s_cb(false);
        }

        esp_vfs_fat_sdcard_unmount(ES_MOUNT_POINT, s_card);
        s_card = NULL;
        s_mounted = false;
        dev_status_manager_clear(DEV_STATUS_BIT_SDCARD_MOUNTED);
        ESP_LOGI(TAG, "unmounted");
    }

    lock();
    s_st.state = EXTERNAL_STORAGE_ABSENT;
    s_st.card_known = false;
    s_st.kind = ES_FS_UNKNOWN;
    s_st.fail = ESP_OK;
    unlock();
}

/** The format the API or the CLI asked for, on this task: FAT32 through
 *  the IDF's own format-on-mount path, then the usual mount. */
static void card_format(void)
{
    bool allowed;

    lock();
    allowed = s_st.state == EXTERNAL_STORAGE_UNREADABLE;

    if (allowed)
    {
        s_st.state = EXTERNAL_STORAGE_FORMATTING;
        s_st.fmt_state = EXTERNAL_STORAGE_FORMAT_RUNNING;
        s_st.fmt_err = ESP_OK;
    }

    unlock();

    if (!allowed)
    {
        return;
    }

    int64_t t0 = esp_timer_get_time();

    ESP_LOGI(TAG, "formatting the card as FAT32");

    bool ok = card_mount(true);
    uint32_t took = (uint32_t)((esp_timer_get_time() - t0) / 1000);

    lock();
    s_st.fmt_state = ok ? EXTERNAL_STORAGE_FORMAT_DONE
                        : EXTERNAL_STORAGE_FORMAT_FAILED;
    s_st.fmt_err = ok ? ESP_OK : s_last_mount_err;
    s_st.fmt_took_ms = took;
    s_st.fmt_end_us = esp_timer_get_time();

    if (!ok)
    {
        s_st.state = EXTERNAL_STORAGE_UNREADABLE;
    }

    unlock();

    if (ok)
    {
        ESP_LOGI(TAG, "formatted in %lu ms (detect task stack headroom %u B)",
                 (unsigned long)took,
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
        return;
    }

    ESP_LOGW(TAG, "format failed after %lu ms: %s", (unsigned long)took,
             esp_err_to_name(s_last_mount_err));
    card_arrived(); /* what the card holds now, or that it stopped answering */
}

bool es_card_details(es_card_details_t *out)
{
    bool known;

    if (out == NULL)
    {
        return false;
    }

    lock();
    known = s_st.card_known;
    *out = s_st.card;
    unlock();
    return known;
}

/* ---- detect task ----------------------------------------------------------------- */

static void detect_task(void *arg)
{
    es_detect_t det;

    (void)arg;
    es_detect_init(&det, read_present());
    s_present = det.stable_present;

    if (s_present)
    {
        card_arrived(); /* card already seated at boot */
    }

    while (s_running)
    {
        vTaskDelay(pdMS_TO_TICKS(ES_POLL_MS));

        if (s_format_req)
        {
            s_format_req = false;
            card_format();
        }

        if (!es_detect_feed(&det, read_present()))
        {
            continue;
        }

        s_present = det.stable_present;
        ESP_LOGI(TAG, "card %s", s_present ? "inserted" : "removed");

        if (s_present)
        {
            card_arrived();
        }
        else
        {
            card_gone();
        }
    }

    s_task = NULL;
    vTaskDelete(NULL);
}

/* ---- lifecycle --------------------------------------------------------------------- */

esp_err_t external_storage_init(void)
{
    if (s_inited)
    {
        return ESP_OK;
    }

    static const log_descriptor_t LOG_DESC =
        { "external_storage", ESP_LOG_INFO };

    log_manager_register(&LOG_DESC);

    s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);

    gpio_reset_pin(SDCARD_DETECT_PIN);
    gpio_set_direction(SDCARD_DETECT_PIN, GPIO_MODE_INPUT);
    gpio_pullup_en(SDCARD_DETECT_PIN); /* socket switch shorts to GND */

    s_inited = true;
    return ESP_OK;
}

esp_err_t external_storage_start(void)
{
    if (!s_inited)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_task != NULL)
    {
        return ESP_OK;
    }

    s_running = true;
    s_task = xTaskCreateStatic(detect_task, "sd_detect",
                               sizeof(s_stack) / sizeof(s_stack[0]), NULL,
                               5, s_stack, &s_tcb);
    return (s_task != NULL) ? ESP_OK : ESP_FAIL;
}

esp_err_t external_storage_stop(void)
{
    s_running = false;

    for (int i = 0; i < 25 && s_task != NULL; i++)
    {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    card_gone();
    return ESP_OK;
}

/* ---- status / registration ----------------------------------------------------------- */

bool external_storage_is_present(void)
{
    return s_present;
}

bool external_storage_is_mounted(void)
{
    return s_mounted;
}

esp_err_t external_storage_status(external_storage_status_t *out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    lock();
    out->state = s_st.state;
    out->reason = s_st.state == EXTERNAL_STORAGE_UNREADABLE
                      ? es_fs_kind_str(s_st.kind)
                  : s_st.state == EXTERNAL_STORAGE_FAILED
                      ? esp_err_to_name(s_st.fail)
                      : "";
    out->card_known = s_st.card_known;
    memcpy(out->name, s_st.card.name, sizeof(out->name));
    out->type = s_st.card.type != NULL ? s_st.card.type : "";
    out->capacity_bytes = s_st.card.capacity_bytes;
    out->sector_size = (uint32_t)s_st.card.sector_size;
    out->speed_khz = s_st.card.speed_khz;
    out->serial = s_st.serial;
    out->cluster_bytes = es_format_cluster_bytes(s_st.card.capacity_bytes);
    out->format.state = s_st.fmt_state;
    out->format.error = s_st.fmt_err != ESP_OK ? esp_err_to_name(s_st.fmt_err)
                                               : "";
    out->format.took_ms = s_st.fmt_took_ms;
    out->format.age_s =
        s_st.fmt_state == EXTERNAL_STORAGE_FORMAT_DONE ||
        s_st.fmt_state == EXTERNAL_STORAGE_FORMAT_FAILED
            ? (uint32_t)((esp_timer_get_time() - s_st.fmt_end_us) / 1000000)
            : 0;
    unlock();
    return ESP_OK;
}

esp_err_t external_storage_format_request(void)
{
    external_storage_state_t state;

    if (s_task == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    lock();
    state = s_st.state;
    unlock();

    if (state == EXTERNAL_STORAGE_FORMATTING || s_format_req)
    {
        return ESP_ERR_NOT_FINISHED;
    }

    if (state != EXTERNAL_STORAGE_UNREADABLE)
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_format_req = true;
    return ESP_OK;
}

esp_err_t external_storage_set_callback(external_storage_event_cb_t cb)
{
    s_cb = cb;
    return ESP_OK;
}
