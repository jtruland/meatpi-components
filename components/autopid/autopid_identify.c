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
 * @file autopid_identify.c
 * @brief Who is this car: the identity requests of one dialect on the
 *        protocol in effect. Shared by first contact (the poller task,
 *        autopid_contact.c) and the detection job (the scan task,
 *        autopid_std_scan.c); each brings its own reply buffer.
 *
 * The responder set comes first: one support-bitmap request with headers
 * on. Every legislated ECU answers it in a single frame and its CAN id
 * tells it apart (the fingerprint, and for a UDS-dialect vehicle the
 * address of each ECU). Then the VIN:
 *
 *   obd2   `0902`, functional (the engine ECU answers on every car seen);
 *          the detection job falls back to `22F190` on the engine ECU's
 *          physical address.
 *   uds    `22F802` ECU by ECU, physically, lowest id first. A functional
 *          request would make every ECU start a multi-frame answer, and
 *          with the chip's receive filter on one of them the others never
 *          get their flow control (bench 2026-10-03: the ECU left waiting
 *          answered the NEXT request late, inside another request's
 *          window). The functional header is put back afterwards.
 *
 * Never touches flash (the poller's stack is PSRAM).
 */
#include <string.h>

#include "freertos/FreeRTOS.h"

#include "autopid_transport.h"
#include "autopid_private.h"

#define AP_ID_REQ_TIMEOUT pdMS_TO_TICKS(6000) /* a multi-frame VIN; the
                                                 chip's search          */
#define AP_ID_AT_TIMEOUT  pdMS_TO_TICKS(2000)
#define AP_ID_VIN_ECUS    3     /* responders asked for the VIN          */

typedef struct
{
    char  *resp;
    size_t len;
} id_io_t;

static esp_err_t ask(const id_io_t *io, const char *cmd, TickType_t timeout)
{
    io->resp[0] = '\0';
    return ap_be()->request(cmd, io->resp, io->len, timeout);
}

/** The responder set: ONE bitmap request; the prelude's headers (on for
 *  every CAN protocol since 2026-10-24, issue #1014) carry each ECU's id.
 *  A K-line prelude keeps them off and the bitmaps merge into one row. */
static void read_responders(const id_io_t *io, ap_dialect_t dialect,
                            ap_veh_seen_t *seen)
{
    char cmd[12];

    seen->n_ecus = 0;

    if (ap_dialect_pid_cmd(dialect, 0x00, cmd, sizeof(cmd)) == 0)
    {
        return;
    }

    if (ask(io, cmd, AP_ID_REQ_TIMEOUT) == ESP_OK)
    {
        seen->n_ecus = (uint8_t)ap_dialect_bitmaps(dialect, io->resp, 0x00,
                                                  seen->ecus,
                                                  AP_VEH_ECUS_MAX);
    }
}

static void read_vin_obd2(const id_io_t *io, char proto, bool fallback,
                          char vin[AP_VIN_LEN])
{
    if (ask(io, "0902", AP_ID_REQ_TIMEOUT) == ESP_OK &&
        ap_veh_parse_vin_0902(io->resp, vin))
    {
        return;
    }

    /* UDS fallback: DID F190 on the engine ECU's physical address, CAN
       protocols only; the functional header is put back either way */
    const char *func = ap_veh_func_header(proto);

    if (!fallback || func == NULL)
    {
        return;
    }

    (void)ask(io, ap_veh_proto_is_29bit(proto) ? "ATSH18DA10F1" : "ATSH7E0",
              AP_ID_AT_TIMEOUT);

    if (ask(io, "22F190", AP_ID_REQ_TIMEOUT) == ESP_OK)
    {
        (void)ap_veh_parse_vin_22f190(io->resp, vin);
    }

    (void)ask(io, func, AP_ID_AT_TIMEOUT);
}

/** The responder after @p after in ascending id order (the first one when
 *  @p first). False when there is none left. */
static bool next_responder(const ap_veh_seen_t *seen, bool first,
                           uint32_t after, uint32_t *out)
{
    bool have = false;

    for (int i = 0; i < seen->n_ecus; i++)
    {
        uint32_t id = seen->ecus[i].id;

        if (id == UINT32_MAX || (!first && id <= after))
        {
            continue;
        }

        if (!have || id < *out)
        {
            *out = id;
            have = true;
        }
    }

    return have;
}

static void read_vin_uds(const id_io_t *io, char proto, bool fallback,
                         ap_veh_seen_t *seen)
{
    char hdr[AP_DIALECT_HDR_CMD_LEN];
    uint32_t id = 0, lowest = UINT32_MAX;
    bool addressed = false;

    for (int k = 0; k < AP_ID_VIN_ECUS && seen->vin[0] == '\0'; k++)
    {
        if (!next_responder(seen, k == 0, id, &id))
        {
            break;
        }

        if (k == 0)
        {
            lowest = id;
        }

        if (ap_dialect_header_cmd(id, hdr, sizeof(hdr)) == 0)
        {
            continue;           /* not a legislated id: cannot be addressed */
        }

        (void)ask(io, hdr, AP_ID_AT_TIMEOUT);
        addressed = true;

        if (ask(io, "22F802", AP_ID_REQ_TIMEOUT) == ESP_OK)
        {
            (void)ap_veh_parse_vin_22f802(io->resp, seen->vin);
        }
    }

    if (lowest == UINT32_MAX)
    {
        /* nobody could be told apart (no ids): one functional try, which
           reads when a single ECU answers */
        if (ask(io, "22F802", AP_ID_REQ_TIMEOUT) == ESP_OK)
        {
            (void)ap_veh_parse_vin_22f802(io->resp, seen->vin);
        }
    }
    else if (seen->vin[0] == '\0' && fallback &&
             ap_dialect_header_cmd(lowest, hdr, sizeof(hdr)) > 0)
    {
        /* the manufacturer's VIN identifier on the first ECU */
        (void)ask(io, hdr, AP_ID_AT_TIMEOUT);
        addressed = true;

        if (ask(io, "22F190", AP_ID_REQ_TIMEOUT) == ESP_OK)
        {
            (void)ap_veh_parse_vin_22f190(io->resp, seen->vin);
        }
    }

    const char *func = ap_veh_func_header(proto);

    if (addressed && func != NULL)
    {
        (void)ask(io, func, AP_ID_AT_TIMEOUT);
    }
}

bool ap_identify(ap_dialect_t dialect, char proto, bool vin_fallback,
                 ap_veh_seen_t *seen, char *resp, size_t resp_len)
{
    const id_io_t io = { resp, resp_len };

    if (seen == NULL || resp == NULL || resp_len == 0 ||
        !ap_dialect_has_requests(dialect))
    {
        return false;
    }

    seen->dialect = (uint8_t)dialect;
    seen->vin[0] = '\0';
    read_responders(&io, dialect, seen);

    if (dialect == AP_DIALECT_UDS)
    {
        read_vin_uds(&io, proto, vin_fallback, seen);
    }
    else
    {
        read_vin_obd2(&io, proto, vin_fallback, seen->vin);
    }

    return seen->vin[0] != '\0' || seen->n_ecus > 0;
}
