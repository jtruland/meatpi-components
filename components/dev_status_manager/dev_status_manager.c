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
 * @file dev_status_manager.c
 * @brief The one device-status event group: publish/read/wait + identity
 *        helpers. Bit changes log at DEBUG (they can be per-connection
 *        chatty), lifecycle at INFO (Coding Standard §10).
 */
#include "dev_status_manager.h"

#include <stdlib.h>
#include <string.h>

#include <stdio.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_spi_flash_counters.h" /* contents compile out when the
                                       counters Kconfig is off */
#include "esp_timer.h"
#include "sdkconfig.h"
#include "driver/temperature_sensor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/freertos_debug.h" /* xTaskGetNext, vTaskGetSnapshot */

#include "log_manager.h"

#include "dev_status_manager_private.h"

static const char *TAG = "dev_status_manager";

static EventGroupHandle_t s_events;
static StaticEventGroup_t s_events_buf; /* internal: FreeRTOS object */

static const esp_partition_t *s_running_partition;
static esp_app_desc_t s_app_desc;
static bool s_inited;

static void log_bits(const char *action, EventBits_t bits)
{
    for (int i = 0; i < 24; i++)
    {
        if ((bits & (1u << i)) != 0)
        {
            ESP_LOGD(TAG, "%s %s", action, dsm_bit_name(1u << i));
        }
    }
}

esp_err_t dev_status_manager_init(void)
{
    if (s_inited)
    {
        return ESP_OK;
    }

    static const log_descriptor_t LOG_DESC =
        { "dev_status_manager", ESP_LOG_INFO };

    log_manager_register(&LOG_DESC); /* per-TAG level control (§9.2) */

    s_events = xEventGroupCreateStatic(&s_events_buf);

    if (s_events == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    s_running_partition = esp_ota_get_running_partition();

    if (s_running_partition == NULL ||
        esp_ota_get_partition_description(s_running_partition, &s_app_desc)
            != ESP_OK)
    {
        memset(&s_app_desc, 0, sizeof(s_app_desc));
        ESP_LOGW(TAG, "running app descriptor unavailable");
    }

    dsm_faults_load(); /* latched fault codes survive until cleared */

    s_inited = true;
    ESP_LOGI(TAG, "up (partition=%s, version=%s)",
             dev_status_manager_partition_label(),
             dev_status_manager_app_version());
    dsm_events_register();
    return ESP_OK;
}

/* ---- internal-RAM guard ----------------------------------------------------------- */

static esp_timer_handle_t s_ram_timer;
static bool s_ram_low_raised;

static void ram_guard_cb(void *arg)
{
    (void)arg;

    if (s_ram_low_raised)
    {
        return; /* one latch per boot; the count keeps in NVS across boots */
    }

    uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    uint32_t free_now = (uint32_t)heap_caps_get_free_size(caps);
    uint32_t min_free = (uint32_t)heap_caps_get_minimum_free_size(caps);

    if (free_now < DEV_STATUS_RAM_LOW_FREE || min_free < DEV_STATUS_RAM_LOW_MIN)
    {
        char detail[48];

        snprintf(detail, sizeof(detail), "free=%lu min=%lu largest=%lu",
                 (unsigned long)free_now, (unsigned long)min_free,
                 (unsigned long)heap_caps_get_largest_free_block(caps));
        ESP_LOGW(TAG, "internal RAM low: %s (the cliff: event loop, BLE "
                 "restart and the BT controller fail silently below ~5 KB)",
                 detail);
        (void)dev_status_manager_fault_raise("internal_ram_low", detail);
        s_ram_low_raised = true;
    }
}

esp_err_t dev_status_manager_start(void)
{
    if (!s_inited)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_ram_timer == NULL)
    {
        const esp_timer_create_args_t args =
        {
            .callback = ram_guard_cb,
            .name = "ram_guard",
            .dispatch_method = ESP_TIMER_TASK,
        };

        if (esp_timer_create(&args, &s_ram_timer) == ESP_OK)
        {
            (void)esp_timer_start_periodic(s_ram_timer, 10 * 1000 * 1000);
        }
    }

    return ESP_OK;
}

esp_err_t dev_status_manager_stop(void)
{
    return ESP_OK; /* passive: nothing to stop */
}

/* change subscribers (event_manager glue, mqtt_manager's link task since
   2026-10-01): setter-context, no blocking. Sized with headroom (§12): the
   table sat at 2/2 the day the second registrant arrived */
static dev_status_change_cb_t s_change_cbs[4];

esp_err_t dev_status_manager_subscribe_changes(dev_status_change_cb_t cb)
{
    for (size_t i = 0;
         i < sizeof(s_change_cbs) / sizeof(s_change_cbs[0]); i++)
    {
        if (s_change_cbs[i] == NULL)
        {
            s_change_cbs[i] = cb;
            return ESP_OK;
        }
    }

    return ESP_ERR_NO_MEM;
}

static void notify_changes(EventBits_t before, EventBits_t now)
{
    EventBits_t changed = (before ^ now) & DEV_STATUS_ALL_BITS;

    if (changed == 0)
    {
        return;
    }

    for (size_t i = 0;
         i < sizeof(s_change_cbs) / sizeof(s_change_cbs[0]); i++)
    {
        if (s_change_cbs[i] != NULL)
        {
            s_change_cbs[i](changed, now);
        }
    }
}

void dev_status_manager_set(EventBits_t bits)
{
    if (s_events != NULL)
    {
        EventBits_t before = xEventGroupGetBits(s_events);

        xEventGroupSetBits(s_events, bits & DEV_STATUS_ALL_BITS);
        log_bits("set", bits);
        notify_changes(before, xEventGroupGetBits(s_events));
    }
}

void dev_status_manager_clear(EventBits_t bits)
{
    if (s_events != NULL)
    {
        EventBits_t before = xEventGroupGetBits(s_events);

        xEventGroupClearBits(s_events, bits & DEV_STATUS_ALL_BITS);
        log_bits("clear", bits);
        notify_changes(before, xEventGroupGetBits(s_events));
    }
}

void dev_status_manager_clear_all(void)
{
    if (s_events != NULL)
    {
        xEventGroupClearBits(s_events, DEV_STATUS_ALL_BITS);
        ESP_LOGD(TAG, "clear all");
    }
}

EventBits_t dev_status_manager_get(void)
{
    return (s_events != NULL) ? xEventGroupGetBits(s_events) : 0;
}

bool dev_status_manager_is_set(EventBits_t bit)
{
    return (dev_status_manager_get() & bit) != 0;
}

bool dev_status_manager_all_set(EventBits_t bits)
{
    return (dev_status_manager_get() & bits) == bits;
}

bool dev_status_manager_any_set(EventBits_t bits)
{
    return (dev_status_manager_get() & bits) != 0;
}

EventBits_t dev_status_manager_wait_all(EventBits_t bits, TickType_t timeout)
{
    if (s_events == NULL)
    {
        return 0;
    }

    return xEventGroupWaitBits(s_events, bits, pdFALSE, pdTRUE, timeout);
}

EventBits_t dev_status_manager_wait_any(EventBits_t bits, TickType_t timeout)
{
    if (s_events == NULL)
    {
        return 0;
    }

    return xEventGroupWaitBits(s_events, bits, pdFALSE, pdFALSE, timeout);
}

const char *dev_status_manager_bit_name(EventBits_t bit)
{
    return dsm_bit_name((uint32_t)bit);
}

size_t dev_status_manager_format_uptime(char *buf, size_t buf_len)
{
    return dsm_format_uptime((uint64_t)esp_timer_get_time(), buf, buf_len);
}

const char *dev_status_manager_app_version(void)
{
    return s_app_desc.version;
}

static const char *s_app_build = "";

void dev_status_manager_set_app_build(const char *build)
{
    s_app_build = (build != NULL) ? build : "";
}

const char *dev_status_manager_app_build(void)
{
    return s_app_build;
}

const char *dev_status_manager_partition_label(void)
{
    return (s_running_partition != NULL) ? s_running_partition->label : "";
}

/** @p walk: also find the largest free block. That walks the whole heap
 *  inside its critical section (interrupts off): 0.14 ms for the internal
 *  heap, 3 to 4 ms for the PSRAM heap (see dev_status_manager_memory). */
static void fill_heap(uint32_t caps, bool walk, dev_status_heap_t *h)
{
    h->total = (uint32_t)heap_caps_get_total_size(caps);
    h->free = (uint32_t)heap_caps_get_free_size(caps);
    h->min_free = (uint32_t)heap_caps_get_minimum_free_size(caps);
    h->largest_block = walk
                           ? (uint32_t)heap_caps_get_largest_free_block(caps)
                           : 0;
}

esp_err_t dev_status_manager_memory(dev_status_memory_t *out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    fill_heap(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, true, &out->internal);
    fill_heap(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, false, &out->psram);
    return ESP_OK;
}

esp_err_t dev_status_manager_memory_deep(dev_status_memory_t *out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    fill_heap(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, true, &out->internal);
    fill_heap(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, true, &out->psram);
    return ESP_OK;
}

esp_err_t dev_status_manager_flash(dev_status_flash_t *out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

#if CONFIG_SPI_FLASH_ENABLE_COUNTERS
    const esp_flash_counters_t *c = esp_flash_get_counters();

    out->write_count = c->write.count;
    out->write_bytes = c->write.bytes;
    out->erase_count = c->erase.count;
    out->erase_bytes = c->erase.bytes;
    return ESP_OK;
#else
    memset(out, 0, sizeof(*out));
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

#if configUSE_TRACE_FACILITY == 1
static char task_state_char(eTaskState st)
{
    switch (st)
    {
        case eRunning:   return 'X';
        case eReady:     return 'R';
        case eBlocked:   return 'B';
        case eSuspended: return 'S';
        case eDeleted:   return 'D';
        default:         return '?';
    }
}

/* One task between the two passes of dev_status_manager_task_stats().
 * `row` comes first: the array is sorted and copied out by rows. */
typedef struct
{
    dev_status_task_t row;
    TaskHandle_t      handle;
    const uint8_t    *stack_base; /* lowest address: unused stack starts here */
    const uint8_t    *stack_end;  /* highest valid address                    */
} task_scratch_t;

#define DS_STACK_FILL 0xA5u            /* what the kernel fills a new stack
                                          with (tasks.c tskSTACK_FILL_BYTE)  */
#define DS_STACK_MAX  (256u * 1024u)   /* larger than any stack: the bound of
                                          a count whose task is gone         */

static int task_cmp_runtime_desc(const void *a, const void *b)
{
    const task_scratch_t *ta = a;
    const task_scratch_t *tb = b;

    return (tb->row.runtime_us > ta->row.runtime_us) -
           (tb->row.runtime_us < ta->row.runtime_us);
}

static bool ram_ptr(const void *p)
{
    return esp_ptr_in_dram(p) || esp_ptr_external_ram(p);
}

/** Bytes of a stack that were never written: the fill pattern, counted from
 *  the stack's base. What uxTaskGetStackHighWaterMark() returns, but from
 *  addresses taken while the task was known to exist, and only ever reading
 *  RAM: the task may have ended since, its control block may be gone. */
static uint32_t stack_never_used(const uint8_t *base, const uint8_t *end)
{
    if (!ram_ptr(base) || end <= base || !ram_ptr(end) ||
        (size_t)(end - base) > DS_STACK_MAX)
    {
        return 0;
    }

    const uint8_t *p = base;

    while (p < end && *p == DS_STACK_FILL)
    {
        p++;
    }

    return (uint32_t)(p - base);
}
#endif

esp_err_t dev_status_manager_task_stats(dev_status_task_t *out, size_t cap,
                                        size_t *out_count,
                                        uint64_t *out_total_us)
{
#if configUSE_TRACE_FACILITY == 1
    if (out == NULL || cap == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    /* +4: tasks can spawn between the count and the snapshot */
    UBaseType_t slots = uxTaskGetNumberOfTasks() + 4;
    /* transient scratch (also the sort space, so a too-small @p cap keeps
       the BUSIEST tasks); PSRAM heap keeps internal RAM untouched */
    task_scratch_t *all = heap_caps_calloc(slots, sizeof(*all),
                                           MALLOC_CAP_SPIRAM |
                                               MALLOC_CAP_8BIT);

    if (all == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    configRUN_TIME_COUNTER_TYPE total = 0;
    UBaseType_t count = 0;
    TaskIterator_t it = { 0 };

    /* Pass 1, who exists: the task lists are walked with this core's
       scheduler suspended (the IDF's condition for xTaskGetNext) and
       interrupts ON. No stack is looked at here: tens of microseconds.
       The other core keeps running, so a list may change under the walk:
       the iterator only ever follows pointers into RAM, the walk is
       bounded, and a task seen twice is kept once. */
    vTaskSuspendAll();

    for (UBaseType_t step = 0; step < slots * 2u && count < slots; step++)
    {
        TaskStatus_t st;
        TaskSnapshot_t snap;
        bool seen = false;

        if (xTaskGetNext(&it) == -1)
        {
            break;
        }

        if (it.pxTaskHandle == NULL ||
            vTaskGetSnapshot(it.pxTaskHandle, &snap) != pdTRUE)
        {
            continue;
        }

        for (UBaseType_t k = 0; k < count && !seen; k++)
        {
            seen = (all[k].handle == it.pxTaskHandle);
        }

        if (seen)
        {
            continue;
        }

        /* no stack count (pdFALSE), no state lookup (a state is given) */
        vTaskGetInfo(it.pxTaskHandle, &st, pdFALSE, eReady);

        task_scratch_t *t = &all[count++];

        t->handle = it.pxTaskHandle;
        t->stack_base = (const uint8_t *)st.pxStackBase;
        t->stack_end = (const uint8_t *)snap.pxEndOfStack;
        strlcpy(t->row.name, st.pcTaskName, sizeof(t->row.name));
        t->row.prio = (uint8_t)st.uxCurrentPriority;
        t->row.stack_ext = esp_ptr_external_ram(st.pxStackBase);
#if configGENERATE_RUN_TIME_STATS == 1
        t->row.runtime_us = (uint64_t)st.ulRunTimeCounter;
#endif
#if configTASKLIST_INCLUDE_COREID == 1
        t->row.core = (st.xCoreID == tskNO_AFFINITY) ? -1
                                                     : (int8_t)st.xCoreID;
#else
        t->row.core = -1;
#endif
    }

#if configGENERATE_RUN_TIME_STATS == 1
    total = portGET_RUN_TIME_COUNTER_VALUE();
#endif
    (void)xTaskResumeAll();

    /* Pass 2, in plain task context: the state (a short kernel lock per
       task) and the unused stack (no lock at all: this is the part that
       takes milliseconds). */
    for (UBaseType_t i = 0; i < count; i++)
    {
        all[i].row.state = task_state_char(eTaskGetState(all[i].handle));
        all[i].row.stack_hw = stack_never_used(all[i].stack_base,
                                               all[i].stack_end);
    }

    qsort(all, count, sizeof(*all), task_cmp_runtime_desc);

    size_t n = (count < cap) ? count : cap;

    for (size_t i = 0; i < n; i++)
    {
        out[i] = all[i].row;
    }

    heap_caps_free(all);

    if (out_count != NULL)
    {
        *out_count = n;
    }

    if (out_total_us != NULL)
    {
        *out_total_us = (uint64_t)total;
    }

    return ESP_OK;
#else
    (void)out;
    (void)cap;
    (void)out_count;
    (void)out_total_us;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t dev_status_manager_temperature(float *out_c)
{
    if (out_c == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    static temperature_sensor_handle_t s_tsens; /* lazy, kept forever */

    if (s_tsens == NULL)
    {
        temperature_sensor_config_t cfg =
            TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
        esp_err_t err = temperature_sensor_install(&cfg, &s_tsens);

        if (err != ESP_OK)
        {
            return err;
        }

        err = temperature_sensor_enable(s_tsens);

        if (err != ESP_OK)
        {
            temperature_sensor_uninstall(s_tsens);
            s_tsens = NULL;
            return err;
        }
    }

    return temperature_sensor_get_celsius(s_tsens, out_c);
}

const char *dev_status_manager_device_id(void)
{
    /* legacy hw_config_get_device_id formula: 12 lowercase hex chars of the
       SoftAP MAC, the one device identity every derived name builds on
       (BLE name "WiCAN_<id>", STA hostname "wican_<id>", AP SSID) */
    static char s_id[13];

    if (s_id[0] == '\0')
    {
        uint8_t mac[6] = { 0 };

        esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
        snprintf(s_id, sizeof(s_id), "%02x%02x%02x%02x%02x%02x", mac[0],
                 mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    return s_id;
}

/* ---- the USB connector summary (2026-10-13) -------------------------------- */

static dev_status_usb_provider_t s_usb_provider;

void dev_status_manager_set_usb_provider(dev_status_usb_provider_t fn)
{
    s_usb_provider = fn;
}

bool dev_status_manager_usb(dev_status_usb_t *out)
{
    if (out == NULL)
    {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (s_usb_provider == NULL)
    {
        return false;
    }
    if (!s_usb_provider(out))
    {
        return false;
    }
    return out->kind != DEV_STATUS_USB_NONE;
}

/* ---- the SD slot summary (2026-10-25) ------------------------------------- */

static dev_status_sd_provider_t s_sd_provider;

void dev_status_manager_set_sd_provider(dev_status_sd_provider_t fn)
{
    s_sd_provider = fn;
}

bool dev_status_manager_sd(dev_status_sd_t *out)
{
    if (out == NULL)
    {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (s_sd_provider == NULL)
    {
        return false;
    }
    return s_sd_provider(out);
}
