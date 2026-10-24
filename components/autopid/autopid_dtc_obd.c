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
 * @file autopid_dtc_obd.c
 * @brief DTC scan, the OBD path (TASK_dtc.md §5, §14): the chip
 *        choreography around a functional scan (dtc_init, headers on,
 *        ATCRA), the mode 03/07/0A code request with its multi-ECU merge
 *        and the mode 02 freeze frame. Split out of autopid_dtc.c
 *        2026-10-02 (700-line rule); behaviour unchanged.
 */
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "autopid.h"
#include "autopid_transport.h"
#include "autopid_private.h"
#include "autopid_dtc_engine.h"

static const char *TAG = "autopid";

/* ---- chip choreography ----------------------------------------------------------- */

static bool s_init_pinned;  /* dtc_init set a protocol: the poller's
                               baseline is due after the job */

bool ap_dtc_obd_init_ok(void)
{
    /* the manufacturer-UDS path and a J1939-only vehicle never send it */
    if (ap_dtc_cfg()->proto == AP_DTC_PROTO_UDS ||
        autopid_vehicle_dialect() == AP_DIALECT_J1939)
    {
        return true;
    }

    return ap_guard_chain_ok(ap_dtc_cfg()->init, "dtc_init", true, NULL);
}

/** Optional prep: dtc_init commands (';'-separated) + ATCRA rxheader.
 *  Best-effort: a failed init command doesn't abort the scan. The job
 *  asked ap_dtc_obd_init_ok() when it started. */
void ap_dtc_obd_prep(char *resp, size_t resp_len)
{
    const char *p = ap_dtc_cfg()->init;
    char pin = '\0';

    (void)ap_guard_chain_allowed(NULL, p, &pin, NULL);
    s_init_pinned = (pin != '\0');

    while (*p != '\0')
    {
        const char *sep = strchr(p, ';');
        size_t len = (sep != NULL) ? (size_t)(sep - p) : strlen(p);
        char one[AP_INIT_LEN];

        if (len > 0 && len < sizeof(one))
        {
            memcpy(one, p, len);
            one[len] = '\0';
            ap_init_sanitize(one); /* spare the chip's EEPROM; an ATH0 or
                                      ATS0 is dropped (an empty piece) */

            if (one[0] != '\0')
            {
                (void)ap_be()->request(one, resp, resp_len,
                                       AP_DTC_INIT_TIMEOUT);
            }

            if (ap_guard_cmd_resets(one, strlen(one)))
            {
                /* the chip is back on its stored protocol: the one in
                   effect (the job asked the guard for it) goes out again */
                ap_runner_send_prelude(ap_std_prelude());
            }
        }

        p += len + ((sep != NULL) ? 1 : 0);
    }

    /* headers ON for the scan window (after the user's init so it can't
       be undone by accident; the CAN prelude has them on since 2026-10-24,
       a K-line one off): responses carry CAN ids, which is what lets a
       functional scan keep EVERY responding ECU apart */
    (void)ap_be()->request("ATH1", resp, resp_len, AP_DTC_INIT_TIMEOUT);

    if (ap_dtc_cfg()->rxheader[0] != '\0')
    {
        char cra[AP_HDR_LEN + 8];

        snprintf(cra, sizeof(cra), "ATCRA%s", ap_dtc_cfg()->rxheader);
        (void)ap_be()->request(cra, resp, resp_len, AP_DTC_INIT_TIMEOUT);
    }
}

void ap_dtc_obd_done(char *resp, size_t resp_len)
{
    /* the chip may sit on dtc_init's protocol, and headers went on for the
       scan (the prelude of a K-line protocol keeps them off): the poller
       starts from its own prelude again */
    s_init_pinned = false;
    ap_runner_baseline_invalidate();

    if (ap_dtc_cfg()->rxheader[0] != '\0')
    {
        (void)ap_be()->request("ATCRA", resp, resp_len, AP_DTC_INIT_TIMEOUT);
    }
}

/* one per-ECU assembly buffer: scan/clear jobs run one at a time */
static ap_resp_ecu_t s_ecus[AP_RESP_ECUS_MAX] EXT_RAM_BSS_ATTR;

ap_resp_ecu_t *ap_dtc_obd_ecus(void)
{
    return s_ecus;
}

/** Request @p cmd and parse its DTC list from EVERY responding ECU
 *  (functional scans hit several; codes are union-merged, duplicates
 *  dropped). A request/parse failure counts as zero codes (mode 07/0A
 *  on a no-pending ECU answers NO DATA; the 0101 probe already proved
 *  an ECU is alive). Cross-talk and negative responses too. */
uint8_t ap_dtc_obd_codes(const char *cmd, uint8_t svc,
                         char out[][AP_DTC_CODE_LEN], ap_dtc_report_t *r,
                         ap_dtc_kind_t kind, char *resp, size_t resp_len)
{
    /* one chip job at a time: PSRAM, not half a kilobyte of whoever's
       stack runs the clear (the httpd task) */
    static char codes[AP_DTC_MAX][AP_DTC_CODE_LEN] EXT_RAM_BSS_ATTR;

    if (ap_be()->request(cmd, resp, resp_len, AP_DTC_REQ_TIMEOUT) != ESP_OK)
    {
        return 0;
    }

    int n_ecu = ap_resp_to_payloads(resp, s_ecus, AP_RESP_ECUS_MAX);
    uint8_t n = 0;

    for (int e = 0; e < n_ecu; e++)
    {
        if (!ap_payload_matches_cmd(cmd, s_ecus[e].payload,
                                    s_ecus[e].len))
        {
            continue;
        }

        int c = ap_dtc_parse_codes(s_ecus[e].payload, s_ecus[e].len,
                                   svc, codes, AP_DTC_MAX);

        if (c > 0 && out != NULL)
        {
            n = ap_dtc_merge_codes(out, n, AP_DTC_MAX, codes,
                                   (uint8_t)c);
        }

        for (int i = 0; i < c && r != NULL; i++)
        {
            /* services 03 / 07 / 0A carry no status byte */
            (void)ap_dtc_report_add(r, kind, codes[i], s_ecus[e].header, 0,
                                    0);
        }
    }

    return n;
}

/* ---- freeze frame (mode 02 frame 0: TASK_dtc §14) --------------------------------- */

/* Curated value PIDs: the classic freeze-frame set, bounded scan cost.
 * The supported-PID bitmaps (02 00/20/40) prune this to what the ECU
 * actually stores; a silent bitmap falls back to blind probing (an
 * unsupported PID just answers NO DATA = zero decoded values). */
static const uint8_t FRZ_PIDS[] =
{
    0x04, 0x05, 0x06, 0x07, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10,
    0x11, 0x2F, 0x33, 0x42, 0x46,
};

/** The freeze ECU's payload from the last request, or NULL. Headers are
 *  ON during the scan window, so responders are told apart by CAN id;
 *  UINT32_MAX (headers off despite ATH1) accepts the first entry. */
static const ap_resp_ecu_t *frz_ecu_payload(int n_ecu, uint32_t want)
{
    for (int e = 0; e < n_ecu; e++)
    {
        if (want == UINT32_MAX || s_ecus[e].header == want ||
            s_ecus[e].header == UINT32_MAX)
        {
            return &s_ecus[e];
        }
    }

    return NULL;
}

/** Capture frame 0 into @p r: DTCFRZF (which DTC froze it, and which
 *  ECU answered: the functional query can hit several; first responder
 *  with a frame wins, multi-ECU freeze = v2), then the curated PIDs
 *  decoded via the standard table. Best-effort throughout: a scan
 *  never fails because the freeze frame is unreadable. */
void ap_dtc_obd_freeze(ap_dtc_report_t *r, char *resp, size_t resp_len)
{
    if (ap_be()->request("020200", resp, resp_len, AP_DTC_REQ_TIMEOUT) !=
        ESP_OK)
    {
        return;
    }

    int n_ecu = ap_resp_to_payloads(resp, s_ecus, AP_RESP_ECUS_MAX);

    for (int e = 0; e < n_ecu && !r->frz_present; e++)
    {
        if (ap_frz_dtc(s_ecus[e].payload, s_ecus[e].len, r->frz_dtc))
        {
            r->frz_present = true;
            r->frz_ecu = s_ecus[e].header;
        }
    }

    if (!r->frz_present)
    {
        return;         /* codes stored but no frame (or NRC): fine */
    }

    /* supported bitmaps for the ranges the curated list spans */
    uint32_t bm[3] = { 0, 0, 0 };
    bool bm_ok[3] = { false, false, false };

    for (int g = 0; g < 3; g++)
    {
        char cmd[8];

        snprintf(cmd, sizeof(cmd), "02%02X00", g * 0x20);

        if (ap_be()->request(cmd, resp, resp_len, AP_DTC_REQ_TIMEOUT) !=
            ESP_OK)
        {
            continue;
        }

        n_ecu = ap_resp_to_payloads(resp, s_ecus, AP_RESP_ECUS_MAX);

        const ap_resp_ecu_t *ecu = frz_ecu_payload(n_ecu, r->frz_ecu);

        if (ecu != NULL)
        {
            bm_ok[g] = ap_frz_bitmap(ecu->payload, ecu->len,
                                     (uint8_t)(g * 0x20), &bm[g]);
        }
    }

    for (size_t i = 0;
         i < sizeof(FRZ_PIDS) && r->n_frz < AP_FRZ_MAX; i++)
    {
        uint8_t pid = FRZ_PIDS[i];
        int g = (pid - 1) / 0x20;

        /* bitmap bit: MSB of byte 0 = PID base+1 */
        if (bm_ok[g] &&
            ((bm[g] >> (31 - ((pid - 1) % 0x20))) & 1u) == 0)
        {
            continue;
        }

        char cmd[8];

        snprintf(cmd, sizeof(cmd), "02%02X00", pid);

        if (ap_be()->request(cmd, resp, resp_len, AP_DTC_REQ_TIMEOUT) !=
            ESP_OK)
        {
            continue;
        }

        n_ecu = ap_resp_to_payloads(resp, s_ecus, AP_RESP_ECUS_MAX);

        const ap_resp_ecu_t *ecu = frz_ecu_payload(n_ecu, r->frz_ecu);

        if (ecu != NULL && ecu->len > 1 && ecu->payload[1] == pid)
        {
            r->n_frz = (uint8_t)ap_frz_decode(ecu->payload, ecu->len,
                                              r->frz, r->n_frz,
                                              AP_FRZ_MAX);
        }
    }

    ESP_LOGI(TAG, "dtc freeze frame: %s from %lX, %u values",
             r->frz_dtc, (unsigned long)r->frz_ecu, r->n_frz);
}
