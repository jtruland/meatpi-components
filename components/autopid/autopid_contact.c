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
 * @file autopid_contact.c
 * @brief First contact, once per boot: which car is this, and how does it
 *        want to be asked (TASK_quick_setup.md second pass; dialects:
 *        TASK_j1939_wwh.md phase 3). Split out of autopid_runner.c
 *        2026-10-03 (700-line rule). Poller-task context.
 *
 * The boot prelude pins the protocol the setting names, or under "0" the
 * one the vehicle store learned for the current car. After the first
 * SUCCESSFUL poll the identity requests (autopid_identify.c) go out and the
 * store decides: the same car, another stored car (switch), or an unknown
 * one (the detection job). A stored protocol that stays silent for the
 * first AP_BOOT_SILENT_POLLS polls gives way to the chip's own search.
 *
 * The polls only make contact when the tables hold rows this car answers.
 * They do not when the tables are empty, and they do not when the device
 * was moved between an OBD-II car and a UDS-dialect one (ISO 27145 /
 * SAE J1979-2), whose rows mean nothing to each other. So while there is no
 * contact a probe goes out every AP_PROBE_GAP_US (doubling up to
 * AP_PROBE_GAP_MAX_US while nothing answers):
 *
 *   a protocol named by the     the bitmap request of the stored car's
 *   setting or the store        dialect; under a PINNED setting the other
 *                               dialect too (there is no search to fall
 *                               back to)
 *   no such protocol, or it     a walk over the ISO 15765-4 protocols the
 *   stayed silent               bus guard allows for this bus, `0100` and
 *                               `22F400` on each (ap_dialect_can_candidates
 *                               has the reasons: the chip's own search
 *                               cannot find a UDS-dialect vehicle and takes
 *                               9.3 s to say so)
 *
 * A J1939 network (phase 5) is heard, not asked: once the chip's probe found
 * nobody, the listener's sources and VIN are the sighting (j1939_contact). A
 * stored J1939 vehicle gives the listener a ten second head start, so a
 * truck's boot sends no OBD request at all; the VIN broadcast gets the same
 * time before an identity without it, and one heard later is still adopted
 * (j1939_vin_watch).
 *
 * Never touches flash: the store hands every write to its worker.
 */
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include "autopid_transport.h"
#include "autopid_private.h"

static const char *TAG = "autopid";

#define AP_BOOT_SILENT_POLLS 3
#define AP_IDENT_TRIES       3                   /* answers without an id */
#define AP_IDENT_TIMEOUT     pdMS_TO_TICKS(6000) /* the chip's search     */
#define AP_AT_TIMEOUT        pdMS_TO_TICKS(2000)
#define AP_PROBE_GAP_US      (10LL * 1000 * 1000)
#define AP_PROBE_GAP_MAX_US  (80LL * 1000 * 1000)

static bool    s_ident_done;       /* the per-boot identity check ran    */
static uint8_t s_ident_tries;      /* checks that found no identity      */
static uint8_t s_boot_fail_streak; /* failed polls before the 1st success */
static bool    s_proto_fallback;   /* stored protocol silent: ATTP0 now  */
static int64_t s_probe_last_us;    /* contact probe pacing               */
static int64_t s_probe_gap_us = AP_PROBE_GAP_US;
static char    s_resp[AP_RESP_MAX] EXT_RAM_BSS_ATTR; /* poller only      */

static esp_err_t ask(const char *cmd, TickType_t timeout)
{
    s_resp[0] = '\0';
    return ap_be()->request(cmd, s_resp, sizeof(s_resp), timeout);
}

bool ap_runner_proto_fallback(void)
{
    return s_proto_fallback;
}

/** The protocol the prelude pins right now ('0' = the chip's search). */
static char protocol_in_effect(void)
{
    return ap_veh_effective_protocol(ap_core_std_protocol(),
                                     autopid_vehicle_protocol(),
                                     s_proto_fallback);
}

static bool setting_pinned(void)
{
    const char *setting = ap_core_std_protocol();

    return setting[0] >= '6' && setting[0] <= '9';
}

/** True while the chip prelude pins the protocol LEARNED into the store
 *  (setting "0" + a current car with a protocol, not yet fallen back). */
static bool stored_protocol_in_use(void)
{
    return !setting_pinned() && protocol_in_effect() != '0';
}

/** The dialect the store expects on this bus (OBD-II when there is no
 *  current car, or one that is not asked by requests). */
static ap_dialect_t car_dialect(void)
{
    ap_dialect_t d = autopid_vehicle_dialect();

    return ap_dialect_has_requests(d) ? d : AP_DIALECT_OBD2;
}

static ap_dialect_t other_dialect(ap_dialect_t d)
{
    return (d == AP_DIALECT_UDS) ? AP_DIALECT_OBD2 : AP_DIALECT_UDS;
}

void ap_runner_rebaseline(void)
{
    ap_runner_baseline_invalidate();
    s_proto_fallback = false;
    s_probe_gap_us = AP_PROBE_GAP_US;
    ap_guard_rearm(); /* another protocol: the bus guard looks again */
}

void ap_runner_force_search(void)
{
    /* the stored protocol is not proven on this bus (or contradicted by
       it): the next prelude carries ATTP0 (the chip's search is
       bus-safe). Once: the guard asks before every transmission, and a
       prelude per poll would restart the search each time. */
    if (!s_proto_fallback)
    {
        s_proto_fallback = true;
        ap_runner_baseline_invalidate();
    }
}

/* ---- the identity check ---------------------------------------------------- */

/**
 * Ask who this is and let the store decide. @p dialect is the one that
 * just answered (a poll: the stored car's); @p found_proto is the pinned
 * protocol a contact probe found the car on ('\0' = the one in effect).
 * Sends the prelude first so the reply set is deterministic (functional
 * header, CRA cleared, the print format) whatever the last PID left behind; the
 * next poll replays its inits (ap_runner_reset).
 */
static void identity_check(ap_dialect_t dialect, char found_proto)
{
    static ap_veh_seen_t seen EXT_RAM_BSS_ATTR; /* poller-task only      */

    /* one chip job at a time: a std/DTC scan or test-a-PID that is just
       starting wins; we retry on the next successful poll */
    if (!ap_core_job_acquire())
    {
        return;
    }

    memset(&seen, 0, sizeof(seen));

    if (found_proto != '\0')
    {
        seen.protocol[0] = found_proto;
    }
    else if (protocol_in_effect() == '0' &&
             ask("ATDPN", AP_AT_TIMEOUT) == ESP_OK)
    {
        /* in search mode the chip knows the protocol NOW (a poll just got
           its answer on it); the prelude below sends ATTP0 again, after
           which ATDPN reads "A0" until the next request went out. Asked
           after the prelude, as it was until 2026-10-03, the store never
           learned a re-detected protocol and searched again on every
           boot. Asked on every search, not only after a fallback: a
           device with an empty store searches too. */
        (void)ap_veh_parse_dpn(s_resp, seen.protocol);
    }

    /* ... and with the protocol known, the identity requests go out on
       it: a second search costs 7 s on a quiet bus (0902 would time out) */
    char proto = (seen.protocol[0] != '\0') ? seen.protocol[0]
                                            : protocol_in_effect();

    if (seen.protocol[0] == '\0' && !ap_guard_pin_ok(proto))
    {
        /* the protocol in effect changed while the poll was out (another
           car made current from the UI): no identity request on a protocol
           the bus guard has not ruled on. The next answered poll asks. */
        ap_core_job_release();
        return;
    }

    /* the prelude of the protocol read above, not a second reading */
    ap_runner_send_prelude(ap_veh_prelude_for(proto));

    /* a car that answered a poll but not its dialect's identity requests
       may be another car speaking the other dialect on the same protocol
       (the poll was a row both understand) */
    if (!ap_identify(dialect, proto, false, &seen, s_resp, sizeof(s_resp)))
    {
        (void)ap_identify(other_dialect(dialect), proto, false, &seen,
                          s_resp, sizeof(s_resp));
    }

    ap_runner_reset();
    ap_core_job_release();

    ESP_LOGI(TAG, "vehicle identity: vin %s, %u ECUs, %s%s",
             seen.vin[0] ? seen.vin : "(none)", (unsigned)seen.n_ecus,
             ap_dialect_name((ap_dialect_t)seen.dialect),
             seen.protocol[0] ? " (protocol re-detected)" : "");

    switch (autopid_vehicle_seen(&seen))
    {
        case AP_VEH_RES_NONE:
            /* case D: the ECU answered a poll but not the identity
               requests; try again on a later success, a few times */
            if (++s_ident_tries >= AP_IDENT_TRIES)
            {
                s_ident_done = true;
            }
            break;

        case AP_VEH_RES_SAME:
            s_ident_done = true;

            if (seen.protocol[0] != '\0')
            {
                /* the chip found the car on its own: the store holds
                   THAT protocol now, let the baseline pin it */
                s_proto_fallback = false;
                ap_runner_restore_baseline();
            }
            break;

        case AP_VEH_RES_SWITCHED:
            /* the store set the other car current (its files follow on
               the worker): pin that car's protocol from here on */
            s_ident_done = true;
            s_proto_fallback = false;
            ap_runner_restore_baseline();
            break;

        case AP_VEH_RES_NEW:
            /* unknown car: the detection job stores it (protocol, VIN,
               standard PIDs) and pauses polling once; a job that cannot
               start now (another chip job) is retried on the next poll.
               What was just found saves the job its own search. */
            ap_std_scan_hint(seen.protocol[0],
                             (ap_dialect_t)seen.dialect);
            s_ident_done = (autopid_std_scan_start() == ESP_OK);
            break;
    }
}

/* ---- contact probes -------------------------------------------------------- */

/** Does anything answer the bitmap request of range 00 in @p dialect on
 *  what the chip is set to now? */
static bool alive(ap_dialect_t dialect)
{
    char cmd[12];
    ap_veh_ecu_t ecus[AP_VEH_ECUS_MAX];

    return ap_dialect_pid_cmd(dialect, 0x00, cmd, sizeof(cmd)) > 0 &&
           ask(cmd, AP_IDENT_TIMEOUT) == ESP_OK &&
           ap_dialect_bitmaps(dialect, s_resp, 0x00, ecus,
                              AP_VEH_ECUS_MAX) > 0;
}

/** A failed poll or probe on a protocol the store named: after
 *  AP_BOOT_SILENT_POLLS of them it gives way to the chip's search. */
static void note_silence(void)
{
    if (s_boot_fail_streak < UINT8_MAX)
    {
        s_boot_fail_streak++;
    }

    if (s_boot_fail_streak == AP_BOOT_SILENT_POLLS &&
        stored_protocol_in_use())
    {
        /* another car, or a stale record: let the chip search for
           the rest of this boot (first contact re-detects) */
        s_proto_fallback = true;
        ESP_LOGW(TAG, "stored protocol %s: no answer in %d polls, "
                      "using the chip's protocol search this boot",
                 autopid_vehicle_protocol(), AP_BOOT_SILENT_POLLS);
        ap_runner_restore_baseline();
    }
}

/** One round of looking for the car (see the file header). */
static void contact_probe(void)
{
    ap_dialect_t car = car_dialect();

    if (!ap_runner_baseline_ensure())
    {
        return; /* the bus guard has not ruled for this protocol yet: the
                   next pass asks it, and probes then */
    }

    s_probe_last_us = esp_timer_get_time();

    if (protocol_in_effect() != '0')
    {
        if (alive(car))
        {
            identity_check(car, '\0');
            return;
        }

        if (setting_pinned() && alive(other_dialect(car)))
        {
            identity_check(other_dialect(car), '\0');
            return;
        }

        note_silence();
        return;
    }

    /* nobody named a protocol that answers: the pinned protocols this
       bus allows, both dialects on each. The guard looks again before
       each one (a bus asleep at boot may have woken up at another bitrate
       since the last look). */
    char cand[AP_DIALECT_CAND_LEN];
    ap_bus_t bus;

    ap_guard_bus(&bus);

    int n = ap_dialect_can_candidates(&bus, cand);
    bool sent = false;

    for (int i = 0; i < n; i++)
    {
        if (!ap_guard_candidate_ok(cand[i]))
        {
            continue;
        }

        ap_runner_send_prelude(ap_veh_prelude_for(cand[i]));
        sent = true;

        bool obd2 = alive(AP_DIALECT_OBD2);

        if (obd2 || alive(AP_DIALECT_UDS))
        {
            ap_dialect_t d = obd2 ? AP_DIALECT_OBD2 : AP_DIALECT_UDS;

            ESP_LOGI(TAG, "contact: a vehicle answers on protocol %c (%s)",
                     cand[i], ap_dialect_name(d));
            identity_check(d, cand[i]);
            return;
        }
    }

    if (sent)
    {
        /* the chip sits on the last candidate: the search prelude and the
           PID inits go out again before the next poll */
        ap_runner_baseline_invalidate();
        ap_runner_reset();
    }

    /* nothing there (ignition off, nothing plugged in): look less often */
    if (s_probe_gap_us < AP_PROBE_GAP_MAX_US)
    {
        s_probe_gap_us *= 2;
    }
}

static bool probe_due(void)
{
    return esp_timer_get_time() - s_probe_last_us >= s_probe_gap_us;
}

/* ---- the listener's word (a J1939 network) --------------------------------- */

#define AP_J1939_LOOK_US   (1LL * 1000 * 1000)  /* the store is asked this often */
#define AP_J1939_SETTLE_US (10LL * 1000 * 1000) /* the listener's head start on
                                                   a truck before the chip
                                                   probes; the VIN broadcast
                                                   (PGN 65260, on request or
                                                   at start-up) gets the same
                                                   time before an identity
                                                   without it                  */
#define AP_J1939_VIN_WATCH_US (120LL * 1000 * 1000) /* after an identity
                                                   without VIN: a VIN heard
                                                   this much later is still
                                                   adopted                     */

static int64_t s_j1939_last_us;
static int64_t s_first_idle_us;
static int64_t s_vin_watch_until_us;   /* 0 = no watch                     */

/**
 * A J1939 network the listener hears is a sighting like a chip answer: its
 * source addresses are the responder set, its VIN (when one was broadcast)
 * the VIN, and the store decides as for any other car. Only once the chip
 * found nobody (its probe ran): an EU truck answers the chip too, and the
 * chip's responders are the better fingerprint of such a vehicle.
 * @return true when the store took a decision.
 */
static bool j1939_contact(void)
{
    static ap_veh_seen_t seen EXT_RAM_BSS_ATTR; /* poller-task only      */
    int64_t now = esp_timer_get_time();

    if (now - s_j1939_last_us < AP_J1939_LOOK_US)
    {
        return false;
    }

    s_j1939_last_us = now;
    memset(&seen, 0, sizeof(seen));

    if (!ap_j1939_identify(&seen, NULL, NULL))
    {
        return false;
    }

    if (seen.vin[0] == '\0' && now - s_first_idle_us < AP_J1939_SETTLE_US)
    {
        /* the groups come at once, the VIN a few seconds later (a BAM,
           sent on request or at start-up): a VIN keys the car for good,
           the fingerprint only by its controllers. Worth the wait. */
        return false;
    }

    seen.dialect = AP_DIALECT_J1939; /* the store keeps a chip dialect it
                                        already knows for this car */

    ESP_LOGI(TAG, "vehicle identity (J1939 network): vin %s, %u sources",
             seen.vin[0] ? seen.vin : "(none)", (unsigned)seen.n_ecus);

    /* an identity without VIN: a VIN heard later is adopted by the store
       (an entry learned without its VIN is matched by its fingerprint) */
    s_vin_watch_until_us = (seen.vin[0] == '\0') ? now + AP_J1939_VIN_WATCH_US
                                                 : 0;

    switch (autopid_vehicle_seen(&seen))
    {
        case AP_VEH_RES_NONE:
            s_vin_watch_until_us = 0;
            return false;

        case AP_VEH_RES_SAME:
            s_ident_done = true;
            break;

        case AP_VEH_RES_SWITCHED:
            s_ident_done = true;
            s_proto_fallback = false;
            ap_runner_restore_baseline();
            break;

        case AP_VEH_RES_NEW:
            /* the detection job stores it (its chip walk finds nothing on
               a J1939-only truck, its network step the rows) */
            ap_std_scan_hint('\0', AP_DIALECT_J1939);
            s_ident_done = (autopid_std_scan_start() == ESP_OK);
            break;
    }

    return true;
}

/** After an identity without VIN: the VIN the listener hears later. */
static void j1939_vin_watch(void)
{
    static ap_veh_seen_t seen EXT_RAM_BSS_ATTR; /* poller-task only      */
    int64_t now = esp_timer_get_time();

    if (s_vin_watch_until_us == 0 || now - s_j1939_last_us < AP_J1939_LOOK_US)
    {
        return;
    }

    s_j1939_last_us = now;

    if (now > s_vin_watch_until_us)
    {
        s_vin_watch_until_us = 0;
        return;
    }

    memset(&seen, 0, sizeof(seen));

    if (ap_j1939_identify(&seen, NULL, NULL) && seen.vin[0] != '\0')
    {
        seen.dialect = AP_DIALECT_J1939;
        ESP_LOGI(TAG, "vehicle identity (J1939 network): VIN %s heard",
                 seen.vin);
        (void)autopid_vehicle_seen(&seen); /* SAME, the VIN learned */
        s_vin_watch_until_us = 0;
    }
}

void ap_runner_idle(void)
{
    if (s_ident_done)
    {
        j1939_vin_watch();
        return;
    }

    int64_t now = esp_timer_get_time();

    if (s_first_idle_us == 0)
    {
        s_first_idle_us = now;
    }

    /* the chip probed and found nobody: the listener's word counts */
    if (s_probe_last_us != 0 && j1939_contact())
    {
        return;
    }

    /* a stored J1939 vehicle with the listener up: give the listener its
       head start before the chip asks a truck for OBD (one probe would be
       harmless; a boot without any is better) */
    if (s_probe_last_us == 0 && autopid_vehicle_dialect() == AP_DIALECT_J1939 &&
        ap_j1939_listening() && now - s_first_idle_us < AP_J1939_SETTLE_US)
    {
        if (j1939_contact())
        {
            s_probe_last_us = now; /* the probe is not owed any more */
        }

        return;
    }

    /* nothing is scheduled: the probe stands in for the poll, so first
       contact happens on a device with empty tables too */
    if (probe_due())
    {
        contact_probe();
    }
}

void ap_runner_poll_result(bool ok)
{
    if (s_ident_done)
    {
        return;
    }

    if (ok)
    {
        identity_check(car_dialect(), '\0');
        return;
    }

    note_silence();

    /* the rows are not answered: they may be another car's (another
       dialect's). Look for whoever is there. */
    if (s_boot_fail_streak >= AP_BOOT_SILENT_POLLS && probe_due())
    {
        contact_probe();
    }

    /* the chip probed and found nobody: the listener's word counts here as
       in ap_runner_idle(), which a table of chip rows never reaches (a
       device moved from a car to a J1939 truck polls the car's rows for
       ever otherwise; bench 2026-10-03) */
    if (s_probe_last_us != 0)
    {
        if (s_first_idle_us == 0)
        {
            s_first_idle_us = esp_timer_get_time(); /* the VIN's settle time */
        }

        (void)j1939_contact();
    }
}
