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
 * @file espnetlink_link_core.h
 * @brief Pure helpers (no IDF deps): host-tested: the dongle's JSON
 *        documents, the VID/PID identity, and the zero-touch pairing
 *        state machine as a transition table the engine drives.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- dongle identity --------------------------------------------------- */

#define ESPNL_USB_VID       0x303A   /* Espressif */
#define ESPNL_USB_PID       0x4007   /* ESPNetLink composite: RNDIS or NCM
                                        + the ACM console, or (dongle
                                        usb_mode=console) the ACM console
                                        alone */
#define ESPNL_USB_JTAG_PID  0x1001   /* S3 ROM serial-JTAG: boot window */
#define ESPNL_USB_HOST_ADDR "192.168.7.1"   /* the dongle on the NCM link */
#define ESPNL_USB_OWN_ADDR  "192.168.7.2"   /* what its DHCP hands us     */
/* The dongle's /api/info api_level this integration was written against:
 * 7 = the WiFi-modem surface (credentials, /api/wifi_modem health, the
 * runtime USB class). Older builds are still probed (they may carry the
 * routes without the bump), but the mismatch is reported in the status
 * so a stale dongle build is visible instead of a "foreign device". */
#define ESPNL_MIN_API_LEVEL 7

/** True for the ESPNetLink's own composite device only. */
bool espnl_core_is_espnetlink(uint16_t vid, uint16_t pid);

/** True when the device on the port is the ESPNetLink with NO USB
 *  Ethernet class driver bound: its usb_mode is `console` (dongle api 9,
 *  2026-10-12), the CDC console alone, nothing to pair over USB. The
 *  engine debounces it (a fresh RNDIS/NCM enumeration shows the device
 *  before its driver is bound). */
bool espnl_core_is_console_only(bool dev_present, uint16_t dev_vid,
                                uint16_t dev_pid, bool eth_driver_bound);

/* A USB mode (usb_rndis / usb_ncm) with the dongle on the port (ID pin)
 * but never enumerated: its data lines were cut by an earlier wifi_modem
 * pass (a runtime cut lasts until the dongle's next boot) and the WiCAN
 * is not on its AP to restore them. One VBUS cycle brings it back with
 * USB on (2026-10-12). Ticks are seconds; the kick is due once. */
#define ESPNL_USB_KICK_TICKS 30

/** True when that kick is due: a USB mode, the host up with a device on
 *  the ID pin, nothing enumerated (no Ethernet link, no console-only
 *  dongle either), the machine idle, for ESPNL_USB_KICK_TICKS ticks, and
 *  not kicked yet for this presence. */
bool espnl_core_usb_kick_due(bool usb_mode, bool host_active,
                             bool device_present, bool enumerated,
                             bool console_only, bool machine_idle,
                             int ticks_present, bool kicked);

/* ---- the GPS hint (2026-10-13) ------------------------------------------ */

/* A USB mode keeps the data lines live, which desenses the dongle's GPS
 * (bench 2026-08-26: cold fixes 86 to 110 s on the wire against 50 to 66 s
 * with the lines cut, and worse availability). A WiCAN away from home on
 * the wire, LTE up, with no fix for this long is what the UI nudges toward
 * WiFi modem. The firmware counts; the status carries the verdict. */
#define ESPNL_NOFIX_HINT_S 600

typedef struct
{
    bool     running;   /**< the condition holds since `since_ms`       */
    uint32_t since_ms;
} espnl_nofix_t;

/** Feed every tick: @p away = the dongle is the uplink over USB and the
 *  station is not on a home network, @p lte_up = the dongle's PPP is up,
 *  @p fix = a fix is known (the cached one or the dongle's own). Returns
 *  the seconds the condition (away && lte_up && !fix) has held, 0 while
 *  it does not; the millisecond clock may wrap. */
uint32_t espnl_core_nofix_update(espnl_nofix_t *acc, bool away, bool lte_up,
                                 bool fix, uint32_t now_ms);

/* ---- the SIM PIN relay (2026-10-13) ------------------------------------- */

/** True when the WiCAN's SIM PIN should go to the dongle now: it has one
 *  (@p pin non-empty), the dongle's SIM reports "pin" (locked: no PIN on
 *  the dongle, or one it has not tried), and this boot has not sent it
 *  yet. A rejected PIN ("pin_wrong") is never retried by itself: three
 *  wrong tries lock the SIM. NULLs read as empty. */
bool espnl_core_pin_relay_due(const char *pin, const char *sim, bool sent);

/* ---- link mode --------------------------------------------------------- */

/* Mirrors the public espnetlink_mode_t (espnetlink_link.h) value-for-value:
 * kept separate so this header stays IDF-free for the host tests. */
typedef enum
{
    ESPNL_CORE_MODE_WIFI_MODEM = 0,
    ESPNL_CORE_MODE_USB_NCM,
    ESPNL_CORE_MODE_USB_RNDIS,
} espnl_core_mode_t;

/** Settings-token decode ("wifi_modem"/"usb_ncm"/"usb_rndis"); anything
 *  else (including NULL) falls back to wifi_modem. (The schema default is
 *  usb_rndis since 2026-10-12; this fallback only matters for a token the
 *  schema never produced.) */
espnl_core_mode_t espnl_core_mode_from_str(const char *s);

/** The settings token for a mode (never NULL). */
const char *espnl_core_mode_str(espnl_core_mode_t mode);

/** The dongle-side `usb_dev_ethernet.class` value this mode needs
 *  ("ncm"/"rndis"), or NULL for wifi_modem (class irrelevant: cut). */
const char *espnl_core_mode_usb_class(espnl_core_mode_t mode);

/** The dongle's wifi_manager.mode that keeps its access point up, given
 *  the mode it reports (2026-10-19: the dongle ships with WiFi off):
 *  "ap" from "off", "apsta" from "sta", NULL when the AP is already up
 *  ("ap", "apsta") or the key is unknown ("", an older firmware). */
const char *espnl_core_ap_mode_wanted(const char *current);

/* ---- documents --------------------------------------------------------- */

typedef struct
{
    bool valid;          /**< document parsed                           */
    bool napt;
    bool usb_data;       /**< data lines routed to the host             */
    bool usb_ethernet;   /**< the USB Ethernet function is present (dongle
                              api 9: false in usb_mode=console; an older
                              dongle has no key and reads true)         */
    bool lte_valid;
    bool lte_attached;
    bool lte_connected;
    int  rssi_dbm;       /**< 0 when absent                             */
    char operator_name[24];
    char network_type[12];
    /* 2026-10-07 (dongle api 8, absent before: ""): the SIM as the
       dongle's bring-up found it ("ready" | "missing" | "pin" |
       "unknown"; "pin_wrong" since 2026-10-13: the SIM refused the PIN
       the dongle holds), the bring-up stage ("not_started" |
       "connecting" | "connected" | "ppp_disconnected" |
       "network_detached") and the carrier address while connected */
    char sim[12];
    char stage[20];
    char ip[16];
    bool gps_valid;
    bool gps_fix;
    int  ap_clients;
} espnl_health_t;

typedef struct
{
    bool valid;
    bool is_espnetlink;  /**< device_type == "espnetlink"               */
    char device_id[13];  /**< 12 hex                                    */
    char fw_version[24];
    int  api_level;
} espnl_info_t;

typedef struct
{
    bool valid;          /**< ssid + password + device_id all present   */
    char ssid[33];
    char password[65];
    char device_id[13];
    bool ap_started;
} espnl_creds_t;

/** True when the connected SSID is the configured ESPNetLink SSID
 *  (exact, case-sensitive; empty configured never matches). */
bool espnl_core_ssid_match(const char *configured, const char *connected);

/** Parse `GET /api/wifi_modem` (dongle health). Tolerant: missing
 *  sections leave their fields false/0; returns false only when no
 *  JSON object is present. */
bool espnl_core_parse_health(const char *json, espnl_health_t *out);

/** Parse `GET /api/info`. False when no object. */
bool espnl_core_parse_info(const char *json, espnl_info_t *out);

/** Parse `GET /api/wifi_modem/credentials`. False unless ssid, password
 *  and device_id are all present and within bounds. */
bool espnl_core_parse_credentials(const char *json, espnl_creds_t *out);

/** True when the freshly read credentials differ from what is stored
 *  (any of ssid / password / device_id; NULL stored strings = ""). */
bool espnl_core_creds_changed(const espnl_creds_t *fresh,
                              const char *stored_ssid,
                              const char *stored_password,
                              const char *stored_device_id);

/** Build "http://<host>/<path>" into @p out; false when it does not fit
 *  or host is empty. */
bool espnl_core_url(char *out, unsigned out_len, const char *host,
                    const char *path);

/* ---- pairing state machine --------------------------------------------- */

typedef enum
{
    ESPNL_SM_IDLE = 0,   /**< no ESPNetLink on the USB link               */
    ESPNL_SM_IDENTIFY,   /**< GET /api/info in flight                     */
    ESPNL_SM_READ_KEY,   /**< GET /api/wifi_modem/credentials              */
    ESPNL_SM_CUT,        /**< POST usb_data=false                          */
    ESPNL_SM_WAIT_DROP,  /**< cut acknowledged; waiting for the USB drop   */
    ESPNL_SM_DONE,       /**< cut done: cable is power only                */
    ESPNL_SM_TETHER,     /**< dongle refused (usb_mode=ncm): leave USB on  */
    ESPNL_SM_FOREIGN,    /**< not an ESPNetLink / gave up until re-plug    */
    ESPNL_SM_NCM_SHARE,  /**< usb_ncm mode: ensuring the dongle shares     */
    ESPNL_SM_NCM_UP,     /**< usb_ncm mode: steady, polling over USB       */
    ESPNL_SM_HOLD,       /**< key read but it cannot be stored now (factory
                              AP password / store refused): wait for the
                              operator, no retries, no VBUS cycles         */
    ESPNL_SM_UNSUPPORTED,/**< the dongle firmware lacks the API (404 /
                              api_level too old): update it, then re-plug */
    ESPNL_SM_UNREACHABLE,/**< enumerated as an ESPNetLink (303A:4007) but
                              /api/info never answered within the identify
                              budget: a GET every 10 s, one VBUS cycle after
                              2 min (budgeted), then a GET every 60 s; an
                              answer resumes the pairing (2026-10-23)      */
    ESPNL_SM_COUNT
} espnl_sm_state_t;

typedef enum
{
    ESPNL_EV_TICK = 0,        /**< 1 s heartbeat                          */
    ESPNL_EV_LINK_UP,         /**< NCM up + IP, device is 303A:4007       */
    ESPNL_EV_LINK_UP_OTHER,   /**< NCM up to something else               */
    ESPNL_EV_LINK_DOWN,
    ESPNL_EV_OK,              /**< last action succeeded                  */
    ESPNL_EV_FAIL,            /**< last action failed (transport/5xx)     */
    ESPNL_EV_CONFLICT,        /**< POST usb_data -> 409                   */
    ESPNL_EV_NOT_ESPNETLINK,  /**< /api/info answered, wrong device_type  */
    ESPNL_EV_AP_STALE,        /**< stored key no longer joins the AP      */
    ESPNL_EV_REPAIR,          /**< operator asked for a re-pair           */
    ESPNL_EV_HOLD,            /**< key read, store not possible now (the
                                   engine explains in last_error)         */
    ESPNL_EV_UNSUPPORTED,     /**< the dongle has no such API (HTTP 404)  */
} espnl_sm_event_t;

typedef enum
{
    ESPNL_ACT_NONE = 0,
    ESPNL_ACT_GET_INFO,
    ESPNL_ACT_GET_KEY,
    ESPNL_ACT_POST_CUT,
    ESPNL_ACT_ENSURE_SHARE,   /**< usb_ncm mode: dongle ncm_share=true     */
    ESPNL_ACT_VBUS_CYCLE,
    ESPNL_ACT_REBOOT,         /**< new key stored: reboot-to-apply         */
    ESPNL_ACT_GIVE_UP,        /**< recovery budget exhausted               */
} espnl_sm_action_t;

#define ESPNL_SM_IDENTIFY_MS   15000  /* /api/info budget per attach (the
                                         first GET after a re-enumeration
                                         tends to time out: 2026-10-23)   */
#define ESPNL_SM_UNREACH_RETRY_MS 10000 /* unreachable: a GET this often    */
#define ESPNL_SM_UNREACH_CYCLE_MS 120000 /* unreachable this long: VBUS cycle */
#define ESPNL_SM_UNREACH_SLOW_MS  60000 /* cycles spent: a GET this often   */
#define ESPNL_SM_READ_KEY_MS   5000
#define ESPNL_SM_DROP_RETRY_MS 3000   /* re-POST the cut once after this  */
#define ESPNL_SM_DROP_FAIL_MS  10000  /* still enumerated: VBUS cycle     */
#define ESPNL_SM_SHARE_MS      10000
#define ESPNL_SM_CYCLE_GAP_MS  60000  /* min spacing of VBUS recoveries   */
#define ESPNL_SM_MAX_CYCLES    3      /* per unbroken failure run         */

typedef struct
{
    espnl_sm_state_t state;
    bool     ncm_mode;        /**< usb_ncm: never cut, ensure sharing      */
    int      cut_retries;     /**< POST attempts before recovery (1..5)    */
    uint32_t since_ms;        /**< state entry time                        */
    uint32_t last_cycle_ms;
    int      cycles;          /**< consecutive VBUS cycles without success */
    int      cut_posts;       /**< POSTs issued for this attach            */
    bool     reboot_pending;  /**< set by the engine when the key changed  */
    bool     link_up;
    uint32_t last_try_ms;     /**< unreachable: the last GET /api/info     */
    int      retries;         /**< unreachable: GETs since entering it     */
} espnl_sm_t;

void espnl_sm_init(espnl_sm_t *sm, bool ncm_mode, int cut_retries,
                   uint32_t now_ms);

/** Feed one event; returns the action the engine must perform now.
 *  The engine reports the action's outcome with OK/FAIL/CONFLICT/
 *  NOT_ESPNETLINK on a later call. Pure: no I/O, no clocks. */
espnl_sm_action_t espnl_sm_step(espnl_sm_t *sm, espnl_sm_event_t ev,
                                uint32_t now_ms);

const char *espnl_sm_state_str(espnl_sm_state_t s);

#ifdef __cplusplus
}
#endif
