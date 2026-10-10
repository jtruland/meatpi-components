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
 * @file vpn_manager_cli.c
 * @brief The `vpn` CLI command: registered by
 *        vpn_manager_register_cli() on the settings apply (§6b).
 */
#include "esp_attr.h"
#include "cmdline_manager.h"

#include "vpn_manager.h"
#include "vpn_manager_private.h"

static const char *state_str(vpn_state_t st)
{
    switch (st)
    {
        case VPN_STATE_WAITING:    return "waiting for network/time";
        case VPN_STATE_CONNECTING: return "connecting";
        case VPN_STATE_CONNECTED:  return "connected";
        case VPN_STATE_PAUSED_HOME: return "paused (home Wi-Fi)";
        default:                   return "disabled";
    }
}

static int cmd_vpn(int argc, char **argv)
{
    vpn_manager_status_t st;

    (void)argc;
    (void)argv;
    (void)vpn_manager_status(&st);

    cmdline_printf("vpn: %s, %s (%s)\n",
                   st.tailscale ? "tailscale" : "wireguard",
                   state_str(st.state),
                   st.endpoint[0] ? st.endpoint : "no endpoint");

    if (st.tailscale && st.ts_ip[0])
    {
        cmdline_printf("  tailnet ip %s, %d peer(s)\n", st.ts_ip,
                       st.ts_peers);

        static vpn_ts_peer_t peers[16] EXT_RAM_BSS_ATTR; /* console task only; PSRAM (1.3 KB) */
        int n = vpn_ts_get_peers(peers, 16);

        for (int i = 0; i < n; i++)
        {
            cmdline_printf("  peer %-16s %-15s %s %s\n",
                           peers[i].hostname, peers[i].ip,
                           peers[i].online ? "online " : "offline",
                           peers[i].direct_path ? "direct" : "relay");
        }
    }

    cmdline_printf("  connects %lu, failures %lu, uptime %lu s\n",
                   (unsigned long)st.connects,
                   (unsigned long)st.failures,
                   (unsigned long)st.uptime_s);
    return 0;
}

esp_err_t vpn_manager_register_cli(void)
{
    static const esp_console_cmd_t CMD =
    {
        .command = "vpn",
        .help = "VPN (WireGuard) status",
        .func = cmd_vpn,
    };

    return cmdline_manager_register(&CMD);
}
