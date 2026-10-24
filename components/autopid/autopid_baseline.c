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
 * @file autopid_baseline.c
 * @brief The chip baseline, split out of autopid_runner.c (2026-10-20,
 *        700-line rule): the prelude (spaces on, headers off, timeout, the
 *        protocol, the functional header, the receive filter cleared), the
 *        chain sender with its reset rule, and the memory the poll and the
 *        test share (ap_base_mem): what the chip is set up for.
 *
 * Two things are owed back to the baseline:
 *
 * - the request header a standard row of a UDS-dialect car borrows with
 *   its own `ATSH` (2026-10-03): the functional header goes back before
 *   any row that does not set one (ap_base_header_give_back);
 * - everything a chain of the tables moves (2026-10-20, issue #964): a
 *   custom row's `ATSH6F1;ATCEA60;ATCRA660;ATFCSH6F1;ATFCSD60300000;ATFCSM1`
 *   stayed in effect, every standard row that followed went to the BMW
 *   gateway, the gateway answered `7F 01 11` and the values froze (bench
 *   2026-10-20: nine standard rows frozen from the second cycle on, 27
 *   service-01 requests at the gateway's address). Every chain is read for
 *   what it sets beyond the baseline (ap_chain_sets) and before the next
 *   row TYPE the baseline goes back (ap_base_restore: the prelude plus one
 *   undo command per thing the prelude does not cover). Within a type a
 *   row still finds what the rows before it set, as it always did.
 *
 * Poller-task context, plus the httpd task for test-a-PID while the poller
 * is paused (the chip job serializes them).
 */
#include "autopid_runner_private.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "autopid_transport.h"

static const char *TAG = "autopid";

#define AP_INIT_TIMEOUT  pdMS_TO_TICKS(2000)

static ap_base_mem_t s_mem;

ap_base_mem_t *ap_base_mem(void)
{
    return &s_mem;
}

esp_err_t ap_base_request_tr(const char *cmd, char *resp, size_t resp_len,
                             TickType_t timeout, ap_tr_t *tr)
{
    resp[0] = '\0';
    ap_tr_add(tr, '>', cmd);

    esp_err_t err = ap_be()->request(cmd, resp, resp_len, timeout);

    ap_tr_add(tr, '<', (err != ESP_OK) ? "(no reply)"
                    : (resp[0] == '\0') ? "(empty)" : resp);
    return err;
}

char ap_base_proto(void)
{
    /* as ap_std_prelude() would: the setting, else the current car's
       record, else the search */
    return ap_veh_effective_protocol(ap_core_std_protocol(),
                                     autopid_vehicle_protocol(),
                                     ap_runner_proto_fallback());
}

/** The prelude went out: nothing is borrowed or moved after it. */
static void prelude_out(void)
{
    s_mem.hdr_borrowed = false;
    s_mem.init_pin = '\0';     /* the chip is on the prelude's protocol */
    s_mem.moved = 0;
    s_mem.rxheader_set = false; /* its ATCRA cleared the filter */
}

/**
 * A reset inside a chain of the tables (`ATZ`, `ATD`, `ATWS`) leaves the chip
 * on the protocol stored in its EEPROM, the last vehicle DETECTED, and its
 * first request goes out on that whatever bus the device is plugged into
 * (bench 2026-10-05: the 500 kbit/s car's on the 250 kbit/s truck, bus-off
 * 17 ms after the first error). So the baseline prelude goes out right
 * behind the reset and the rest of the chain builds on it.
 * @return false = the bus guard has no verdict for that protocol right now
 * (another car made current under the row): the chain is cut.
 */
static bool after_reset(ap_tr_t *tr)
{
    char proto = ap_base_proto();

    if (!ap_guard_pin_ok(proto))
    {
        s_mem.baseline_sent = false;
        s_mem.chain_cut = true;
        return false;
    }

    ap_base_send_chain(ap_veh_prelude_for(proto), tr);
    prelude_out();
    return true;
}

void ap_base_send_chain(const char *chain, ap_tr_t *tr)
{
    char cmd[AP_INIT_LEN];
    size_t n = 0;

    for (const char *p = chain;; p++)
    {
        if (*p == ';' || *p == '\0')
        {
            if (n > 0)
            {
                cmd[n] = '\0';
                ap_init_sanitize(cmd); /* spare the chip's EEPROM */

                char resp[64];

                (void)ap_base_request_tr(cmd, resp, sizeof(resp),
                                         AP_INIT_TIMEOUT, tr);

                if (ap_guard_cmd_resets(cmd, n) && !after_reset(tr))
                {
                    return;
                }

                n = 0;
            }

            if (*p == '\0')
            {
                break;
            }
        }
        else if (n < sizeof(cmd) - 1)
        {
            cmd[n++] = *p;
        }
    }
}

void ap_base_send_init(const char *chain, ap_tr_t *tr)
{
    ap_base_send_chain(chain, tr);
    s_mem.moved |= ap_chain_sets(chain);
}

/**
 * Send the baseline prelude, when the bus guard's verdict covers the
 * protocol it pins. The protocol is read ONCE and the prelude built from
 * that reading: the current car can change under this task (an activation
 * from the UI, in the HTTP task), and a prelude for the new car's protocol
 * sent on the strength of the old car's verdict transmits at the wrong bit
 * rate on a live bus (bench 2026-10-05: a 500 kbit/s car activated on a 250
 * kbit/s truck; the truck's adapter was bus-off 40 ms later).
 */
bool ap_base_send_baseline(ap_tr_t *tr)
{
    char proto = ap_base_proto();

    if (!ap_guard_pin_ok(proto))
    {
        ESP_LOGD(TAG, "baseline for protocol %c waits for the bus guard",
                 proto);
        s_mem.baseline_sent = false;
        return false;
    }

    ap_base_send_chain(ap_veh_prelude_for(proto), tr);
    prelude_out();
    return true;
}

void ap_runner_restore_baseline(void)
{
    /* the app's ATZ/ATS0/ATH0/ATSH/ATCRA are all still in effect: put
       back what the parser and the std PIDs assume (the same prelude
       the std scan uses - ATTP not ATSP, so no EEPROM write), then let
       the next poll replay the configured type/PID inits on top. A
       prelude the guard has no verdict for is left to the next poll. */
    (void)ap_base_send_baseline(NULL);
    ap_runner_reset();
}

void ap_runner_send_prelude(const char *prelude)
{
    ap_base_send_chain(prelude, NULL);
    prelude_out();
}

void ap_runner_send_init(const char *init)
{
    ap_base_send_init(init, NULL);
}

bool ap_runner_baseline_ensure(void)
{
    /* boot baseline, once: spaces/headers/timeout + the protocol the
       setting (or the current car's record under "0") names, BEFORE any
       user init so the inits still win. Without it the chip polled on
       whatever its EEPROM held until the first ELM-app pause
       (2026-10-01). Re-armed by ap_runner_rebaseline() on a car switch. */
    if (!s_mem.baseline_sent)
    {
        if (!ap_base_send_baseline(NULL))
        {
            return false;
        }

        s_mem.baseline_sent = true;
    }

    return true;
}

void ap_runner_baseline_invalidate(void)
{
    s_mem.baseline_sent = false;
}

bool ap_base_row_borrows_header(int type, const char *init)
{
    return type == AP_PID_STD && ap_dialect_init_sets_header(init);
}

void ap_base_header_give_back(ap_tr_t *tr)
{
    char proto = ap_base_proto();
    const char *func = ap_veh_func_header(proto);

    if (func != NULL)
    {
        char resp[64];

        (void)ap_base_request_tr(func, resp, sizeof(resp), AP_INIT_TIMEOUT,
                                 tr);
        s_mem.moved &= ~AP_SETS_HEADER;
    }
    else if (ap_guard_pin_ok(proto))
    {
        /* the chip's search (it picks its own headers), or a protocol
           without a functional header here: the whole prelude, which pins
           that protocol (hence the guard's word, as for the baseline) */
        ap_base_send_chain(ap_veh_prelude_for(proto), tr);
        prelude_out();
    }

    s_mem.hdr_borrowed = false;
}

bool ap_base_restore(ap_tr_t *tr)
{
    char proto = ap_base_proto();

    if (!ap_guard_pin_ok(proto))
    {
        ESP_LOGD(TAG, "baseline restore for protocol %c waits for the bus "
                      "guard", proto);
        return false;
    }

    char chain[AP_RESTORE_LEN];

    (void)ap_chain_restore(s_mem.moved, ap_veh_prelude_for(proto), chain,
                           sizeof(chain));
    ESP_LOGD(TAG, "the tables moved the chip (0x%x): baseline back: %s",
             s_mem.moved, chain);
    ap_base_send_chain(chain, tr);
    prelude_out();
    return true;
}
