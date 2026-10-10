/*
 * This file is part of the MeatPi components project (bitz fork).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/**
 * @file vpn_route_hook.h
 * @brief WireGuard split-tunnel routing: an lwIP IPv4 route hook that sends
 *        only the destinations in vpn_manager's `routes` setting over the
 *        tunnel. Port of the v4 fork's a8ac72e.
 *
 * Injected into lwIP by the top-level CMakeLists via
 * ESP_IDF_LWIP_HOOK_FILENAME (the pattern of Espressif's vlan_support
 * example). The hook is a `static inline` in this header so it compiles
 * straight into lwIP's own objects; a hook in a separate .c of another
 * component is never pulled in by archive linking.
 *
 * It REPLACES the stock LWIP_HOOK_IP4_ROUTE_SRC (ip4_route_src_hook), which
 * must stay defined because ip4_route_src() only exists when it is: the
 * wrapper tries a destination match first and falls back to the stock hook.
 *
 * Why: esp_wireguard can only steer the tunnel netif's own subnet, or make
 * wg the default route for everything. The second sends the HA webhook to
 * a public URL into the tunnel, where the home firewall drops it.
 */
#pragma once

#include "lwip/ip4_addr.h"
#include "lwip/netif.h"

/* vpn_manager.c: the tunnel netif when @p dest is one of the configured
 * routes and the peer is up; NULL otherwise. Lock-free, tcpip-thread safe. */
struct netif *vpn_manager_route_netif(const ip4_addr_t *dest);

/* the stock hook (lwip_default_hooks.c); lwip_default_hooks.h declares it
 * only AFTER including this file */
struct netif *ip4_route_src_hook(const ip4_addr_t *src,
                                 const ip4_addr_t *dest);

static inline struct netif *bitz_ip4_route_hook(const ip4_addr_t *src,
                                                const ip4_addr_t *dest)
{
    struct netif *wg = (dest != NULL) ? vpn_manager_route_netif(dest) : NULL;

    return (wg != NULL) ? wg : ip4_route_src_hook(src, dest);
}

#undef LWIP_HOOK_IP4_ROUTE_SRC
#define LWIP_HOOK_IP4_ROUTE_SRC bitz_ip4_route_hook
