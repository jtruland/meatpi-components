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
 * @file vpn_manager_check.c
 * @brief Pure config validation (host-tested; no esp deps beyond
 *        esp_err.h). The settings on_validate speaks through
 *        vpn_check_config so the UI gets a reason, not just a 400.
 */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "vpn_manager_private.h"

#define WG_KEY_B64_LEN 44 /* base64(32 bytes) incl. the trailing '=' */

static bool b64_char(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '+' || c == '/';
}

bool vpn_check_wg_key(const char *key_b64)
{
    if (key_b64 == NULL || strlen(key_b64) != WG_KEY_B64_LEN)
    {
        return false;
    }

    for (int i = 0; i < WG_KEY_B64_LEN - 1; i++)
    {
        if (!b64_char(key_b64[i]))
        {
            return false;
        }
    }

    return key_b64[WG_KEY_B64_LEN - 1] == '=';
}

bool vpn_check_endpoint(const char *host)
{
    if (host == NULL)
    {
        return false;
    }

    size_t len = strlen(host);

    if (len < 1 || len > 63)
    {
        return false;
    }

    for (size_t i = 0; i < len; i++)
    {
        char c = host[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '-' ||
                  c == ':' || c == '_';

        if (!ok)
        {
            return false;
        }
    }

    return true;
}

const char *vpn_check_config(const vpn_config_t *cfg)
{
    if (!cfg->enabled)
    {
        return NULL; /* disabled config is always storable */
    }

    if (cfg->tailscale)
    {
        size_t len = strlen(cfg->ts_auth_key);

        /* tskey-auth-... (Tailscale) or a headscale preauth key (hex);
         * shape-check only: the coordinator is the judge */
        if (len < 16)
        {
            return "ts_auth_key is required for tailscale (from your "
                   "coordinator's admin)";
        }

        if (cfg->ts_control_url[0] != '\0' &&
            !vpn_check_endpoint(cfg->ts_control_url))
        {
            return "ts_control_url must be a hostname or IP (no "
                   "scheme)";
        }

        return NULL; /* wireguard fields not required for tailscale */
    }

    if (!vpn_check_wg_key(cfg->private_key))
    {
        return "private_key must be a 44-char base64 WireGuard key "
               "(use the keygen)";
    }

    if (!vpn_check_wg_key(cfg->peer_public_key))
    {
        return "peer_public_key must be a 44-char base64 WireGuard key";
    }

    if (cfg->preshared_key[0] != '\0' &&
        !vpn_check_wg_key(cfg->preshared_key))
    {
        return "preshared_key must be empty or a 44-char base64 key";
    }

    if (!vpn_check_endpoint(cfg->endpoint))
    {
        return "endpoint must be a hostname or IP";
    }

    if (cfg->port < 1 || cfg->port > 65535)
    {
        return "port must be 1..65535";
    }

    if (cfg->address[0] == '\0')
    {
        return "address (local tunnel IP) is required";
    }

    vpn_route_t r[VPN_MAX_ROUTES];

    if (vpn_parse_routes(cfg->routes, r, VPN_MAX_ROUTES) < 0)
    {
        return "routes must be up to 8 comma-separated IPv4 CIDRs "
               "(e.g. 192.168.86.0/24, 10.200.0.0/24)";
    }

    if (cfg->home_bypass && cfg->home_ssids[0] == '\0')
    {
        return "home_ssids is required when home_bypass is on";
    }

    return NULL;
}

/* ---- bitz fork: split-tunnel routes + home SSIDs -------------------------- */

/* "a.b.c.d" -> host-order u32; strict: four 0..255 decimal octets */
static bool parse_ipv4(const char *s, size_t len, uint32_t *out)
{
    uint32_t v = 0;
    int octets = 0;
    size_t i = 0;

    while (octets < 4)
    {
        if (i >= len || !isdigit((unsigned char)s[i]))
        {
            return false;
        }

        int o = 0;
        int digits = 0;

        while (i < len && isdigit((unsigned char)s[i]) && digits < 4)
        {
            o = o * 10 + (s[i++] - '0');
            digits++;
        }

        if (o > 255 || digits > 3)
        {
            return false;
        }

        v = (v << 8) | (uint32_t)o;

        if (++octets < 4)
        {
            if (i >= len || s[i] != '.')
            {
                return false;
            }

            i++;
        }
    }

    *out = v;
    return i == len;
}

static void trim(const char **b, const char **e)
{
    while (*b < *e && isspace((unsigned char)**b))
    {
        (*b)++;
    }

    while (*e > *b && isspace((unsigned char)(*e)[-1]))
    {
        (*e)--;
    }
}

int vpn_parse_routes(const char *list, vpn_route_t *out, int max)
{
    int n = 0;

    if (list == NULL)
    {
        return 0;
    }

    const char *p = list;

    while (*p != '\0')
    {
        const char *end = strchr(p, ',');

        if (end == NULL)
        {
            end = p + strlen(p);
        }

        const char *b = p;
        const char *e = end;

        trim(&b, &e);

        if (b < e)
        {
            const char *slash = memchr(b, '/', (size_t)(e - b));
            int bits = 32;
            uint32_t ip;

            if (slash != NULL)
            {
                char *num_end = NULL;
                char buf[4] = { 0 };
                size_t nl = (size_t)(e - slash - 1);

                if (nl < 1 || nl > 2)
                {
                    return -1;
                }

                memcpy(buf, slash + 1, nl);
                bits = (int)strtol(buf, &num_end, 10);

                if (*num_end != '\0' || bits < 0 || bits > 32)
                {
                    return -1;
                }
            }

            if (!parse_ipv4(b, (size_t)((slash ? slash : e) - b), &ip) ||
                n >= max)
            {
                return -1;
            }

            uint32_t mask = bits == 0 ? 0 : 0xFFFFFFFFu << (32 - bits);

            out[n].net = ip & mask;
            out[n].mask = mask;
            n++;
        }

        p = (*end == ',') ? end + 1 : end;
    }

    return n;
}

bool vpn_route_match(const vpn_route_t *r, int n, uint32_t dest)
{
    for (int i = 0; i < n; i++)
    {
        if ((dest & r[i].mask) == r[i].net)
        {
            return true;
        }
    }

    return false;
}

bool vpn_ssid_in_list(const char *ssid, const char *list)
{
    if (ssid == NULL || ssid[0] == '\0' || list == NULL)
    {
        return false;
    }

    size_t sl = strlen(ssid);
    const char *p = list;

    while (*p != '\0')
    {
        const char *end = strchr(p, ',');

        if (end == NULL)
        {
            end = p + strlen(p);
        }

        const char *b = p;
        const char *e = end;

        trim(&b, &e);

        if ((size_t)(e - b) == sl && memcmp(b, ssid, sl) == 0)
        {
            return true;
        }

        p = (*end == ',') ? end + 1 : end;
    }

    return false;
}
