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
 * @file vpn_manager_private.h
 * @brief Internals shared between the state machine, the esp_wireguard
 *        glue, the pure config checks, and the HTTP/CLI/events glue.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "vpn_manager.h"

/* split-tunnel route (bitz fork): HOST byte order, mask as bits */
#define VPN_MAX_ROUTES 8
typedef struct
{
    uint32_t net;
    uint32_t mask;
} vpn_route_t;

/* boot-applied configuration (strings sized to the settings schema) */
typedef struct
{
    bool enabled;
    bool tailscale;            /* type == "tailscale"                */
    char private_key[64];
    char peer_public_key[64];
    char preshared_key[64];
    char address[32];          /* local tunnel IP, optional /cidr    */
    char allowed_ip[32];
    char allowed_ip_mask[32];
    char endpoint[64];
    int  port;
    int  keepalive_s;
    bool default_route;
    char dns[16];              /* optional override, "" = off        */
    /* bitz fork (settings v2) */
    char routes[192];          /* "a.b.c.d/n, ..." split-tunnel CIDRs */
    vpn_route_t route[VPN_MAX_ROUTES];
    int  route_count;          /* parsed from routes; 0 = upstream mode */
    bool home_bypass;          /* pause the tunnel on a home SSID     */
    char home_ssids[128];      /* comma-separated, exact match        */
    /* tailscale (microlink) */
    char ts_auth_key[96];      /* SECRET (auth_key redaction suffix) */
    char ts_device_name[48];   /* "" = wican_<device_id>             */
    char ts_control_url[64];   /* "" = tailscale.com; host only      */
} vpn_config_t;

/* ---- vpn_manager_check.c: PURE (host-tested) ---------------------------- */

/* a WireGuard key is 32 bytes base64: exactly 44 chars, valid alphabet,
 * '=' terminated */
bool vpn_check_wg_key(const char *key_b64);
/* endpoint hostname/IP sanity (len 1..63, [A-Za-z0-9.-:_]) */
bool vpn_check_endpoint(const char *host);
/* full enabled-config validation; returns NULL when OK, else a static
 * human-readable reason (the settings on_validate message) */
const char *vpn_check_config(const vpn_config_t *cfg);

/* bitz fork. Parse "192.168.86.0/24, 10.200.0.0/24" (a bare address is a
 * /32) into @p out; returns the count, or -1 when any entry is malformed or
 * there are more than @p max. Empty/NULL = 0. */
int vpn_parse_routes(const char *list, vpn_route_t *out, int max);
/* @p dest (host byte order) falls inside one of the routes */
bool vpn_route_match(const vpn_route_t *r, int n, uint32_t dest);
/* @p ssid is exactly one of the comma-separated entries (each trimmed);
 * SSIDs are case-sensitive. Empty ssid never matches. */
bool vpn_ssid_in_list(const char *ssid, const char *list);

/* ---- vpn_manager_wg.c: esp_wireguard glue (state task ONLY) ------------- */

esp_err_t vpn_wg_up(const vpn_config_t *cfg);   /* init + connect      */
void      vpn_wg_down(void);                    /* disconnect + reset  */
bool      vpn_wg_peer_up(void);
esp_err_t vpn_wg_set_default_route(void);
struct netif *vpn_wg_netif(void);               /* NULL when down      */

/* ---- vpn_manager_ts.c: tailscale/microlink glue (state task ONLY) ------- */

esp_err_t vpn_ts_up(const vpn_config_t *cfg);   /* init + start        */
void      vpn_ts_down(void);                    /* stop + destroy      */
bool      vpn_ts_connected(void);
bool      vpn_ts_failed(void);
void      vpn_ts_status(char *ip, size_t ip_len, int *peers);

/* Per-peer snapshot for /api/vpn + the `vpn` CLI ("peer online +
 * direct-vs-relay"). Returns the number of entries written (0 when the
 * tunnel is down or type != tailscale). */
typedef struct
{
    char hostname[64];
    char ip[16];
    bool online;
    bool direct_path; /* true = direct UDP path, false = DERP relay */
} vpn_ts_peer_t;

int vpn_ts_get_peers(vpn_ts_peer_t *out, int max);

/* ---- settings (vpn_manager_settings.c) ------------------------------------ */

/** Register the "vpn_manager" descriptor with settings_manager. */
esp_err_t vpn_settings_register(void);

/** Boot-applied config; valid once vpn_settings_is_configured(). */
const vpn_config_t *vpn_settings_config(void);
bool vpn_settings_is_configured(void); /* boot apply ran (standard §4.3) */

/** Display endpoint ("host:port" or the control server), derived on
 *  the settings apply for status/log strings. */
const char *vpn_settings_endpoint(void);

/* ---- glue ----------------------------------------------------------------- */

void vpn_events_register(void);
void vpn_events_state(bool connected);
