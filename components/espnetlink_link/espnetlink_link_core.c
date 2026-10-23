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
 * @file espnetlink_link_core.c
 * @brief Pure helpers: document parsers (the same lightweight keyed scan
 *        as usb_acm_gps.c, no cJSON, so the host suite needs no IDF
 *        components), identity checks, URL build, and the pairing state
 *        machine (§3 of the ESPNetLink ↔ WiCAN integration contract).
 */
#include "espnetlink_link_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- identity ---------------------------------------------------------- */

bool espnl_core_is_espnetlink(uint16_t vid, uint16_t pid)
{
    return vid == ESPNL_USB_VID && pid == ESPNL_USB_PID;
}

espnl_core_mode_t espnl_core_mode_from_str(const char *s)
{
    if (s != NULL)
    {
        if (strcmp(s, "usb_ncm") == 0)
        {
            return ESPNL_CORE_MODE_USB_NCM;
        }
        if (strcmp(s, "usb_rndis") == 0)
        {
            return ESPNL_CORE_MODE_USB_RNDIS;
        }
    }

    return ESPNL_CORE_MODE_WIFI_MODEM;
}

const char *espnl_core_mode_str(espnl_core_mode_t mode)
{
    switch (mode)
    {
        case ESPNL_CORE_MODE_USB_NCM:
            return "usb_ncm";
        case ESPNL_CORE_MODE_USB_RNDIS:
            return "usb_rndis";
        default:
            return "wifi_modem";
    }
}

const char *espnl_core_mode_usb_class(espnl_core_mode_t mode)
{
    switch (mode)
    {
        case ESPNL_CORE_MODE_USB_NCM:
            return "ncm";
        case ESPNL_CORE_MODE_USB_RNDIS:
            return "rndis";
        default:
            return NULL;
    }
}

const char *espnl_core_ap_mode_wanted(const char *current)
{
    if (current == NULL || current[0] == '\0')
    {
        return NULL;
    }
    if (strcmp(current, "off") == 0)
    {
        return "ap";
    }
    if (strcmp(current, "sta") == 0)
    {
        return "apsta";
    }
    return NULL;
}

bool espnl_core_is_console_only(bool dev_present, uint16_t dev_vid,
                                uint16_t dev_pid, bool eth_driver_bound)
{
    /* the dongle's own identity on the port but no USB Ethernet class
     * bound: its usb_mode is console (2026-10-12), the CDC console alone.
     * An NCM or RNDIS composite has its driver bound within the first
     * tick after enumeration; the engine still debounces the verdict. */
    return dev_present && espnl_core_is_espnetlink(dev_vid, dev_pid) &&
           !eth_driver_bound;
}

bool espnl_core_usb_kick_due(bool usb_mode, bool host_active,
                             bool device_present, bool enumerated,
                             bool console_only, bool machine_idle,
                             int ticks_present, bool kicked)
{
    return usb_mode && host_active && device_present && !enumerated &&
           !console_only && machine_idle && !kicked &&
           ticks_present >= ESPNL_USB_KICK_TICKS;
}

/* ---- the GPS hint ------------------------------------------------------- */

uint32_t espnl_core_nofix_update(espnl_nofix_t *acc, bool away, bool lte_up,
                                 bool fix, uint32_t now_ms)
{
    if (acc == NULL)
    {
        return 0;
    }

    bool cond = away && lte_up && !fix;

    if (!cond)
    {
        acc->running = false;
        return 0;
    }
    if (!acc->running)
    {
        acc->running = true;
        acc->since_ms = now_ms;
        return 0;
    }
    return (now_ms - acc->since_ms) / 1000u;
}

/* ---- the SIM PIN relay -------------------------------------------------- */

bool espnl_core_pin_relay_due(const char *pin, const char *sim, bool sent)
{
    if (pin == NULL || pin[0] == '\0' || sim == NULL || sent)
    {
        return false;
    }
    return strcmp(sim, "pin") == 0;
}

bool espnl_core_ssid_match(const char *configured, const char *connected)
{
    if (configured == NULL || connected == NULL || configured[0] == '\0')
    {
        return false;
    }
    return strcmp(configured, connected) == 0;
}

/* ---- keyed scan -------------------------------------------------------- */

/* Locate `"key":` inside [start, end). Returns a pointer just past the
 * colon (whitespace skipped) or NULL. */
static const char *find_key(const char *start, const char *end,
                            const char *quoted_key)
{
    size_t kl = strlen(quoted_key);
    const char *p = strstr(start, quoted_key);

    if (p == NULL || p >= end)
    {
        return NULL;
    }
    p += kl;
    while (*p == ' ' || *p == '\t')
    {
        p++;
    }
    return p;
}

static bool val_true(const char *start, const char *end, const char *key)
{
    const char *p = find_key(start, end, key);

    return p != NULL && *p == 't';
}

static bool val_num(const char *start, const char *end, const char *key,
                    double *out)
{
    const char *p = find_key(start, end, key);

    if (p == NULL)
    {
        return false;
    }

    char *e = NULL;
    double v = strtod(p, &e);

    if (e == p)
    {
        return false;
    }
    *out = v;
    return true;
}

/* Copy a JSON string value (no escape processing beyond \" and \\: the
 * dongle never emits anything else in these documents). False when the
 * key is absent, not a string, or does not fit. */
static bool val_str(const char *start, const char *end, const char *key,
                    char *out, size_t out_len)
{
    const char *p = find_key(start, end, key);
    size_t n = 0;

    out[0] = '\0';
    if (p == NULL || *p != '"')
    {
        return false;
    }
    p++;
    while (*p != '\0' && *p != '"')
    {
        char c = *p;

        if (c == '\\' && (p[1] == '"' || p[1] == '\\'))
        {
            c = p[1];
            p++;
        }
        if (n + 1 >= out_len)
        {
            out[0] = '\0';
            return false;
        }
        out[n++] = c;
        p++;
    }
    out[n] = '\0';
    return *p == '"';
}

/* [start,end) of the object value for `"section":{...}` (one nesting
 * level: the health document is flat inside each section). */
static bool section(const char *json, const char *quoted_key,
                    const char **start, const char **end)
{
    const char *p = find_key(json, json + strlen(json), quoted_key);

    if (p == NULL || *p != '{')
    {
        return false;
    }

    const char *q = strchr(p, '}');

    if (q == NULL)
    {
        return false;
    }
    *start = p;
    *end = q + 1;
    return true;
}

static const char *object_start(const char *json)
{
    return json != NULL ? strchr(json, '{') : NULL;
}

/* ---- documents --------------------------------------------------------- */

bool espnl_core_parse_health(const char *json, espnl_health_t *out)
{
    if (out == NULL)
    {
        return false;
    }
    memset(out, 0, sizeof(*out));

    const char *obj = object_start(json);

    if (obj == NULL)
    {
        return false;
    }

    const char *top_end = obj + strlen(obj);
    const char *s;
    const char *e;
    double d;

    out->napt = val_true(obj, top_end, "\"napt\":");
    out->usb_data = val_true(obj, top_end, "\"usb_data\":");
    /* dongle api 9 (2026-10-12): false = the dongle presents its CDC
     * console alone on USB (usb_mode console); an older dongle has no such
     * key and always carries the Ethernet function */
    out->usb_ethernet = find_key(obj, top_end, "\"usb_ethernet\":") == NULL ||
                        val_true(obj, top_end, "\"usb_ethernet\":");

    if (section(obj, "\"lte\":", &s, &e))
    {
        out->lte_valid     = val_true(s, e, "\"valid\":");
        out->lte_attached  = val_true(s, e, "\"attached\":");
        out->lte_connected = val_true(s, e, "\"connected\":");
        if (val_num(s, e, "\"rssi_dbm\":", &d))
        {
            out->rssi_dbm = (int)d;
        }
        (void)val_str(s, e, "\"operator\":", out->operator_name,
                      sizeof(out->operator_name));
        (void)val_str(s, e, "\"network_type\":", out->network_type,
                      sizeof(out->network_type));
        (void)val_str(s, e, "\"sim\":", out->sim, sizeof(out->sim));
        (void)val_str(s, e, "\"stage\":", out->stage, sizeof(out->stage));
        (void)val_str(s, e, "\"ip\":", out->ip, sizeof(out->ip));
    }

    if (section(obj, "\"gps\":", &s, &e))
    {
        out->gps_valid = val_true(s, e, "\"valid\":");
        out->gps_fix   = val_true(s, e, "\"fix\":");
    }

    if (section(obj, "\"ap\":", &s, &e) &&
        val_num(s, e, "\"clients\":", &d))
    {
        out->ap_clients = (int)d;
    }

    out->valid = true;
    return true;
}

bool espnl_core_parse_info(const char *json, espnl_info_t *out)
{
    if (out == NULL)
    {
        return false;
    }
    memset(out, 0, sizeof(*out));

    const char *obj = object_start(json);

    if (obj == NULL)
    {
        return false;
    }

    const char *end = obj + strlen(obj);
    char type[16];
    double d;

    out->is_espnetlink = val_str(obj, end, "\"device_type\":", type,
                                 sizeof(type)) &&
                         strcmp(type, "espnetlink") == 0;
    (void)val_str(obj, end, "\"device_id\":", out->device_id,
                  sizeof(out->device_id));
    (void)val_str(obj, end, "\"fw_version\":", out->fw_version,
                  sizeof(out->fw_version));
    if (val_num(obj, end, "\"api_level\":", &d))
    {
        out->api_level = (int)d;
    }
    out->valid = true;
    return true;
}

bool espnl_core_parse_credentials(const char *json, espnl_creds_t *out)
{
    if (out == NULL)
    {
        return false;
    }
    memset(out, 0, sizeof(*out));

    const char *obj = object_start(json);

    if (obj == NULL)
    {
        return false;
    }

    const char *end = obj + strlen(obj);

    if (!val_str(obj, end, "\"ssid\":", out->ssid, sizeof(out->ssid)) ||
        !val_str(obj, end, "\"password\":", out->password,
                 sizeof(out->password)) ||
        !val_str(obj, end, "\"device_id\":", out->device_id,
                 sizeof(out->device_id)) ||
        out->ssid[0] == '\0' || out->device_id[0] == '\0')
    {
        memset(out, 0, sizeof(*out));
        return false;
    }
    out->ap_started = val_true(obj, end, "\"ap_started\":");
    out->valid = true;
    return true;
}

bool espnl_core_creds_changed(const espnl_creds_t *fresh,
                              const char *stored_ssid,
                              const char *stored_password,
                              const char *stored_device_id)
{
    if (fresh == NULL || !fresh->valid)
    {
        return false;
    }
    return strcmp(fresh->ssid, stored_ssid ? stored_ssid : "") != 0 ||
           strcmp(fresh->password,
                  stored_password ? stored_password : "") != 0 ||
           strcmp(fresh->device_id,
                  stored_device_id ? stored_device_id : "") != 0;
}

bool espnl_core_url(char *out, unsigned out_len, const char *host,
                    const char *path)
{
    if (out == NULL || out_len == 0)
    {
        return false;
    }
    out[0] = '\0';

    if (host == NULL || host[0] == '\0' || path == NULL)
    {
        return false;
    }

    int n = snprintf(out, out_len, "http://%s%s%s", host,
                     path[0] == '/' ? "" : "/", path);

    if (n <= 0 || (unsigned)n >= out_len)
    {
        out[0] = '\0'; /* never hand back a truncated URL */
        return false;
    }
    return true;
}

/* ---- state machine ----------------------------------------------------- */

static const char *const STATE_STR[ESPNL_SM_COUNT] =
{
    "idle", "identify", "read_key", "cut", "wait_drop", "done", "tether",
    "foreign", "ncm_share", "ncm_up", "hold", "unsupported", "unreachable",
};

const char *espnl_sm_state_str(espnl_sm_state_t s)
{
    return (s >= 0 && s < ESPNL_SM_COUNT) ? STATE_STR[s] : "?";
}

void espnl_sm_init(espnl_sm_t *sm, bool ncm_mode, int cut_retries,
                   uint32_t now_ms)
{
    memset(sm, 0, sizeof(*sm));
    sm->state = ESPNL_SM_IDLE;
    sm->ncm_mode = ncm_mode;
    sm->cut_retries = cut_retries < 1 ? 1 : (cut_retries > 5 ? 5
                                                              : cut_retries);
    sm->since_ms = now_ms;
    /* allow a recovery cycle right away on the first failure run */
    sm->last_cycle_ms = now_ms - ESPNL_SM_CYCLE_GAP_MS;
}

static void enter(espnl_sm_t *sm, espnl_sm_state_t s, uint32_t now_ms)
{
    sm->state = s;
    sm->since_ms = now_ms;
}

static uint32_t elapsed(const espnl_sm_t *sm, uint32_t now_ms)
{
    return now_ms - sm->since_ms;
}

/* Recovery: a VBUS cycle re-boots the dongle so it enumerates afresh.
 * Spaced and budgeted so a dongle that can never be cut does not get
 * power-cycled forever. */
static espnl_sm_action_t recover(espnl_sm_t *sm, uint32_t now_ms)
{
    if (sm->cycles >= ESPNL_SM_MAX_CYCLES ||
        (uint32_t)(now_ms - sm->last_cycle_ms) < ESPNL_SM_CYCLE_GAP_MS)
    {
        enter(sm, ESPNL_SM_FOREIGN, now_ms);
        return ESPNL_ACT_GIVE_UP;
    }
    sm->cycles++;
    sm->last_cycle_ms = now_ms;
    sm->cut_posts = 0;
    enter(sm, ESPNL_SM_IDLE, now_ms);
    return ESPNL_ACT_VBUS_CYCLE;
}

static espnl_sm_action_t on_link_up(espnl_sm_t *sm, uint32_t now_ms)
{
    sm->link_up = true;
    sm->cut_posts = 0;
    sm->reboot_pending = false;
    enter(sm, ESPNL_SM_IDENTIFY, now_ms);
    return ESPNL_ACT_GET_INFO;
}

espnl_sm_action_t espnl_sm_step(espnl_sm_t *sm, espnl_sm_event_t ev,
                                uint32_t now_ms)
{
    if (sm == NULL)
    {
        return ESPNL_ACT_NONE;
    }

    /* link edges are meaningful in (almost) every state */
    if (ev == ESPNL_EV_LINK_DOWN)
    {
        bool was_waiting = sm->state == ESPNL_SM_WAIT_DROP ||
                           sm->state == ESPNL_SM_CUT;

        sm->link_up = false;
        if (was_waiting)
        {
            /* the cut landed: cable is power only from here */
            sm->cycles = 0;
            enter(sm, ESPNL_SM_DONE, now_ms);
            return sm->reboot_pending ? ESPNL_ACT_REBOOT : ESPNL_ACT_NONE;
        }
        if (sm->state != ESPNL_SM_DONE)
        {
            enter(sm, ESPNL_SM_IDLE, now_ms);
        }
        return ESPNL_ACT_NONE;
    }
    if (ev == ESPNL_EV_LINK_UP_OTHER)
    {
        sm->link_up = true;
        enter(sm, ESPNL_SM_FOREIGN, now_ms);
        return ESPNL_ACT_NONE;
    }
    if (ev == ESPNL_EV_LINK_UP)
    {
        /* every enumeration re-reads the key (swapped / reset dongle) */
        return on_link_up(sm, now_ms);
    }
    if (ev == ESPNL_EV_REPAIR)
    {
        /* the operator's button: spend one cycle regardless of budget */
        sm->cycles = 0;
        sm->last_cycle_ms = now_ms - ESPNL_SM_CYCLE_GAP_MS;
        return recover(sm, now_ms);
    }

    switch (sm->state)
    {
        case ESPNL_SM_IDLE:
        case ESPNL_SM_FOREIGN:
        case ESPNL_SM_TETHER:
        case ESPNL_SM_HOLD:
        case ESPNL_SM_UNSUPPORTED:
            /* terminal until the link edges or the operator's re-pair:
             * nothing to retry, and never a VBUS cycle (a HOLD waits for
             * a settings change, UNSUPPORTED for a firmware update) */
            return ESPNL_ACT_NONE;

        case ESPNL_SM_IDENTIFY:
            if (ev == ESPNL_EV_OK)
            {
                if (sm->ncm_mode)
                {
                    enter(sm, ESPNL_SM_NCM_SHARE, now_ms);
                    return ESPNL_ACT_ENSURE_SHARE;
                }
                enter(sm, ESPNL_SM_READ_KEY, now_ms);
                return ESPNL_ACT_GET_KEY;
            }
            if (ev == ESPNL_EV_NOT_ESPNETLINK)
            {
                enter(sm, ESPNL_SM_FOREIGN, now_ms);
                return ESPNL_ACT_NONE;
            }
            if (ev == ESPNL_EV_UNSUPPORTED)
            {
                enter(sm, ESPNL_SM_UNSUPPORTED, now_ms);
                return ESPNL_ACT_NONE;
            }
            if (ev == ESPNL_EV_TICK || ev == ESPNL_EV_FAIL)
            {
                if (elapsed(sm, now_ms) >= ESPNL_SM_IDENTIFY_MS)
                {
                    /* it IS an ESPNetLink by its USB identity, it only
                     * does not answer HTTP: keep asking on a slow cadence
                     * instead of parking it as foreign (2026-10-23: a
                     * reply the host never delivered looked exactly like
                     * this, and nothing retried until a re-plug) */
                    enter(sm, ESPNL_SM_UNREACHABLE, now_ms);
                    sm->last_try_ms = now_ms;
                    sm->retries = 0;
                    return ESPNL_ACT_NONE;
                }
                return ev == ESPNL_EV_TICK ? ESPNL_ACT_GET_INFO
                                           : ESPNL_ACT_NONE;
            }
            return ESPNL_ACT_NONE;

        case ESPNL_SM_UNREACHABLE:
            if (ev == ESPNL_EV_OK)
            {
                /* it answered after all: the pairing goes on as from
                 * identify */
                if (sm->ncm_mode)
                {
                    enter(sm, ESPNL_SM_NCM_SHARE, now_ms);
                    return ESPNL_ACT_ENSURE_SHARE;
                }
                enter(sm, ESPNL_SM_READ_KEY, now_ms);
                return ESPNL_ACT_GET_KEY;
            }
            if (ev == ESPNL_EV_NOT_ESPNETLINK)
            {
                enter(sm, ESPNL_SM_FOREIGN, now_ms);
                return ESPNL_ACT_NONE;
            }
            if (ev == ESPNL_EV_UNSUPPORTED)
            {
                enter(sm, ESPNL_SM_UNSUPPORTED, now_ms);
                return ESPNL_ACT_NONE;
            }
            if (ev == ESPNL_EV_TICK)
            {
                bool can_cycle = sm->cycles < ESPNL_SM_MAX_CYCLES &&
                                 (uint32_t)(now_ms - sm->last_cycle_ms) >=
                                     ESPNL_SM_CYCLE_GAP_MS;

                if (elapsed(sm, now_ms) >= ESPNL_SM_UNREACH_CYCLE_MS &&
                    can_cycle)
                {
                    /* a fresh enumeration is the one thing that has helped
                     * in practice; budgeted like every other recovery */
                    return recover(sm, now_ms);
                }

                uint32_t period = sm->cycles >= ESPNL_SM_MAX_CYCLES
                                      ? ESPNL_SM_UNREACH_SLOW_MS
                                      : ESPNL_SM_UNREACH_RETRY_MS;

                if ((uint32_t)(now_ms - sm->last_try_ms) >= period)
                {
                    sm->last_try_ms = now_ms;
                    sm->retries++;
                    return ESPNL_ACT_GET_INFO;
                }
            }
            return ESPNL_ACT_NONE;

        case ESPNL_SM_READ_KEY:
            if (ev == ESPNL_EV_OK)
            {
                /* the engine stored/verified the key before reporting OK
                 * and set reboot_pending when it changed */
                enter(sm, ESPNL_SM_CUT, now_ms);
                sm->cut_posts = 1;
                return ESPNL_ACT_POST_CUT;
            }
            if (ev == ESPNL_EV_HOLD)
            {
                /* the key is fine, the WiCAN side cannot take it yet
                 * (factory AP password): a retry or a power cycle would
                 * change nothing, park until the operator acts */
                enter(sm, ESPNL_SM_HOLD, now_ms);
                return ESPNL_ACT_NONE;
            }
            if (ev == ESPNL_EV_UNSUPPORTED)
            {
                enter(sm, ESPNL_SM_UNSUPPORTED, now_ms);
                return ESPNL_ACT_NONE;
            }
            if (ev == ESPNL_EV_TICK || ev == ESPNL_EV_FAIL)
            {
                if (elapsed(sm, now_ms) >= ESPNL_SM_READ_KEY_MS)
                {
                    return recover(sm, now_ms);
                }
                return ev == ESPNL_EV_TICK ? ESPNL_ACT_GET_KEY
                                           : ESPNL_ACT_NONE;
            }
            return ESPNL_ACT_NONE;

        case ESPNL_SM_CUT:
            if (ev == ESPNL_EV_OK)
            {
                enter(sm, ESPNL_SM_WAIT_DROP, now_ms);
                return ESPNL_ACT_NONE;
            }
            if (ev == ESPNL_EV_CONFLICT)
            {
                /* owner locked usb_mode=ncm: PC-tethering mode, leave it */
                enter(sm, ESPNL_SM_TETHER, now_ms);
                return ESPNL_ACT_NONE;
            }
            if (ev == ESPNL_EV_FAIL || ev == ESPNL_EV_TICK)
            {
                if (ev == ESPNL_EV_TICK)
                {
                    if (sm->cut_posts >= sm->cut_retries)
                    {
                        return recover(sm, now_ms);
                    }
                    sm->cut_posts++;
                    return ESPNL_ACT_POST_CUT;
                }
                return ESPNL_ACT_NONE;
            }
            return ESPNL_ACT_NONE;

        case ESPNL_SM_WAIT_DROP:
            if (ev == ESPNL_EV_TICK)
            {
                uint32_t t = elapsed(sm, now_ms);

                if (t >= ESPNL_SM_DROP_FAIL_MS)
                {
                    return recover(sm, now_ms);
                }
                if (t >= ESPNL_SM_DROP_RETRY_MS &&
                    sm->cut_posts <= sm->cut_retries)
                {
                    /* one more POST; the dongle answers "already" when
                     * it did cut and the drop is just late */
                    sm->cut_posts = sm->cut_retries + 1;
                    return ESPNL_ACT_POST_CUT;
                }
            }
            if (ev == ESPNL_EV_CONFLICT)
            {
                enter(sm, ESPNL_SM_TETHER, now_ms);
            }
            return ESPNL_ACT_NONE;

        case ESPNL_SM_DONE:
            if (ev == ESPNL_EV_AP_STALE)
            {
                /* stored key no longer joins the AP (dongle reset or key
                 * rotated): re-enumerate it to read the key again */
                return recover(sm, now_ms);
            }
            return ESPNL_ACT_NONE;

        case ESPNL_SM_NCM_SHARE:
            if (ev == ESPNL_EV_OK || ev == ESPNL_EV_UNSUPPORTED)
            {
                /* UNSUPPORTED: the dongle firmware has no settings API
                 * for the class/sharing, the link itself works, so run
                 * with what it offers (the engine reports the gap) */
                sm->cycles = 0;
                enter(sm, ESPNL_SM_NCM_UP, now_ms);
                return ESPNL_ACT_NONE;
            }
            if (ev == ESPNL_EV_TICK || ev == ESPNL_EV_FAIL)
            {
                if (elapsed(sm, now_ms) >= ESPNL_SM_SHARE_MS)
                {
                    /* can't read/set the dongle's settings: run with
                     * whatever it offers rather than fight it */
                    enter(sm, ESPNL_SM_NCM_UP, now_ms);
                    return ESPNL_ACT_NONE;
                }
                return ev == ESPNL_EV_TICK ? ESPNL_ACT_ENSURE_SHARE
                                           : ESPNL_ACT_NONE;
            }
            return ESPNL_ACT_NONE;

        case ESPNL_SM_NCM_UP:
        default:
            return ESPNL_ACT_NONE;
    }
}
