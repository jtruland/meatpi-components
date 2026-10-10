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
 * @file vpn_manager.c
 * @brief Lifecycle, the state task, and the DNS override (see
 *        include/vpn_manager.h for the model). Settings live in
 *        vpn_manager_settings.c (standard §4.1).
 *
 * State task ladder: WAITING (network bit + valid clock) → CONNECTING
 * (vpn_wg_up + peer poll, 20 s budget) → CONNECTED (VPN bit, event,
 * optional DNS override/default route) → loss of network or peer
 * silence tears down and re-enters WAITING with backoff.
 */
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/def.h"

#include "dev_status_manager.h"
#include "log_manager.h"
#include "rtc_manager.h"

#include "vpn_manager_private.h"
#include "vpn_route_hook.h"

static const char *TAG = "vpn_manager";

#define VPN_POLL_MS        1000
#define VPN_HANDSHAKE_S    20    /* peer-up budget per attempt        */
#define VPN_PEER_LOST_S    30    /* silent peer => reconnect          */
#define VPN_BACKOFF_MIN_S  5
#define VPN_BACKOFF_MAX_S  60

static volatile bool s_run;
static TaskHandle_t s_task;
/* The state task uses a DYNAMIC internal stack (plain xTaskCreate),
 * created only when the VPN feature is enabled, so it costs zero
 * internal RAM when disabled. INTERNAL (not PSRAM) because the
 * tailscale path calls microlink_init here, which reads/writes device
 * keys in NVS = flash access, and §2-corollary forbids flash IO from a
 * PSRAM-stack task (WireGuard does no NVS, so it never tripped this). */
#define VPN_TASK_STACK 6144 /* watermark-checked on the bench 2026-07-07; was 7168, see TASK_tailscale.md internal-RAM note */

static vpn_manager_status_t s_status;
static uint32_t s_up_since_ms;

/* split-tunnel (bitz fork): the tunnel netif the route hook hands out,
 * published only while CONNECTED with routes configured. Read lock-free on
 * the tcpip thread; cleared BEFORE the tunnel is torn down. */
static struct netif *volatile s_route_netif;

/* DNS override bookkeeping */
static esp_netif_dns_info_t s_saved_dns;
static bool s_dns_overridden;

/* ---- DNS override ----------------------------------------------------------- */

static esp_netif_t *sta_netif(void)
{
    return esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
}

static void dns_override_apply(void)
{
    const vpn_config_t *cfg = vpn_settings_config();
    esp_netif_t *netif = sta_netif();
    esp_netif_dns_info_t dns = { 0 };

    if (cfg->dns[0] == '\0' || netif == NULL)
    {
        return;
    }

    if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN,
                               &s_saved_dns) != ESP_OK)
    {
        return;
    }

    dns.ip.type = ESP_IPADDR_TYPE_V4;

    if (inet_pton(AF_INET, cfg->dns, &dns.ip.u_addr.ip4) != 1)
    {
        ESP_LOGW(TAG, "dns '%s' is not an IPv4 address", cfg->dns);
        return;
    }

    if (esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK)
    {
        s_dns_overridden = true;
        ESP_LOGI(TAG, "DNS override -> %s", cfg->dns);
    }
}

static void dns_override_restore(void)
{
    esp_netif_t *netif = sta_netif();

    if (!s_dns_overridden || netif == NULL)
    {
        return;
    }

    (void)esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &s_saved_dns);
    s_dns_overridden = false;
    ESP_LOGI(TAG, "DNS restored");
}

/* ---- state task -------------------------------------------------------------- */

static bool network_up(void)
{
    return (dev_status_manager_get() &
            DEV_STATUS_NETWORK_CONNECTED_MASK) != 0;
}

/* home-Wi-Fi pause (bitz fork, v4 commit 5cc581d): the station is
 * associated to a trusted SSID. Authenticated by the WPA association itself
 * (we only join networks we hold credentials for); a subnet match was
 * rejected because 192.168.86.0/24 is a router factory default. */
static bool home_paused(void)
{
    const vpn_config_t *cfg = vpn_settings_config();
    wifi_ap_record_t ap;

    if (!cfg->home_bypass || esp_wifi_sta_get_ap_info(&ap) != ESP_OK)
    {
        return false;
    }

    return vpn_ssid_in_list((const char *)ap.ssid, cfg->home_ssids);
}

struct netif *vpn_manager_route_netif(const ip4_addr_t *dest)
{
    struct netif *wg = s_route_netif;

    if (wg == NULL)
    {
        return NULL;
    }

    const vpn_config_t *cfg = vpn_settings_config();

    return vpn_route_match(cfg->route, cfg->route_count,
                           lwip_ntohl(ip4_addr_get_u32(dest)))
               ? wg : NULL;
}

static void enter_connected(void)
{
    const vpn_config_t *cfg = vpn_settings_config();

    s_status.state = VPN_STATE_CONNECTED;
    s_status.connects++;
    s_up_since_ms = (uint32_t)(esp_log_timestamp());
    dev_status_manager_set(DEV_STATUS_BIT_VPN_ENABLED);

    if (!cfg->tailscale)
    {
        /* wireguard-only concerns: microlink owns its own netif,
         * routes and (Magic)DNS */
        dns_override_apply();

        if (cfg->default_route)
        {
            (void)vpn_wg_set_default_route();
        }
        else if (cfg->route_count > 0)
        {
            s_route_netif = vpn_wg_netif();
            ESP_LOGI(TAG, "split tunnel: %d route(s) via WireGuard (%s)",
                     cfg->route_count, cfg->routes);
        }
    }

    vpn_events_state(true);
    ESP_LOGI(TAG, "connected (%s)", vpn_settings_endpoint());
}

static void teardown(bool was_connected)
{
    s_route_netif = NULL; /* the hook stops handing it out first */

    if (vpn_settings_config()->tailscale)
    {
        vpn_ts_down();
    }
    else
    {
        dns_override_restore();
        vpn_wg_down();
    }

    dev_status_manager_clear(DEV_STATUS_BIT_VPN_ENABLED);

    if (was_connected)
    {
        vpn_events_state(false);
    }
}

/* type-dispatched primitives for the state ladder */
static esp_err_t tunnel_up(void)
{
    const vpn_config_t *cfg = vpn_settings_config();

    return cfg->tailscale ? vpn_ts_up(cfg) : vpn_wg_up(cfg);
}

static bool tunnel_alive(void)
{
    return vpn_settings_config()->tailscale ? vpn_ts_connected()
                                            : vpn_wg_peer_up();
}

static void state_task(void *arg)
{
    const vpn_config_t *cfg = vpn_settings_config();
    uint32_t backoff_s = VPN_BACKOFF_MIN_S;

    (void)arg;

    while (true)
    {
        if (!s_run)
        {
            if (s_status.state >= VPN_STATE_CONNECTING)
            {
                teardown(s_status.state == VPN_STATE_CONNECTED);
            }

            s_status.state = VPN_STATE_DISABLED;
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        /* ---- PAUSED_HOME (bitz fork): on a trusted home SSID */
        if (network_up() && home_paused())
        {
            if (s_status.state != VPN_STATE_PAUSED_HOME)
            {
                ESP_LOGI(TAG, "on a home Wi-Fi SSID; tunnel paused");
            }

            s_status.state = VPN_STATE_PAUSED_HOME;
            vTaskDelay(pdMS_TO_TICKS(VPN_POLL_MS * 5));
            continue;
        }

        /* ---- WAITING: uplink + a sane clock (WG handshake needs it) */
        s_status.state = VPN_STATE_WAITING;

        if (!network_up())
        {
            vTaskDelay(pdMS_TO_TICKS(VPN_POLL_MS));
            continue;
        }

        if (!rtc_manager_time_valid())
        {
            static bool s_warned;

            if (!s_warned)
            {
                ESP_LOGW(TAG, "waiting for a valid clock (WireGuard "
                         "handshakes need one)");
                s_warned = true;
            }

            vTaskDelay(pdMS_TO_TICKS(VPN_POLL_MS));
            continue;
        }

        /* ---- CONNECTING */
        s_status.state = VPN_STATE_CONNECTING;

        if (tunnel_up() != ESP_OK)
        {
            s_status.failures++;
            teardown(false);
            vTaskDelay(pdMS_TO_TICKS(backoff_s * 1000));
            backoff_s = (backoff_s * 2 > VPN_BACKOFF_MAX_S)
                            ? VPN_BACKOFF_MAX_S : backoff_s * 2;
            continue;
        }

        bool up = false;
        /* tailscale needs the full control-plane dance (register +
         * map + DERP/DISCO): give it a longer budget than a plain
         * WG handshake */
        int budget_s = cfg->tailscale ? 90 : VPN_HANDSHAKE_S;

        for (int i = 0; i < budget_s && s_run; i++)
        {
            if (!network_up() || home_paused())
            {
                break;
            }

            if (cfg->tailscale && vpn_ts_failed())
            {
                break;
            }

            if (tunnel_alive())
            {
                up = true;
                break;
            }

            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        if (!up)
        {
            ESP_LOGW(TAG, "no handshake within %d s", budget_s);
            s_status.failures++;
            teardown(false);
            vTaskDelay(pdMS_TO_TICKS(backoff_s * 1000));
            backoff_s = (backoff_s * 2 > VPN_BACKOFF_MAX_S)
                            ? VPN_BACKOFF_MAX_S : backoff_s * 2;
            continue;
        }

        /* ---- CONNECTED */
        backoff_s = VPN_BACKOFF_MIN_S;
        enter_connected();

        uint32_t silent_s = 0;

        while (s_run && network_up() && !home_paused())
        {
            vTaskDelay(pdMS_TO_TICKS(VPN_POLL_MS));

            if (tunnel_alive())
            {
                silent_s = 0;
                continue;
            }

            if (++silent_s >= VPN_PEER_LOST_S)
            {
                ESP_LOGW(TAG, "peer silent for %lu s; reconnecting",
                         (unsigned long)silent_s);
                break;
            }
        }

        teardown(true);
        ESP_LOGI(TAG, "left connected state (network %s)",
                 network_up() ? "up" : "down");
    }
}

/* ---- lifecycle -------------------------------------------------------------- */

esp_err_t vpn_manager_init(void)
{
    static const log_descriptor_t LOG_DESC =
        { "vpn_manager", ESP_LOG_INFO };

    log_manager_register(&LOG_DESC);
    vpn_events_register();
    return vpn_settings_register();
}

esp_err_t vpn_manager_start(void)
{
    const vpn_config_t *cfg = vpn_settings_config();

    if (!vpn_settings_is_configured())
    {
        ESP_LOGE(TAG, "unconfigured; not starting");
        return ESP_ERR_INVALID_STATE; /* standard §4.3 step 5 */
    }

    if (!cfg->enabled)
    {
        ESP_LOGI(TAG, "disabled in settings");
        return ESP_OK;
    }

    s_run = true;

    if (s_task == NULL)
    {
        if (xTaskCreate(state_task, "vpn_state", VPN_TASK_STACK,
                        NULL, 3, &s_task) != pdPASS)
        {
            s_task = NULL;
        }

        if (s_task == NULL)
        {
            return ESP_FAIL;
        }
    }

    ESP_LOGI(TAG, "started (%s, default_route %s)",
             vpn_settings_endpoint(),
             cfg->default_route ? "on" : "off");
    return ESP_OK;
}

esp_err_t vpn_manager_stop(void)
{
    s_run = false; /* the state task tears down on its next lap */
    return ESP_OK;
}

esp_err_t vpn_manager_status(vpn_manager_status_t *out)
{
    const vpn_config_t *cfg = vpn_settings_config();

    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out = s_status;
    /* boot-applied fields live with the settings (vpn_manager_settings.c) */
    strlcpy(out->endpoint, vpn_settings_endpoint(),
            sizeof(out->endpoint));
    out->tailscale = cfg->tailscale;
    out->uptime_s = (s_status.state == VPN_STATE_CONNECTED)
        ? (esp_log_timestamp() - s_up_since_ms) / 1000 : 0;

    if (cfg->tailscale)
    {
        vpn_ts_status(out->ts_ip, sizeof(out->ts_ip), &out->ts_peers);
    }

    return ESP_OK;
}

esp_err_t vpn_manager_tunnel_ip(char *buf, size_t len)
{
    if (buf == NULL || len == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    buf[0] = '\0';

    if (s_status.state != VPN_STATE_CONNECTED)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const vpn_config_t *cfg = vpn_settings_config();

    if (cfg->tailscale)
    {
        int peers = 0;

        vpn_ts_status(buf, len, &peers);
    }
    else
    {
        strlcpy(buf, cfg->address, len);

        /* the setting allows an optional /cidr suffix: the tunnel IP
           consumers (HA vpn_ip) want the bare address */
        char *slash = strchr(buf, '/');

        if (slash != NULL)
        {
            *slash = '\0';
        }
    }

    return buf[0] != '\0' ? ESP_OK : ESP_ERR_INVALID_STATE;
}
