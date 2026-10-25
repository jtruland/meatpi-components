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
 * @file dev_status_manager.h
 * @brief WiCAN device status manager (core component).
 *
 * Owns the ONE device-status event group. Feature components publish their
 * state by setting/clearing well-known bits; consumers (LED logic, sleep
 * manager, status API, UI) read snapshots or block on bit combinations.
 * Nobody keeps a private "is X connected" flag: this is the single source
 * of truth for live device state (Architecture §2: own the resource, let
 * others register into it).
 *
 * Bits are ephemeral runtime state: they reset on reboot and are NOT
 * settings. Only the lower 24 bits exist (FreeRTOS reserves the top 8).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "dev_status_usb.h"
#include "dev_status_sd.h"
#include "freertos/event_groups.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Well-known device status bits. Add new ones here (≤ BIT23) with a matching
 * name entry in dev_status_manager_fmt.c. */
#define DEV_STATUS_BIT_AWAKE            BIT0
#define DEV_STATUS_BIT_SLEEP            BIT1
#define DEV_STATUS_BIT_STA_CONNECTED    BIT2
#define DEV_STATUS_BIT_MQTT_CONNECTED   BIT3
#define DEV_STATUS_BIT_BLE_CONNECTED    BIT4
#define DEV_STATUS_BIT_SDCARD_MOUNTED   BIT5
#define DEV_STATUS_BIT_BLE_ENABLED      BIT6
#define DEV_STATUS_BIT_STA_ENABLED      BIT7
#define DEV_STATUS_BIT_AP_ENABLED       BIT8
#define DEV_STATUS_BIT_AUTOPID_ENABLED  BIT9
#define DEV_STATUS_BIT_HOME_MODE        BIT10
#define DEV_STATUS_BIT_DRIVE_MODE       BIT11
#define DEV_STATUS_BIT_SMARTCONNECT     BIT12
#define DEV_STATUS_BIT_STA_AP_OVERLAP   BIT13
#define DEV_STATUS_BIT_TIME_SYNCED      BIT14
#define DEV_STATUS_BIT_VPN_ENABLED      BIT15
#define DEV_STATUS_BIT_WAKE_VOLTAGE_OK  BIT16
#define DEV_STATUS_BIT_ETH_CONNECTED    BIT17
#define DEV_STATUS_BIT_AUTOPID_IDLE     BIT18
#define DEV_STATUS_BIT_MOTION           BIT19 /* imu_manager activity */
/* interface_manager's runtime arbitration state (WHY an interface is
 * down): the configured surface is untouched, the policy holds it */
#define DEV_STATUS_BIT_STA_SUSPENDED    BIT20
#define DEV_STATUS_BIT_AP_SUSPENDED     BIT21
#define DEV_STATUS_BIT_BLE_SUSPENDED    BIT22

/** Any upstream network path (WiFi STA or wired ETH). */
#define DEV_STATUS_NETWORK_CONNECTED_MASK \
    (DEV_STATUS_BIT_STA_CONNECTED | DEV_STATUS_BIT_ETH_CONNECTED)

#define DEV_STATUS_ALL_BITS ((EventBits_t)0x00FFFFFFu)

/** Create the event group and capture running-app/partition info. */
esp_err_t dev_status_manager_init(void);

/** Lifecycle uniformity (§3); passive component: both trivial. */
esp_err_t dev_status_manager_start(void);
esp_err_t dev_status_manager_stop(void);

/* ---- publish ------------------------------------------------------------- */

void dev_status_manager_set(EventBits_t bits);
void dev_status_manager_clear(EventBits_t bits);
void dev_status_manager_clear_all(void);

/* ---- read / wait ----------------------------------------------------------- */

EventBits_t dev_status_manager_get(void);
bool dev_status_manager_is_set(EventBits_t bit);
bool dev_status_manager_all_set(EventBits_t bits);
bool dev_status_manager_any_set(EventBits_t bits);

/** Block until ALL of @p bits are set (bits are not cleared on exit). */
EventBits_t dev_status_manager_wait_all(EventBits_t bits, TickType_t timeout);

/** Block until ANY of @p bits is set (bits are not cleared on exit). */
EventBits_t dev_status_manager_wait_any(EventBits_t bits, TickType_t timeout);

/* ---- identity / uptime helpers (for status transports) --------------------- */

/** Name of a single status bit ("sta_connected", ...); "unknown" if unmapped. */
const char *dev_status_manager_bit_name(EventBits_t bit);

/** Uptime as "HH:MM:SS" or "<d>d HH:MM:SS". Returns chars written, 0 on error. */
size_t dev_status_manager_format_uptime(char *buf, size_t buf_len);

/** Running app version string (from the app descriptor); "" before init. */
const char *dev_status_manager_app_version(void);

/** The build's git describe (2026-10-18: the version string is the
 *  release number from version.txt, so the commit lives here): set by
 *  the composition root from the build, "" before. Static text only. */
void dev_status_manager_set_app_build(const char *build);
const char *dev_status_manager_app_build(void);

/** Running partition label ("ota_0"/"ota_1"/...); "" before init. */
const char *dev_status_manager_partition_label(void);

/**
 * The device id: 12 lowercase hex chars of the SoftAP MAC (legacy
 * hw_config_get_device_id). THE one identity every derived name builds on:
 * BLE device name ("WiCAN_<id>"), STA hostname ("wican_<id>"), AP SSID.
 * Cached; safe to call from any component after boot.
 */
const char *dev_status_manager_device_id(void);

/* ---- memory observability (Architecture §12b) ------------------------------ */

/** One heap region's health. `largest_block` is THE fragmentation signal:
 *  a large gap between `free` and `largest_block` means the free space is
 *  shattered and big allocations will fail despite "enough" free bytes. */
typedef struct
{
    uint32_t total;         /* capability heap size at boot                */
    uint32_t free;          /* free bytes right now                        */
    uint32_t min_free;      /* lowest free ever (high-water mark)          */
    uint32_t largest_block; /* biggest single allocation possible now;
                               0 = not measured (the PSRAM heap in the
                               routine snapshot, see below)                */
} dev_status_heap_t;

typedef struct
{
    dev_status_heap_t internal; /* MALLOC_CAP_INTERNAL: the scarce one    */
    dev_status_heap_t psram;    /* MALLOC_CAP_SPIRAM                       */
} dev_status_memory_t;

/** Register the `version`/`status` CLI commands with cmdline_manager.
 *  Called INTERNALLY on the settings boot apply when the `cli` setting is
 *  true (default): main no longer wires it. */
esp_err_t dev_status_manager_register_cli(void);

/** Register the settings descriptor ({cli}). Init runs before
 *  settings_manager_init, so the composition root calls this separately
 *  (the log_manager_register_settings pattern). */
esp_err_t dev_status_manager_register_settings(void);

/**
 * Snapshot both heaps, poll-friendly: nothing in here keeps interrupts off
 * for longer than about 0.15 ms.
 *
 * `largest_block` needs a walk of the whole heap inside the heap's critical
 * section. For the internal heap (some 600 blocks) that is 0.14 ms and it is
 * measured. For the PSRAM heap (6000 blocks, slow memory) it is 3 to 4 ms
 * with interrupts off, which a busy CAN bus pays for in frames (the
 * controller's FIFO holds four: 74 frames lost in 30 s at five requests a
 * second, bench 2026-10-03). So psram.largest_block is 0 here.
 */
esp_err_t dev_status_manager_memory(dev_status_memory_t *out);

/**
 * The same, with the PSRAM heap's `largest_block`: 3 to 4 ms with interrupts
 * off. For a boot report or for something a person asked for (console
 * `system -m`, `GET /api/status?deep=1`). Never on a timer, never from a
 * route a page polls.
 */
esp_err_t dev_status_manager_memory_deep(dev_status_memory_t *out);

/* Internal-RAM guard (2026-09-22, TASK_internal_ram.md): a 10 s esp_timer
 * latches the fault `internal_ram_low` (once per boot, W log) when the
 * internal heap's free size drops under DEV_STATUS_RAM_LOW_FREE or its
 * lifetime minimum under DEV_STATUS_RAM_LOW_MIN. Below ~5 KB the device
 * misbehaves silently: the default event loop stops taking the web
 * server's events, BLE does not restart, the BT controller drops PDUs. */
#define DEV_STATUS_RAM_LOW_FREE 8192
#define DEV_STATUS_RAM_LOW_MIN  4096

/* ---- flash-write observability (2026-07-19) --------------------------------
 * Counts since boot, from the SPI-flash driver's own counters
 * (CONFIG_SPI_FLASH_ENABLE_COUNTERS). THE tripwire for the
 * "component silently rewrites flash every boot / on a timer" bug class
 * (settings_manager did exactly that: 37 files, every boot, for weeks):
 * the boot report prints them and the bench asserts a write budget and
 * zero idle writes. */
typedef struct
{
    uint32_t write_count; /* esp_flash_write calls                        */
    uint32_t write_bytes;
    uint32_t erase_count; /* erase ops: the wear that actually ages NOR  */
    uint32_t erase_bytes;
} dev_status_flash_t;

/** Flash op counters since boot. ESP_ERR_NOT_SUPPORTED (zeros) when the
 *  counters are compiled out. */
esp_err_t dev_status_manager_flash(dev_status_flash_t *out);

/* ---- device fault codes (2026-07-19) ----------------------------------------
 * The automotive-DTC idea for the firmware itself (meatpi): structural
 * problems (registry overflow, settings degraded, errors during boot, …)
 * are LATCHED to NVS and survive reboots/power cycles until MANUALLY
 * cleared: `faults -c` on the CLI or POST /api/faults/clear. Raise is
 * wear-disciplined: one NVS write on first occurrence, at most one
 * count-update per code per boot; recurrences count in RAM. */

#define DEV_STATUS_FAULT_MAX 16

typedef struct
{
    char     code[24];   /* stable identifier, e.g. "cmdline_table_full" */
    char     detail[48]; /* latest human-readable context                */
    uint32_t count;      /* total occurrences since last clear           */
    uint32_t first_time; /* unix seconds (0 = clock not set yet)         */
    uint32_t last_time;
} dev_status_fault_t;

/** Latch (or re-count) a fault code. Safe from any task context. */
esp_err_t dev_status_manager_fault_raise(const char *code,
                                         const char *detail);

/** Copy up to @p cap faults into @p out; returns the number stored.
 *  NULL @p out returns just the active count. */
int dev_status_manager_faults(dev_status_fault_t *out, int cap);

/** The manual clear (the "mode 04"). */
esp_err_t dev_status_manager_faults_clear(void);

/** Drop ONE latched code (2026-10-10: the boot after a partition table
 *  migration drops `boot_errors`, latched while the settings partition
 *  was missing). ESP_ERR_NOT_FOUND when it is not latched: no NVS write. */
esp_err_t dev_status_manager_fault_clear(const char *code);

/* ---- task monitoring (2026-07-08) ------------------------------------------ */

/** One task's live stats. `stack_hw` is stack that was NEVER used (bytes;
 *  small = close to overflow). `runtime_us` is the cumulative esp_timer
 *  time the scheduler charged to the task: CPU% comes from DELTAS
 *  between two snapshots: task_delta / (total_delta * cores). */
typedef struct
{
    char     name[16];
    char     state;      /* X running, R ready, B blocked, S suspended,
                            D deleted-pending-cleanup                    */
    int8_t   core;       /* 0 / 1, -1 = unpinned                        */
    uint8_t  prio;
    uint32_t stack_hw;   /* stack high-water: unused bytes remaining    */
    uint64_t runtime_us; /* cumulative run time (u64: never wraps)     */
    bool     stack_ext;  /* stack lives in PSRAM (else internal DRAM)   */
} dev_status_task_t;

/**
 * Poll-friendly since 2026-10-03: the tasks are listed with the scheduler
 * of the calling core suspended (interrupts stay on), and the unused bytes
 * of each stack are counted afterwards in plain task context. The kernel's
 * own uxTaskGetSystemState() counts them inside its critical section: 6 ms
 * with interrupts off for 47 tasks (478 CAN frames lost in 30 s at five
 * requests a second). A task that ends between the two passes may show a
 * meaningless `stack_hw` once.
 *
 * Snapshot every task into @p out (capacity @p cap): fills @p out_count
 * and @p out_total_us (scheduler time since boot, PER CORE, multiply by
 * core count for the CPU% denominator; both nullable). Sorted by
 * runtime, busiest first. Needs CONFIG_FREERTOS_USE_TRACE_FACILITY (+
 * _GENERATE_RUN_TIME_STATS for runtimes); ESP_ERR_NOT_SUPPORTED without.
 * ESP_ERR_NO_MEM on scratch-alloc failure; if @p cap is too small the
 * BUSIEST @p cap tasks are kept (out_count says how many were written).
 */
esp_err_t dev_status_manager_task_stats(dev_status_task_t *out, size_t cap,
                                        size_t *out_count,
                                        uint64_t *out_total_us);

/** Die temperature in °C (ESP32-S3 internal sensor; installed lazily on
 *  first call, -10..80 °C measurement range). */
esp_err_t dev_status_manager_temperature(float *out_c);

/* ---- the USB connector summary (2026-10-13) ------------------------------- */

/* The types and the kind's spelling: dev_status_usb.h (pure). What is on
 * the USB connector and its one-line state, for the header pill and the
 * ESPNetLink bar of the web UI on the status poll they make anyway (`usb`
 * on GET /api/status). The composition root provides it: it knows the USB
 * components, this manager must not. */

/** The one provider (main's), asked per status request; NULL = no `usb`. */
void dev_status_manager_set_usb_provider(dev_status_usb_provider_t fn);

/** The summary now: false when there is no provider or nothing is on the
 *  connector (@p out zeroed, kind NONE). */
bool dev_status_manager_usb(dev_status_usb_t *out);

/* ---- the SD slot summary (2026-10-25) --------------------------------------- */

/* The types: dev_status_sd.h. What card is in the slot and whether WiCAN
 * can read it (`sd` on GET /api/status, absent while the slot is empty),
 * for the web UI's notice bar and the pages that mention the card. The
 * composition root provides it from external_storage. */

/** The one provider (main's), asked per status request; NULL = no `sd`. */
void dev_status_manager_set_sd_provider(dev_status_sd_provider_t fn);

/** The summary now: false when there is no provider or no card in the
 *  slot (@p out zeroed). */
bool dev_status_manager_sd(dev_status_sd_t *out);

#ifdef __cplusplus
}
#endif

/* ---- change notification (event_manager glue and friends) ------------------ */

/** Called after any bit change: @p changed = XOR of old/new, @p now =
 *  the bits after. Runs in the SETTER's context: keep it non-blocking
 *  (queue sends only). ≤4 subscribers (event_manager glue and
 *  mqtt_manager today), registered for life; ESP_ERR_NO_MEM when full. */
typedef void (*dev_status_change_cb_t)(EventBits_t changed,
                                       EventBits_t now);
esp_err_t dev_status_manager_subscribe_changes(dev_status_change_cb_t cb);

/** event_manager glue: declare the `status.bit` source + hook changes
 *  (dev_status_manager_events.c). Called from init. */
void dsm_events_register(void);
