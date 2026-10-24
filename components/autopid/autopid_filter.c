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
 * @file autopid_filter.c
 * @brief The ATMA filter window (Phase 4): capture ONE frame with the
 *        filter's CAN id and evaluate every enabled parameter from it.
 *
 * Flow per scheduled filter entry (poller-task context):
 *   ATCRA<id>  (COMMAND transaction: hardware-filters the monitor)
 *   claim MONITOR -> subscribe -> "ATMA" -> collect lines until a
 *   matching frame or monitor_ms elapses -> SPACE stop -> drain to the
 *   '>' prompt -> release -> unsubscribe -> ap_runner_reset() (the
 *   window changed ATCRA/monitor state under the PID runner).
 *
 * While the window is open, other chip masters' ap_be()->request()
 * fails fast with ESP_ERR_INVALID_STATE (v1 "manual" arbitration):
 * windows should be short (default 1000 ms) and scheduled sparsely.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "battery_monitor.h"
#include "obd_chip.h"

#include "autopid_transport.h"

#include "expression_parser.h"

#include "autopid_private.h"

static const char *TAG = "autopid";

#define AP_FLT_SETUP_TIMEOUT  pdMS_TO_TICKS(2000)
#define AP_FLT_CLAIM_TIMEOUT  pdMS_TO_TICKS(2000)
#define AP_FLT_DRAIN_MS       1000   /* post-stop prompt wait            */
#define AP_FLT_Q_LEN          16

static QueueHandle_t s_q;
static StaticQueue_t s_q_buf;                       /* internal object   */
static uint8_t s_q_store[AP_FLT_Q_LEN * sizeof(obd_chunk_t)]
    EXT_RAM_BSS_ATTR;

/* per-parameter captured frame: a multiplexed message carries each
   parameter's data in a DIFFERENT frame (mux page), so the window
   collects one frame per parameter instead of one per filter */
typedef struct
{
    uint8_t data[AP_PAYLOAD_MAX];
    size_t  len;
    bool    have;
} flt_slot_t;

static flt_slot_t s_slot[AP_PARAMS_PER] EXT_RAM_BSS_ATTR; /* poller task */

/** Stop the monitor and drain until the prompt. A lingering monitor
 *  poisons every later transaction (any byte terminates it and the
 *  "STOPPED" lands in someone else's response window), so retry the
 *  stop once before giving up. */
static void window_close(void)
{
    for (int attempt = 0; attempt < 2; attempt++)
    {
        if (ap_be()->monitor_stop() != ESP_OK)
        {
            ESP_LOGW(TAG, "filter: monitor stop send failed");
        }

        int64_t deadline = esp_timer_get_time() + AP_FLT_DRAIN_MS * 1000;
        obd_chunk_t c;

        while (esp_timer_get_time() < deadline)
        {
            if (xQueueReceive(s_q, &c, pdMS_TO_TICKS(50)) != pdTRUE)
            {
                continue;
            }

            for (uint16_t i = 0; i < c.len; i++)
            {
                if (c.data[i] == '>')
                {
                    return;
                }
            }
        }
    }

    ESP_LOGW(TAG, "filter: no prompt after monitor stop (x2)");
}

bool ap_runner_run_filter(const ap_filter_t *f, const ap_param_t *params)
{
    if (s_q == NULL)
    {
        s_q = xQueueCreateStatic(AP_FLT_Q_LEN, sizeof(obd_chunk_t),
                                 s_q_store, &s_q_buf);

        if (s_q == NULL)
        {
            return false;
        }
    }

    /* window setup (COMMAND transactions): headers ON so monitor lines
       carry the CAN id our parser matches, then hardware-filter to the
       one id */
    char cmd[20], resp[64];

    if (ap_be()->request("ATH1", resp, sizeof(resp),
                         AP_FLT_SETUP_TIMEOUT) != ESP_OK)
    {
        return false;
    }

    snprintf(cmd, sizeof(cmd),
             f->is_extended ? "ATCRA%08lX" : "ATCRA%03lX",
             (unsigned long)f->frame_id);

    if (ap_be()->request(cmd, resp, sizeof(resp),
                         AP_FLT_SETUP_TIMEOUT) != ESP_OK)
    {
        return false;   /* headers stay on: the CAN prelude's state */
    }

    bool got = false;
    uint8_t payload[AP_PAYLOAD_MAX];
    size_t payload_len = 0;

    float volts = 0;

    (void)battery_monitor_voltage(&volts);

    uint16_t n_prm = (f->param_count > AP_PARAMS_PER) ? AP_PARAMS_PER
                                                      : f->param_count;
    uint16_t pending = 0;

    for (uint16_t i = 0; i < n_prm; i++)
    {
        s_slot[i].have = false;

        if (params[i].enabled)
        {
            pending++;
        }
    }

    if (ap_be()->claim_monitor(
                       AP_FLT_CLAIM_TIMEOUT) != ESP_OK)
    {
        ap_runner_reset();      /* ATCRA changed under the PID runner */
        return false;
    }

    xQueueReset(s_q);

    if (ap_be()->subscribe(s_q, "autopid_flt") == ESP_OK)
    {
        if (ap_be()->send((const uint8_t *)"ATMA\r", 5) == ESP_OK)
        {
            ap_flt_stream_t lines;
            int64_t deadline = esp_timer_get_time() +
                               (int64_t)f->monitor_ms * 1000;

            ap_flt_stream_init(&lines);

            while (pending > 0 && esp_timer_get_time() < deadline)
            {
                obd_chunk_t c;

                if (xQueueReceive(s_q, &c, pdMS_TO_TICKS(50)) != pdTRUE)
                {
                    continue;
                }

                /* a chunk can carry several frames: resume mid-chunk
                   after every match (mux pages arrive back to back) */
                size_t off = 0;

                while (off < c.len && pending > 0)
                {
                    size_t used = 0;

                    if (!ap_flt_stream_feed_ex(&lines, c.data + off,
                                               c.len - off, f->frame_id,
                                               payload, sizeof(payload),
                                               &payload_len, &used))
                    {
                        break;
                    }

                    off += used;
                    got = true;

                    /* hand the frame to every still-waiting parameter */
                    for (uint16_t i = 0; i < n_prm; i++)
                    {
                        const ap_param_t *prm = &params[i];

                        if (!prm->enabled || s_slot[i].have)
                        {
                            continue;
                        }

                        if (prm->mux_expr[0] != '\0')
                        {
                            double mv = 0;

                            if (expression_parser_eval(
                                    prm->mux_expr, payload, payload_len,
                                    (double)volts, &mv) != ESP_OK ||
                                fabs(mv - (double)prm->mux_val) > 0.5)
                            {
                                continue;   /* different mux page      */
                            }
                        }

                        memcpy(s_slot[i].data, payload, payload_len);
                        s_slot[i].len = payload_len;
                        s_slot[i].have = true;
                        pending--;
                    }
                }
            }

            window_close();
        }

        ap_be()->unsubscribe(s_q);
    }

    ap_be()->release();

    /* restore the polling shape: receive-all (headers stay on, the CAN
       prelude's state since 2026-10-24) */
    (void)ap_be()->request("ATCRA", resp, sizeof(resp),
                           AP_FLT_SETUP_TIMEOUT);
    ap_runner_reset();          /* replay PID inits on the next poll    */

    if (!got)
    {
        ESP_LOGD(TAG, "filter %lX: no frame in %lu ms",
                 (unsigned long)f->frame_id,
                 (unsigned long)f->monitor_ms);
        return false;
    }

    /* evaluate every parameter that saw its frame */
    int64_t now = esp_timer_get_time();
    bool any = false;

    for (uint16_t i = 0; i < n_prm; i++)
    {
        const ap_param_t *prm = &params[i];

        if (!prm->enabled || !s_slot[i].have)
        {
            continue;
        }

        double value = 0;

        if (expression_parser_eval(prm->expression, s_slot[i].data,
                                   s_slot[i].len, (double)volts,
                                   &value) != ESP_OK)
        {
            ESP_LOGD(TAG, "filter %lX/%s: expression failed",
                     (unsigned long)f->frame_id, prm->name);
            continue;
        }

        if ((!isnan(prm->min) && value < prm->min) ||
            (!isnan(prm->max) && value > prm->max))
        {
            continue;           /* plausibility clamp, like the PID path */
        }

        ap_cache_put(f->param_start + i, value, now);
        ap_events_param(prm, f->param_start + i, f->group, value);
        any = true;
    }

    return any;
}
