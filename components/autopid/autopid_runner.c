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
 * @file autopid_runner.c
 * @brief The chip-facing runner: one PID poll end to end, init strings
 *        on type/PID transitions, ATCRA rxheader management, request,
 *        payload parse + cross-talk guard, then EVERY enabled parameter
 *        from the ONE response (fix #1) into the cache
 *        (ap_runner_publish(), autopid_publish.c); and the one-shot of
 *        test-a-PID through the same choreography.
 *
 * Owns "which type / which row the chip is currently set up for". The chip
 * baseline (the prelude, the chain sender, what the chains moved and its
 * restore) lives in autopid_baseline.c since 2026-10-20 with the memory
 * the two share (ap_base_mem). Poller-task context only (plus
 * ap_runner_reset from the scan job / settings). First contact (which car
 * is this) lives in autopid_contact.c.
 *
 * The request header is part of that memory since 2026-10-03: the standard
 * rows of a UDS-dialect car (ISO 27145 / SAE J1979-2) address their ECU
 * physically, each row's init carrying its `ATSH`. Such a row BORROWS the
 * header: before any row that does not set one itself the functional
 * header of the protocol is put back, so a custom or specific row finds
 * the same baseline as on an OBD-II car.
 *
 * Since 2026-10-20 (issue #964) the whole baseline is: every chain of the
 * tables (a type's init, a row's init, an AT command as a cmd) is read for
 * what it sets beyond the baseline (ap_chain_sets: header, receive filter,
 * extended addressing, flow control, protocol, print format, ...), and
 * before the next row TYPE the baseline goes back (ap_base_restore). A
 * custom row's `ATSH6F1;ATCEA60;ATCRA660;ATFCSM1` used to stay in effect
 * and send the standard rows that followed to the BMW gateway, which
 * answered `7F 01 11`. Within a type a row still finds what the rows
 * before it set, as it always did.
 *
 * So does a PROTOCOL a custom or specific init sets (`ATSP6`), with one
 * condition since 2026-10-05: the bus guard must allow it on this bus, when
 * it is sent and before every request that goes out on it (row_pins_ok).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include "obd_chip.h"

#include "autopid_transport.h"

#include "autopid_private.h"
#include "autopid_runner_private.h"

static const char *TAG = "autopid";

#define AP_REQ_TIMEOUT   pdMS_TO_TICKS(1500)
#define AP_INIT_TIMEOUT  pdMS_TO_TICKS(2000)

/* chip-state memory: which type/PID the chip is currently set up for */
static char s_type_init[3][AP_INIT_LEN];
static int  s_last_type = -1;
static int  s_last_pid = -1;

static const char *const TYPE_INIT_NAME[3] = { "std_init", "custom_init",
                                               "specific_init" };

void ap_runner_set_type_init(int type, const char *init)
{
    if (type >= 0 && type < 3)
    {
        snprintf(s_type_init[type], sizeof(s_type_init[type]), "%s",
                 (init != NULL) ? init : "");
        /* as a row's init: no EEPROM write, and the print-format commands
           the poller cannot live with dropped (the old `std_init` advice
           ended in ATH0) */
        ap_init_sanitize(s_type_init[type]);
    }
}

void ap_runner_reply_hint(const char *type_init, const char *init,
                          const char *rxheader)
{
    /* how a 29-bit id in the reply is read (ap_resp_set_id29_hint): the
       row's receive filter (its rxheader, else an ATCRA in its init, else
       one in its type's init), and whether the protocol in effect, the one
       a chain of the tables pinned or the baseline's, is 29-bit */
    uint32_t id = 0;
    bool ext = false;

    if (rxheader != NULL && strlen(rxheader) == 8)
    {
        id = (uint32_t)strtoul(rxheader, NULL, 16);
    }
    else if (!(ap_chain_cra_id(init, &id, &ext) && ext) &&
             !(ap_chain_cra_id(type_init, &id, &ext) && ext))
    {
        id = 0;
    }

    const ap_base_mem_t *mem = ap_base_mem();
    char proto = (mem->init_pin != '\0') ? mem->init_pin : ap_base_proto();

    ap_resp_set_id29_hint(id, ap_veh_proto_is_29bit(proto));
}

void ap_runner_reset(void)
{
    /* someone else (std scan, settings apply) changed protocol/header
       state under the chip: replay inits on the next poll */
    s_last_type = -1;
    s_last_pid = -1;
    ap_base_mem()->rxheader_set = false;
}

const char *ap_runner_type_init(int type)
{
    return (type >= 0 && type < 3) ? s_type_init[type] : "";
}

/**
 * May this row go out? The tables can pin the chip to a CAN protocol
 * themselves (`ATSP6` in a row's init, in a type's init, as a row's cmd:
 * most vehicle profiles do) and the bus guard's verdict is about the
 * protocol autopid pins, not about theirs. So every protocol the chains
 * about to be sent set must be allowed on the bus as the guard last saw it;
 * and when they set none, the one an earlier chain left the chip on must
 * still be (a bus asleep when it was sent may have woken up at another bit
 * rate). Refused = NOTHING of the row is sent: neither its protocol, nor
 * its header on the protocol in effect (an 11-bit header on a truck's
 * 29-bit protocol makes a frame the network has a meaning for), nor the
 * request. Bench 2026-10-05: a row with `ATSP6` tested on a 250 kbit/s
 * truck went out at 500, the truck's adapter was bus-off 19 ms after its
 * first error and stayed there, the chip left on protocol 6 for every poll
 * that followed.
 *
 * @p name NULL = the row under test (test-a-PID). @p type_init / @p init:
 * NULL when not due. @p pin_after: the protocol of the tables the chip sits
 * on once the chains went out ('\0' = the baseline's).
 */
static bool row_pins_ok(const char *name, int type, const char *type_init,
                        const char *init, const char *cmd, bool job,
                        char *pin_after)
{
    ap_base_mem_t *mem = ap_base_mem();
    char owner[AP_NAME_LEN + 24] = "";
    char pin = '\0';
    char last = '\0';

    if (type_init != NULL && type_init[0] != '\0')
    {
        if (!ap_guard_chain_ok(type_init,
                               (type >= 0 && type < 3) ? TYPE_INIT_NAME[type]
                                                       : "the type's init",
                               job, &last))
        {
            return false;
        }

        pin = (last != '\0') ? last : pin;
    }

    if (init != NULL && init[0] != '\0')
    {
        if (name != NULL)
        {
            snprintf(owner, sizeof(owner), "the init of row %s", name);
        }

        if (!ap_guard_chain_ok(init, (name != NULL) ? owner
                                                    : "the tested row's init",
                               job, &last))
        {
            return false;
        }

        pin = (last != '\0') ? last : pin;
    }

    if (name != NULL)
    {
        snprintf(owner, sizeof(owner), "row %s", name);
    }

    if (!ap_guard_chain_ok(cmd, (name != NULL) ? owner : "the tested row",
                           job, &last))
    {
        return false;
    }

    if (pin == '\0' && last == '\0' && mem->init_pin != '\0')
    {
        /* nothing of this row sets a protocol: its request goes out on the
           one an earlier chain left behind */
        const char standing[] = { 'A', 'T', 'T', 'P', mem->init_pin, '\0' };

        if (!ap_guard_chain_ok(standing, "an earlier row's init", job, NULL))
        {
            /* the prelude takes the chip off it, the inits are due again
               (and are judged when they are) */
            mem->baseline_sent = false;
            ap_runner_reset();
            return false;
        }

        pin = mem->init_pin;
    }

    *pin_after = (last != '\0') ? last : pin;
    return true;
}

esp_err_t ap_runner_test(int type, const char *type_init, const char *init,
                         const char *rxheader, const char *cmd, char *raw,
                         size_t raw_len, int64_t *elapsed_us,
                         char *transcript, size_t transcript_len)
{
    /* ONE-SHOT through the same chip choreography as a real poll
       (test-a-PID, §11): the baseline back when the tables moved the
       chip, the type init chain (what the poller sends when it switches
       to this PID's type), the per-PID init, ATCRA, the request, ATCRA
       off. Caller must hold the poller paused (ap_core_scan_pause) and
       this leaves the chip state dirty on purpose, unpausing runs
       ap_runner_reset(). The baseline prelude goes out first when none
       went out since the chip was last reset. httpd-task context
       (internal stack). */
    ap_base_mem_t *mem = ap_base_mem();
    char resp[64];
    ap_tr_t tr = { transcript, transcript_len, 0 };
    bool borrows = ap_base_row_borrows_header(type, init);
    char cmd_s[AP_CMD_LEN];
    char pin_after = '\0';

    if (transcript != NULL && transcript_len > 0)
    {
        transcript[0] = '\0';
    }

    snprintf(cmd_s, sizeof(cmd_s), "%s", cmd);
    ap_init_sanitize(cmd_s); /* the tested cmd can be an AT command */

    if (!mem->baseline_sent)
    {
        /* the first chip traffic of this boot (Automate off, or nothing
           polled yet), or the first after a reset: the chip sits on the
           protocol STORED in it, not on the one in effect, and its request
           went out on that one about every third time (bench 2026-10-05).
           The baseline first, as before a poll (the handler has asked the
           bus guard for it). */
        if (ap_base_send_baseline(&tr))
        {
            mem->baseline_sent = true;
        }
    }

    if (mem->moved != 0)
    {
        (void)ap_base_restore(&tr); /* as a poll of this row would */
    }
    else if (mem->hdr_borrowed && !borrows)
    {
        ap_base_header_give_back(&tr);
    }

    if (!row_pins_ok(NULL, type, type_init, init, cmd_s, true, &pin_after))
    {
        ap_tr_add(&tr, '!', "not sent: a protocol this row sets is not the "
                         "vehicle bus's (the bus guard)");
        return ESP_ERR_NOT_ALLOWED;
    }

    if (type_init != NULL && type_init[0] != '\0')
    {
        ap_base_send_init(type_init, &tr);
    }

    if (init != NULL && init[0] != '\0')
    {
        ap_base_send_init(init, &tr);
    }

    if (mem->chain_cut)
    {
        mem->chain_cut = false;
        ap_tr_add(&tr, '!', "stopped behind a reset: the vehicle was changed "
                         "while the row went out");
        return ESP_FAIL;
    }

    if (borrows)
    {
        mem->hdr_borrowed = true;
    }

    mem->init_pin = pin_after;

    bool cra_set = false;

    if (rxheader != NULL && rxheader[0] != '\0')
    {
        char cra[AP_HDR_LEN + 8];

        snprintf(cra, sizeof(cra), "ATCRA%s", rxheader);
        (void)ap_base_request_tr(cra, resp, sizeof(resp), AP_INIT_TIMEOUT,
                                 &tr);
        cra_set = true;
    }

    int64_t t0 = esp_timer_get_time();
    esp_err_t err = ap_base_request_tr(cmd_s, raw, raw_len, AP_REQ_TIMEOUT,
                                       &tr);

    mem->moved |= ap_chain_sets(cmd_s); /* an AT command as the cmd */

    if (ap_guard_cmd_resets(cmd_s, strlen(cmd_s)))
    {
        mem->baseline_sent = false; /* the chip is on its stored protocol */
    }

    if (elapsed_us != NULL)
    {
        *elapsed_us = esp_timer_get_time() - t0;
    }

    if (cra_set)
    {
        (void)ap_base_request_tr("ATCRA", resp, sizeof(resp),
                                 AP_INIT_TIMEOUT, &tr);
    }

    return err;
}

bool ap_runner_run(const ap_pid_t *pid, int pid_index,
                   const ap_param_t *params)
{
    ap_base_mem_t *mem = ap_base_mem();

    if (!ap_runner_baseline_ensure())
    {
        /* the protocol in effect changed since this pass asked the bus
           guard: nothing is sent, the next pass asks again */
        return false;
    }

    bool borrows = ap_base_row_borrows_header(pid->type, pid->init);
    bool type_due = (pid->type != s_last_type);
    bool row_due = type_due || pid_index != s_last_pid;
    char pin_after = '\0';

    if (type_due && mem->moved != 0)
    {
        /* a chain of the tables moved the chip off the baseline (a custom
           row's ATSH / ATCEA / ATCRA, issue #964): the next row type
           starts from the baseline again, its own inits on top */
        if (!ap_base_restore(NULL))
        {
            return false; /* no verdict for the baseline: the next pass */
        }
    }
    else if (mem->hdr_borrowed && !borrows && row_due)
    {
        /* a standard row that set its own header gives the functional one
           back before a row that does not set one (the file header) */
        ap_base_header_give_back(NULL);
    }

    /* the protocols the tables set themselves, before anything of the row
       is sent */
    if (!row_pins_ok(pid->name, pid->type,
                     type_due ? s_type_init[pid->type] : NULL,
                     row_due ? pid->init : NULL, pid->cmd, false, &pin_after))
    {
        ESP_LOGD(TAG, "%s: not sent (the bus guard)", pid->name);
        return false;
    }

    /* type init once per type transition (legacy behavior kept) */
    if (pid->type != s_last_type)
    {
        if (s_type_init[pid->type][0] != '\0')
        {
            ap_base_send_init(s_type_init[pid->type], NULL);
        }

        s_last_type = pid->type;
        s_last_pid = -1; /* force per-PID setup after a type switch */
    }

    if (pid_index != s_last_pid)
    {
        if (pid->init[0] != '\0')
        {
            ap_base_send_init(pid->init, NULL);
        }

        if (borrows)
        {
            mem->hdr_borrowed = true;
        }

        char resp[64];

        if (pid->rxheader[0] != '\0')
        {
            char cra[AP_HDR_LEN + 8];

            snprintf(cra, sizeof(cra), "ATCRA%s", pid->rxheader);
            (void)ap_be()->request(cra, resp, sizeof(resp),
                                   AP_INIT_TIMEOUT);
            mem->rxheader_set = true;
        }
        else if (mem->rxheader_set)
        {
            (void)ap_be()->request("ATCRA", resp, sizeof(resp),
                                   AP_INIT_TIMEOUT);
            mem->rxheader_set = false;
        }

        s_last_pid = pid_index;
    }

    if (mem->chain_cut)
    {
        /* a reset in an init found the guard without a verdict: the chip is
           on its stored protocol, nothing goes out (the next pass asks) */
        mem->chain_cut = false;
        ap_runner_reset();
        return false;
    }

    mem->init_pin = pin_after;

    static char resp[AP_RESP_MAX] EXT_RAM_BSS_ATTR; /* poller-task only */

    esp_err_t err = ap_be()->request(pid->cmd, resp, sizeof(resp),
                                     AP_REQ_TIMEOUT);

    mem->moved |= ap_chain_sets(pid->cmd); /* an AT command as the cmd */

    if (ap_guard_cmd_resets(pid->cmd, strlen(pid->cmd)))
    {
        mem->baseline_sent = false; /* the chip is on its stored protocol */
    }

    if (err != ESP_OK)
    {
        ESP_LOGD(TAG, "%s: request failed (%s)", pid->name,
                 esp_err_to_name(err));
        return false;
    }

    uint8_t payload[AP_PAYLOAD_MAX];
    size_t payload_len = 0;

    ap_runner_reply_hint(s_type_init[pid->type], pid->init, pid->rxheader);

    if (ap_resp_to_payload(resp, payload, sizeof(payload), &payload_len)
            != ESP_OK)
    {
        ESP_LOGD(TAG, "%s: no payload in response", pid->name);
        return false;
    }

    /* cross-talk guard: another chip master's response (WS-OBD bridge,
       CLI) can land in our request window, never cache it (Phase 1b) */
    if (!ap_payload_matches_cmd(pid->cmd, payload, payload_len))
    {
        ESP_LOGD(TAG, "%s: response echo mismatch (cross-talk?)",
                 pid->name);
        return false;
    }

    /* the bytes the row's expressions index: the reply as the chip
       printed it, headers on (the legacy frame of reference, the published
       profiles'; 2026-10-24, issue #1014). Poller-task only. */
    static uint8_t s_frames[AP_FRAMES_MAX] EXT_RAM_BSS_ATTR;
    size_t n_frames = 0;

    if (ap_resp_frames(resp, s_frames, sizeof(s_frames), &n_frames) != ESP_OK)
    {
        ESP_LOGD(TAG, "%s: no frames in response", pid->name);
        return false;
    }

    return ap_runner_publish(pid, params, s_frames, n_frames,
                             esp_timer_get_time());
}
