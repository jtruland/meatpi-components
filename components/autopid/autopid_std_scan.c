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
 * @file autopid_std_scan.c
 * @brief The async standard-PID support scan ("scan once, store",
 *        TASK_autopid.md §7) grown into the vehicle DETECTION job
 *        (TASK_quick_setup.md, second pass). Split out of autopid_std.c
 *        2026-10-01.
 *
 * POST /api/autopid/vehicles/detect (alias: POST /api/autopid/std_scan)
 * spawns a short-lived task (INTERNAL stack: it writes /data at the end,
 * §2) that pauses the poller and runs three phases, reported as `phase`
 * in the status JSON:
 *   protocol  which protocol AND which dialect (autopid_dialect.h) the
 *             car answers. With `std_protocol` = "0": a walk over the
 *             ISO 15765-4 protocols the bus guard allows for this bus,
 *             `0100` (OBD-II) and `22F400` (ISO 27145 / SAE J1979-2) on
 *             each. Until 2026-10-03 this was the chip's own search
 *             (ATTP0), which asks `0100` only: it cannot find a
 *             UDS-dialect vehicle and takes 9.3 s to give up. A pinned
 *             `std_protocol` is asked in both dialects. What first contact
 *             just found (a hint) is tried before any of that;
 *   vin       the responder set and the VIN, in the car's dialect
 *             (autopid_identify.c);
 *   pids      the support bitmap walk mapped onto the SAE table
 *             (autopid_scan_walk.c);
 *   network   is the vehicle network J1939 (phase 5): the listener's store
 *             when it runs, a one second sample of a live bus otherwise;
 *             the built-in SPN rows of the groups heard join the result,
 *             a J1939-only vehicle gets the dialect `j1939` and its source
 *             addresses as the responder set (autopid_j1939_std.c).
 * The result goes to the vehicle store (autopid_vehicle_detected: a
 * known car comes back with its own tables, an unknown car becomes a new
 * entry polled on the standard rows found here), then to
 * /data/autopid/std_scan.json (atomic). The poller starts this job by
 * itself once per boot when first contact meets an unknown car; the UI
 * owns the button otherwise.
 */
#include "autopid_private.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "filesystem.h"
#include "obd_chip.h"

static const char *TAG = "autopid";

#define AP_STD_SCAN_PATH    "/data/autopid/std_scan.json"
#define AP_SCAN_REQ_TIMEOUT pdMS_TO_TICKS(10000) /* SEARCHING can be slow  */
#define AP_SCAN_AT_TIMEOUT  pdMS_TO_TICKS(2000)

typedef enum
{
    SCAN_IDLE = 0,
    SCAN_RUNNING,
    SCAN_DONE,
    SCAN_FAILED,
} scan_state_t;

typedef enum
{
    PHASE_IDLE = 0,
    PHASE_PROTOCOL,
    PHASE_VIN,
    PHASE_PIDS,
    PHASE_NETWORK,
} scan_phase_t;

static volatile scan_state_t s_scan_state;
static volatile scan_phase_t s_scan_phase;
static uint16_t s_scan_found;
static char     s_scan_err[176];  /* holds a bus guard sentence too */
static int64_t  s_scan_ts;      /* epoch seconds of last completed scan */

static StaticTask_t s_scan_tcb;                    /* internal object    */
static StackType_t  s_scan_stack[6144];            /* INTERNAL: fs write */

static char s_resp[AP_RESP_MAX] EXT_RAM_BSS_ATTR;  /* scan task only     */

/* what first contact found a moment ago (ap_std_scan_hint): the next job
   tries it first, once */
static volatile char    s_hint_proto;
static volatile uint8_t s_hint_dialect;
static uint16_t s_no_way_kbps;  /* scan task: the bus bitrate that left the
                                   job no protocol to ask on (0 = asked)  */

const char *autopid_std_scan_path(void)
{
    return AP_STD_SCAN_PATH;
}

const char *ap_std_prelude(void)
{
    /* The chip baseline for the scan AND the poller (its boot prelude and
       the restore after an external ELM app / diagnostic tool had the
       chip - bench 2026-09-08: Car Scanner's ATS0 made every resumed poll
       fail to parse until the next reboot). A pinned `std_protocol`
       (6..9) wins; "0" pins the current car's protocol from the vehicle
       store, or ATTP0 (chip search) when there is none or it went silent
       this boot (ap_runner_proto_fallback). */
    return ap_veh_prelude_for(ap_veh_effective_protocol(
        ap_core_std_protocol(), autopid_vehicle_protocol(),
        ap_runner_proto_fallback()));
}

/* ---- chip helpers (scan task context) ----------------------------------------- */

static esp_err_t scan_request(const char *cmd, TickType_t timeout)
{
    s_resp[0] = '\0';
    return obd_chip_request(cmd, s_resp, sizeof(s_resp), timeout);
}

/** Send a ';'-separated prelude one command at a time. */
static void scan_send_prelude(const char *prelude)
{
    const char *p = prelude;

    while (*p != '\0')
    {
        const char *sep = strchr(p, ';');
        size_t len = (sep != NULL) ? (size_t)(sep - p) : strlen(p);
        char one[24];

        if (len > 0 && len < sizeof(one))
        {
            memcpy(one, p, len);
            one[len] = '\0';
            (void)scan_request(one, AP_SCAN_AT_TIMEOUT);
        }

        p += len + ((sep != NULL) ? 1 : 0);
    }
}

/* ---- phase: protocol ------------------------------------------------------------- */

/** Does anything answer the bitmap request of range 00 in @p dialect on
 *  what the chip is set to now? */
static bool scan_alive(ap_dialect_t dialect)
{
    char cmd[12];
    ap_veh_ecu_t ecus[AP_VEH_ECUS_MAX];

    return ap_dialect_pid_cmd(dialect, 0x00, cmd, sizeof(cmd)) > 0 &&
           scan_request(cmd, AP_SCAN_REQ_TIMEOUT) == ESP_OK &&
           ap_dialect_bitmaps(dialect, s_resp, 0x00, ecus,
                              AP_VEH_ECUS_MAX) > 0;
}

void ap_std_scan_hint(char proto, ap_dialect_t dialect)
{
    s_hint_dialect = (uint8_t)dialect;
    s_hint_proto = proto;
}

/** Which protocol and which dialect answer (see the file header).
 *  @return false when nothing did. */
static bool scan_contact(char *proto, ap_dialect_t *dialect)
{
    const char *setting = ap_core_std_protocol();
    bool pinned = (setting[0] >= '6' && setting[0] <= '9');
    char hint = s_hint_proto;
    ap_dialect_t hint_dialect = (ap_dialect_t)s_hint_dialect;

    s_hint_proto = '\0';

    if (hint != '\0' && ap_dialect_has_requests(hint_dialect) &&
        (!pinned || hint == setting[0]))
    {
        scan_send_prelude(ap_veh_prelude_for(hint));

        if (scan_alive(hint_dialect))
        {
            *proto = hint;
            *dialect = hint_dialect;
            return true;
        }
    }

    if (pinned)
    {
        /* the user's protocol: both dialects on it, no search */
        *proto = setting[0];
        scan_send_prelude(ap_veh_prelude_for(setting[0]));

        if (scan_alive(AP_DIALECT_OBD2))
        {
            *dialect = AP_DIALECT_OBD2;
            return true;
        }

        if (scan_alive(AP_DIALECT_UDS))
        {
            *dialect = AP_DIALECT_UDS;
            return true;
        }

        return false;
    }

    /* nobody named a protocol: the ISO 15765-4 protocols this bus
       allows, both dialects on each. The guard looks at the bus again
       before each of them. */
    char cand[AP_DIALECT_CAND_LEN];
    ap_bus_t bus;

    ap_guard_bus(&bus);

    int n = ap_dialect_can_candidates(&bus, cand);

    if (n == 0 && bus.kind == AP_BUS_LIVE)
    {
        s_no_way_kbps = bus.kbps;       /* e.g. a 125 kbit/s body bus */
    }

    for (int i = 0; i < n; i++)
    {
        if (!ap_guard_candidate_ok(cand[i]))
        {
            continue;
        }

        scan_send_prelude(ap_veh_prelude_for(cand[i]));
        *proto = cand[i];

        if (scan_alive(AP_DIALECT_OBD2))
        {
            *dialect = AP_DIALECT_OBD2;
            return true;
        }

        if (scan_alive(AP_DIALECT_UDS))
        {
            *dialect = AP_DIALECT_UDS;
            return true;
        }
    }

    *proto = '\0';
    return false;
}

/** `22F810`, the protocol identification of ISO 27145-4: recorded when
 *  the vehicle answers it, never needed. */
static void scan_uds_protocol_id(cJSON *root)
{
    uint8_t id = 0;

    if (scan_request("22F810", AP_SCAN_AT_TIMEOUT) == ESP_OK &&
        ap_dialect_uds_protocol_id(s_resp, &id))
    {
        cJSON_AddNumberToObject(root, "uds_protocol_id", id);
    }
}

/* ---- the job ------------------------------------------------------------------------ */

/** Hand the result to the vehicle store and stamp the result document
 *  with the car it landed on (`key`, `known`, `name`). */
static void scan_store(cJSON *root, const cJSON *supported,
                       ap_veh_seen_t *seen, uint16_t found)
{
    static ap_veh_entry_t entry EXT_RAM_BSS_ATTR; /* scan task only     */
    bool known = false;
    /* whatever the table says, no parameter name twice in what is stored
       (the result document and the car's rows alike) */
    int renamed = ap_names_dedupe((cJSON *)supported);
    /* a new car's rows are stored OFF: nothing is read until the user
       ticks the ones they want (Ali, 2026-10-09) */
    char *tables = ap_scan_rows_config(supported);
    esp_err_t err = ESP_ERR_NO_MEM;

    if (renamed > 0)
    {
        ESP_LOGW(TAG, "detection: %d repeated parameter name%s made unique",
                 renamed, renamed == 1 ? "" : "s");
    }

    seen->std_supported = found;

    if (tables != NULL)
    {
        err = autopid_vehicle_detected(seen, tables, strlen(tables), &entry,
                                       &known);
        free(tables);
    }

    if (err == ESP_OK)
    {
        cJSON_AddStringToObject(root, "key", entry.key);
        cJSON_AddBoolToObject(root, "known", known);
        cJSON_AddStringToObject(root, "name", entry.name);

        if (!known && found > 0)
        {
            ESP_LOGI(TAG, "detection: the %u rows of %s are stored off: "
                          "tick the ones to read under Automate > Parameters "
                          "or in the Quick Setup", (unsigned)found,
                     entry.key);
        }
    }
    else
    {
        /* a car without VIN or responders cannot be stored: the table
           still is (the user can merge it by hand) */
        ESP_LOGW(TAG, "detection: not stored (%s)", esp_err_to_name(err));
        cJSON_AddStringToObject(root, "key", "");
        cJSON_AddBoolToObject(root, "known", false);
        cJSON_AddStringToObject(root, "name", "");
    }
}

static void scan_task(void *arg)
{
    (void)arg;

    ap_core_scan_pause(true);

    static ap_veh_seen_t seen EXT_RAM_BSS_ATTR;      /* scan task only  */
    const char *setting = ap_core_std_protocol();
    char proto = '\0';
    ap_dialect_t dialect = AP_DIALECT_OBD2;
    char fp[AP_FP_LEN] = "";

    memset(&seen, 0, sizeof(seen));
    s_no_way_kbps = 0;

    s_scan_phase = PHASE_PROTOCOL;

    bool contact = scan_contact(&proto, &dialect);

    if (contact)
    {
        seen.protocol[0] = proto;
        seen.protocol[1] = '\0';
        /* the protocol's prelude for the rest (headers on for CAN, the
           functional header, the receive filter cleared) */
        scan_send_prelude(ap_veh_prelude_for(proto));

        s_scan_phase = PHASE_VIN;
        (void)ap_identify(dialect, proto, true, &seen, s_resp,
                          sizeof(s_resp));
        ap_veh_fingerprint(seen.ecus, seen.n_ecus, fp);
    }

    seen.dialect = (uint8_t)dialect;

    cJSON *root = cJSON_CreateObject();
    cJSON *supported = NULL;
    uint16_t found = 0;
    bool any_response = false;

    if (root != NULL)
    {
        cJSON_AddNumberToObject(root, "version", 1);
        cJSON_AddStringToObject(root, "protocol", setting);
        cJSON_AddStringToObject(root, "protocol_detected", seen.protocol);
        cJSON_AddStringToObject(root, "dialect", ap_dialect_name(dialect));
        cJSON_AddStringToObject(root, "vin", seen.vin);
        cJSON_AddStringToObject(root, "fingerprint", fp);
        supported = cJSON_AddArrayToObject(root, "supported");
    }

    if (contact && supported != NULL)
    {
        if (dialect == AP_DIALECT_UDS)
        {
            scan_uds_protocol_id(root);
        }

        s_scan_phase = PHASE_PIDS;
        any_response = ap_scan_walk(dialect, supported, &found, s_resp,
                                    sizeof(s_resp));
    }

    /* the network (phase 5): a J1939 vehicle beside its OBD dialect (an EU
       truck) or instead of one. The listener's store when it runs; a one
       second sample of a live bus otherwise (nothing transmitted). */
    bool j1939 = false;
    bool listening = ap_j1939_listening();

    if (supported != NULL)
    {
        ap_bus_t bus;

        s_scan_phase = PHASE_NETWORK;
        ap_guard_bus(&bus);

        if (listening)
        {
            j1939 = ap_j1939_identify(&seen, supported, &found);
        }
        else if (bus.kind == AP_BUS_LIVE)
        {
            j1939 = ap_j1939_identify_sample(&seen, supported, &found);
        }

        if (j1939 && !contact)
        {
            dialect = AP_DIALECT_J1939;
            seen.dialect = (uint8_t)dialect;
            ap_veh_fingerprint(seen.ecus, seen.n_ecus, fp);
            cJSON_ReplaceItemInObject(root, "dialect",
                                      cJSON_CreateString(
                                          ap_dialect_name(dialect)));
            cJSON_ReplaceItemInObject(root, "vin",
                                      cJSON_CreateString(seen.vin));
            cJSON_ReplaceItemInObject(root, "fingerprint",
                                      cJSON_CreateString(fp));
        }

        cJSON_AddBoolToObject(root, "j1939", j1939);
        cJSON_AddBoolToObject(root, "j1939_listening", listening);
        cJSON_AddNumberToObject(root, "bus_kbps",
                                (bus.kind == AP_BUS_LIVE) ? bus.kbps : 0);
        any_response = any_response || j1939;
    }

    esp_err_t err = ESP_FAIL;

    if (root == NULL)
    {
        snprintf(s_scan_err, sizeof(s_scan_err), "out of memory");
    }
    else if (!any_response && s_no_way_kbps != 0)
    {
        snprintf(s_scan_err, sizeof(s_scan_err),
                 "the vehicle bus runs at %u kbit/s; OBD is asked at 250 or "
                 "500, so nothing was sent", (unsigned)s_no_way_kbps);
    }
    else if (!any_response)
    {
        snprintf(s_scan_err, sizeof(s_scan_err),
                 "no ECU response (ignition on?)");
    }
    else
    {
        cJSON_AddNumberToObject(root, "found", found);
        cJSON_AddNumberToObject(root, "ts", (double)time(NULL));

        /* the store first (entry, tables, switch, chip protocol), then
           the result document */
        scan_store(root, supported, &seen, found);

        char *body = cJSON_PrintUnformatted(root);

        if (body != NULL)
        {
            err = filesystem_write(AP_STD_SCAN_PATH, body, strlen(body));
            free(body);
        }

        if (err != ESP_OK)
        {
            snprintf(s_scan_err, sizeof(s_scan_err), "store failed");
        }
    }

    cJSON_Delete(root);
    ap_core_scan_pause(false);

    s_scan_found = found;
    s_scan_ts = (int64_t)time(NULL);
    s_scan_phase = PHASE_IDLE;
    s_scan_state = (err == ESP_OK) ? SCAN_DONE : SCAN_FAILED;
    ap_core_job_release();
    ap_events_scan_done(found);
    ESP_LOGI(TAG, "detection %s: %u rows, protocol %s (%s%s), vin %s, fp %s",
             (err == ESP_OK) ? "done" : "FAILED", found,
             seen.protocol[0] ? seen.protocol : "?",
             ap_dialect_name(dialect), j1939 ? ", J1939 network" : "",
             seen.vin[0] ? seen.vin : "(none)", fp[0] ? fp : "(none)");

    /* ephemeral tasks escape System Monitor: surface the watermark for
       the stack-audit bench (same net as the dtc job task) */
    ESP_LOGI(TAG, "std scan stack_hw=%u B",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    vTaskDelete(NULL);
}

esp_err_t autopid_std_scan_start(void)
{
    if (s_scan_state == SCAN_RUNNING)
    {
        return ESP_ERR_INVALID_STATE;
    }

    /* bus guard: a detection on a pinned protocol, or on a bus nothing
       here can read, would transmit at the wrong bitrate */
    if (!ap_guard_job_ok(s_scan_err, sizeof(s_scan_err)))
    {
        s_scan_state = SCAN_FAILED;
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* one chip job at a time (test-a-PID / dtc scan / dtc clear) */
    if (!ap_core_job_acquire())
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_scan_state = SCAN_RUNNING;
    s_scan_phase = PHASE_IDLE;
    s_scan_found = 0;
    s_scan_err[0] = '\0';

    /* INTERNAL stack: the job ends in /data writes (§2) */
    if (xTaskCreateStatic(scan_task, "apid_scan",
                          sizeof(s_scan_stack) / sizeof(StackType_t),
                          NULL, 5, s_scan_stack, &s_scan_tcb) == NULL)
    {
        s_scan_state = SCAN_FAILED;
        snprintf(s_scan_err, sizeof(s_scan_err), "task create failed");
        ap_core_job_release();
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

cJSON *ap_std_scan_status_json(void)
{
    static const char *const NAMES[] =
    {
        "idle", "running", "done", "failed",
    };
    static const char *const PHASES[] =
    {
        "idle", "protocol", "vin", "pids", "network",
    };

    cJSON *o = cJSON_CreateObject();

    if (o == NULL)
    {
        return NULL;
    }

    cJSON_AddStringToObject(o, "status", NAMES[s_scan_state]);
    cJSON_AddStringToObject(o, "phase", PHASES[s_scan_phase]);
    cJSON_AddNumberToObject(o, "found", s_scan_found);

    if (s_scan_err[0] != '\0')
    {
        cJSON_AddStringToObject(o, "error", s_scan_err);
    }

    if (s_scan_ts != 0)
    {
        cJSON_AddNumberToObject(o, "ts", (double)s_scan_ts);
    }

    size_t size = 0;

    cJSON_AddBoolToObject(o, "stored",
                          filesystem_size(AP_STD_SCAN_PATH, &size)
                                  == ESP_OK &&
                              size > 0);
    return o;
}
