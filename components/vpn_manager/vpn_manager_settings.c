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
 * @file vpn_manager_settings.c
 * @brief settings_manager descriptor for vpn_manager: field-table schema
 *        (source of truth for shape/ranges/defaults), on_validate (the
 *        pure vpn_check_config rules), on_apply (parses into the
 *        boot-applied config + the derived display endpoint).
 */
#include <stdio.h>
#include <string.h>

#include "settings_manager.h"

#include "vpn_manager.h" /* vpn_manager_register_cli (settings-gated) */
#include "vpn_manager_private.h"

static const settings_field_t FIELDS[] =
{
    SETTINGS_BOOL("enabled", false),
    SETTINGS_STR_ENUM("type", "wireguard,tailscale", "wireguard"),
    SETTINGS_STR("ts_auth_key", 95, ""),
    SETTINGS_STR("ts_device_name", 47, ""),
    SETTINGS_STR("ts_control_url", 63, ""),
    SETTINGS_STR("private_key", 63, ""),
    SETTINGS_STR("peer_public_key", 63, ""),
    SETTINGS_STR("preshared_key", 63, ""),
    SETTINGS_STR("address", 31, ""),
    SETTINGS_STR("allowed_ip", 31, "0.0.0.0"),
    SETTINGS_STR("allowed_ip_mask", 31, "0.0.0.0"),
    SETTINGS_STR("endpoint", 63, ""),
    SETTINGS_INT("port", 1, 65535, 51820),
    SETTINGS_INT("keepalive_s", 0, 3600, 25),
    SETTINGS_BOOL("default_route", false),
    SETTINGS_STR("dns", 15, ""),
    /* v2 (bitz fork): real split-tunnel routing and the home-Wi-Fi pause.
       routes = the destination CIDRs that go over WireGuard (an lwIP route
       hook, see vpn_route_hook.h); "" keeps the upstream behaviour (the
       AllowedIPs netmask on the tunnel netif). home_bypass holds the tunnel
       down while the station is associated to one of home_ssids. */
    SETTINGS_STR("routes", 191, ""),
    SETTINGS_BOOL("home_bypass", false),
    SETTINGS_STR("home_ssids", 127, ""),
    SETTINGS_BOOL("cli", true),
};

static vpn_config_t s_cfg;
static bool s_configured;
static char s_endpoint[64]; /* display endpoint (status/log strings) */

const vpn_config_t *vpn_settings_config(void)
{
    return &s_cfg;
}

bool vpn_settings_is_configured(void)
{
    return s_configured;
}

const char *vpn_settings_endpoint(void)
{
    return s_endpoint;
}

static void field_str(const cJSON *settings, const char *key, char *out,
                      size_t len)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(settings, key);

    strlcpy(out, cJSON_IsString(item) ? item->valuestring : "", len);
}

static void config_from_json(const cJSON *settings, vpn_config_t *cfg)
{
    const cJSON *item;

    memset(cfg, 0, sizeof(*cfg));
    cfg->enabled = cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(settings, "enabled"));
    item = cJSON_GetObjectItemCaseSensitive(settings, "type");
    cfg->tailscale = cJSON_IsString(item) &&
                     strcmp(item->valuestring, "tailscale") == 0;
    field_str(settings, "ts_auth_key", cfg->ts_auth_key,
              sizeof(cfg->ts_auth_key));
    field_str(settings, "ts_device_name", cfg->ts_device_name,
              sizeof(cfg->ts_device_name));
    field_str(settings, "ts_control_url", cfg->ts_control_url,
              sizeof(cfg->ts_control_url));
    field_str(settings, "private_key", cfg->private_key,
              sizeof(cfg->private_key));
    field_str(settings, "peer_public_key", cfg->peer_public_key,
              sizeof(cfg->peer_public_key));
    field_str(settings, "preshared_key", cfg->preshared_key,
              sizeof(cfg->preshared_key));
    field_str(settings, "address", cfg->address, sizeof(cfg->address));
    field_str(settings, "allowed_ip", cfg->allowed_ip,
              sizeof(cfg->allowed_ip));
    field_str(settings, "allowed_ip_mask", cfg->allowed_ip_mask,
              sizeof(cfg->allowed_ip_mask));
    field_str(settings, "endpoint", cfg->endpoint,
              sizeof(cfg->endpoint));
    item = cJSON_GetObjectItemCaseSensitive(settings, "port");
    cfg->port = cJSON_IsNumber(item) ? item->valueint : 51820;
    item = cJSON_GetObjectItemCaseSensitive(settings, "keepalive_s");
    cfg->keepalive_s = cJSON_IsNumber(item) ? item->valueint : 25;
    cfg->default_route = cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(settings, "default_route"));
    field_str(settings, "dns", cfg->dns, sizeof(cfg->dns));
    field_str(settings, "routes", cfg->routes, sizeof(cfg->routes));
    cfg->route_count = vpn_parse_routes(cfg->routes, cfg->route,
                                        VPN_MAX_ROUTES);
    if (cfg->route_count < 0)
    {
        cfg->route_count = 0; /* on_validate rejects it before storing */
    }
    cfg->home_bypass = cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(settings, "home_bypass"));
    field_str(settings, "home_ssids", cfg->home_ssids,
              sizeof(cfg->home_ssids));
}

static esp_err_t on_validate(const cJSON *settings, char *err,
                             size_t err_len)
{
    vpn_config_t cfg;

    config_from_json(settings, &cfg);

    const char *reason = vpn_check_config(&cfg);

    if (reason != NULL)
    {
        strlcpy(err, reason, err_len);
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

static esp_err_t on_apply(const cJSON *settings)
{
    config_from_json(settings, &s_cfg);

    if (s_cfg.tailscale)
    {
        snprintf(s_endpoint, sizeof(s_endpoint), "%.62s",
                 s_cfg.ts_control_url[0] ? s_cfg.ts_control_url
                                         : "tailscale.com");
    }
    else
    {
        snprintf(s_endpoint, sizeof(s_endpoint),
                 "%.56s:%d", s_cfg.endpoint, s_cfg.port);
    }

    if (!cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(settings, "cli")))
    {
        static bool s_cli_registered;

        if (!s_cli_registered && vpn_manager_register_cli() == ESP_OK)
        {
            s_cli_registered = true;
        }
    }

    s_configured = true;
    return ESP_OK;
}

esp_err_t vpn_settings_register(void)
{
    static const settings_descriptor_t DESC =
    {
        .name = "vpn_manager",
        .version = 1, /* bitz fork adds routes/home_bypass/home_ssids as
                         non-breaking additions (filled from the defaults).
                         NOT a bump: a bump without on_migrate resets the
                         component to defaults, wiping the WireGuard keys */
        .fields = FIELDS,
        .field_count = sizeof(FIELDS) / sizeof(FIELDS[0]),
        .on_apply = on_apply,
        .on_validate = on_validate,
    };

    return settings_manager_register(&DESC);
}
