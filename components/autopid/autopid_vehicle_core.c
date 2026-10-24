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
 * @file autopid_vehicle_core.c
 * @brief PURE vehicle identity (TASK_quick_setup.md): ATDPN reply parse,
 *        VIN extraction from 0902 / 22F190 replies, the responder
 *        fingerprint, the effective-protocol + prelude selection and the
 *        reader for the first-pass vehicle.json (imported once into the
 *        store, autopid_vehicle_index.c). No IDF dependencies;
 *        host-tested (host_test/main/test_vehicle.c).
 *
 * Reply text comes in exactly as autopid receives it from the chip
 * (echo + prompt stripped, spaces on, headers on or off): the VIN
 * parsers reuse ap_resp_to_payload() so every ISO-TP shape the poller
 * understands is a VIN shape too.
 */
#include "autopid_private.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

/* ---- ATDPN ------------------------------------------------------------------ */

bool ap_veh_proto_valid(char c)
{
    return (c >= '1' && c <= '9') || (c >= 'A' && c <= 'C');
}

static bool is_tail_noise(char c)
{
    return c == ' ' || c == '\r' || c == '\n' || c == '\t' || c == '>';
}

bool ap_veh_parse_dpn(const char *reply, char out[AP_VEH_PROTO_LEN])
{
    if (out != NULL)
    {
        out[0] = '\0';
    }

    if (reply == NULL || out == NULL)
    {
        return false;
    }

    const char *p = reply;

    while (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t')
    {
        p++;
    }

    /* a leading 'A' = the chip auto-detected what follows (the scan always
       asks after ATTP0, so the flag is always there); a bare "A" is
       therefore malformed, not J1939 */
    if (*p == 'A' || *p == 'a')
    {
        p++;
    }

    char c = (char)toupper((unsigned char)*p);

    if (!ap_veh_proto_valid(c))
    {
        return false;
    }

    for (p++; *p != '\0'; p++)
    {
        if (!is_tail_noise(*p))
        {
            return false;       /* "6X", "?6", garbage */
        }
    }

    out[0] = c;
    out[1] = '\0';
    return true;
}

/* ---- VIN --------------------------------------------------------------------- */

bool ap_veh_vin_valid(const char *vin)
{
    if (vin == NULL || strlen(vin) != 17)
    {
        return false;
    }

    for (int i = 0; i < 17; i++)
    {
        char c = vin[i];
        bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z');

        if (!ok || c == 'I' || c == 'O' || c == 'Q')
        {
            return false;
        }
    }

    return true;
}

/** Find @p pfx in @p p, skip 0x00 padding after it (and, with @p nodi,
 *  one count byte 0x01), take 17 bytes. */
static bool vin_after_prefix(const uint8_t *p, size_t len,
                             const uint8_t pfx[3], bool nodi,
                             char vin[AP_VIN_LEN])
{
    for (size_t i = 0; i + 3 <= len; i++)
    {
        if (p[i] != pfx[0] || p[i + 1] != pfx[1] || p[i + 2] != pfx[2])
        {
            continue;
        }

        size_t j = i + 3;

        while (j < len && p[j] == 0x00)
        {
            j++;                /* a few ECUs pad before the text        */
        }

        if (nodi && j < len && p[j] == 0x01)
        {
            j++;                /* "one data item": never a VIN character */
        }

        if (len - j < 17)
        {
            return false;
        }

        char cand[AP_VIN_LEN];

        memcpy(cand, p + j, 17);
        cand[17] = '\0';

        if (!ap_veh_vin_valid(cand))
        {
            return false;
        }

        memcpy(vin, cand, AP_VIN_LEN);
        return true;
    }

    return false;
}

static bool vin_from_resp(const char *resp, const uint8_t pfx[3],
                          bool nodi, char vin[AP_VIN_LEN])
{
    if (vin != NULL)
    {
        vin[0] = '\0';
    }

    if (resp == NULL || vin == NULL)
    {
        return false;
    }

    uint8_t payload[AP_PAYLOAD_MAX];
    size_t n = 0;

    /* lowest responder wins (the engine ECU answers 0902 first on every
       car seen so far); error lines (NO DATA, ?) fail here */
    if (ap_resp_to_payload(resp, payload, sizeof(payload), &n) != ESP_OK)
    {
        return false;
    }

    return vin_after_prefix(payload, n, pfx, nodi, vin);
}

bool ap_veh_parse_vin_0902(const char *resp, char vin[AP_VIN_LEN])
{
    static const uint8_t PFX[3] = { 0x49, 0x02, 0x01 };

    return vin_from_resp(resp, PFX, false, vin);
}

bool ap_veh_parse_vin_22f190(const char *resp, char vin[AP_VIN_LEN])
{
    static const uint8_t PFX[3] = { 0x62, 0xF1, 0x90 };

    return vin_from_resp(resp, PFX, false, vin);
}

bool ap_veh_parse_vin_22f802(const char *resp, char vin[AP_VIN_LEN])
{
    static const uint8_t PFX[3] = { 0x62, 0xF8, 0x02 };

    return vin_from_resp(resp, PFX, true, vin);
}

/* ---- responder fingerprint ---------------------------------------------------- */

int ap_veh_ecus_from_0100(const char *resp, ap_veh_ecu_t *out, size_t max)
{
    /* one parser for both dialects (autopid_dialect.c). Until 2026-10-03
       this one took a 29-bit id only as ONE 8-hex token; the chip prints
       four byte tokens, so every 29-bit car was kept as a single
       headers-off row with all bitmaps merged */
    return ap_dialect_bitmaps(AP_DIALECT_OBD2, resp, 0x00, out, max);
}

void ap_veh_fingerprint(const ap_veh_ecu_t *ecus, size_t n,
                        char out[AP_FP_LEN])
{
    if (out == NULL)
    {
        return;
    }

    out[0] = '\0';

    if (ecus == NULL || n == 0)
    {
        return;
    }

    /* sorted + duplicate-merged copy: order independence */
    ap_veh_ecu_t s[AP_RESP_ECUS_MAX];
    size_t m = 0;

    for (size_t i = 0; i < n && m < AP_RESP_ECUS_MAX; i++)
    {
        size_t k = 0;

        while (k < m && s[k].id != ecus[i].id)
        {
            k++;
        }

        if (k < m)
        {
            s[k].bitmap |= ecus[i].bitmap;
            continue;
        }

        /* insertion sort by id */
        size_t pos = m;

        while (pos > 0 && s[pos - 1].id > ecus[i].id)
        {
            s[pos] = s[pos - 1];
            pos--;
        }

        s[pos] = ecus[i];
        m++;
    }

    uint32_t h = 2166136261u;           /* FNV-1a 32 offset basis        */

    for (size_t i = 0; i < m; i++)
    {
        uint8_t b[8] =
        {
            (uint8_t)(s[i].id >> 24), (uint8_t)(s[i].id >> 16),
            (uint8_t)(s[i].id >> 8), (uint8_t)s[i].id,
            (uint8_t)(s[i].bitmap >> 24), (uint8_t)(s[i].bitmap >> 16),
            (uint8_t)(s[i].bitmap >> 8), (uint8_t)s[i].bitmap,
        };

        for (size_t j = 0; j < sizeof(b); j++)
        {
            h ^= b[j];
            h *= 16777619u;
        }
    }

    snprintf(out, AP_FP_LEN, "%08x", (unsigned)h);
}

/* ---- protocol selection + prelude ----------------------------------------------- */

static bool has(const char *s)
{
    return s != NULL && s[0] != '\0';
}

char ap_veh_effective_protocol(const char *setting, const char *stored,
                               bool search_fallback)
{
    char s = has(setting) ? setting[0] : '0';

    if (s >= '6' && s <= '9')
    {
        return s;                   /* pinned by the user: wins          */
    }

    if (!search_fallback && has(stored))
    {
        char c = (char)toupper((unsigned char)stored[0]);

        if (ap_veh_proto_valid(c))
        {
            return c;
        }
    }

    return '0';
}

const char *ap_veh_prelude_for(char proto)
{
    /* CAN protocols: headers ON (2026-10-24, issue #1014): every reply is
       read as the chip prints it with the CAN id, which is what the user
       expressions index (the legacy firmware's frame of reference, the
       published profiles'), and what tells responders and extended-address
       bytes apart. K-line and J1850 keep headers off: their printed header
       is three bytes and a checksum, not an id, and ap_resp_frames()
       rebuilds the ISO-TP shape from the plain bytes instead. */
    switch (proto)
    {
        /* CAN: pin protocol + functional header + clear the CRA filter */
        case '6': return "ATS1;ATH1;ATST96;ATTP6;ATSH7DF;ATCRA";
        case '7': return "ATS1;ATH1;ATST96;ATTP7;ATSH18DB33F1;ATCRA";
        case '8': return "ATS1;ATH1;ATST96;ATTP8;ATSH7DF;ATCRA";
        case '9': return "ATS1;ATH1;ATST96;ATTP9;ATSH18DB33F1;ATCRA";
        /* J1939 and the user CAN protocols: protocol only, headers on */
        case 'A': return "ATS1;ATH1;ATST96;ATTPA";
        case 'B': return "ATS1;ATH1;ATST96;ATTPB";
        case 'C': return "ATS1;ATH1;ATST96;ATTPC";
        /* K-line / J1850: protocol only, headers off */
        case '1': return "ATS1;ATH0;ATST96;ATTP1";
        case '2': return "ATS1;ATH0;ATST96;ATTP2";
        case '3': return "ATS1;ATH0;ATST96;ATTP3";
        case '4': return "ATS1;ATH0;ATST96;ATTP4";
        case '5': return "ATS1;ATH0;ATST96;ATTP5";
        default:  return "ATS1;ATH0;ATST96;ATTP0";
    }
}

bool ap_veh_proto_is_29bit(char proto)
{
    return proto == '7' || proto == '9' || (proto >= 'A' && proto <= 'C');
}

const char *ap_veh_func_header(char proto)
{
    switch (proto)
    {
        case '6':
        case '8': return "ATSH7DF";
        case '7':
        case '9': return "ATSH18DB33F1";
        default:  return NULL;
    }
}

/* ---- the first-pass vehicle.json (import only) --------------------------------- */

bool ap_veh_doc_empty(const ap_vehicle_doc_t *doc)
{
    return doc == NULL || (!has(doc->vin) && !has(doc->fingerprint));
}

static void copy_str(char *dst, size_t cap, const cJSON *obj,
                     const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);

    dst[0] = '\0';

    if (cJSON_IsString(v) && v->valuestring != NULL)
    {
        snprintf(dst, cap, "%s", v->valuestring);
    }
}

bool ap_veh_doc_from_json(const char *json, ap_vehicle_doc_t *out)
{
    if (out == NULL)
    {
        return false;
    }

    memset(out, 0, sizeof(*out));

    if (json == NULL)
    {
        return false;
    }

    cJSON *root = cJSON_Parse(json);

    if (root == NULL || !cJSON_IsObject(root))
    {
        cJSON_Delete(root);
        return false;
    }

    const cJSON *ver = cJSON_GetObjectItemCaseSensitive(root, "version");

    if (!cJSON_IsNumber(ver) || ver->valueint != 1)
    {
        cJSON_Delete(root);
        return false;
    }

    copy_str(out->vin, sizeof(out->vin), root, "vin");
    copy_str(out->protocol, sizeof(out->protocol), root, "protocol");
    copy_str(out->fingerprint, sizeof(out->fingerprint), root,
             "fingerprint");
    copy_str(out->seen_vin, sizeof(out->seen_vin), root, "seen_vin");
    copy_str(out->seen_fingerprint, sizeof(out->seen_fingerprint), root,
             "seen_fingerprint");

    const cJSON *ts = cJSON_GetObjectItemCaseSensitive(root, "detected_ts");

    out->detected_ts = cJSON_IsNumber(ts) ? (int64_t)ts->valuedouble : 0;
    out->changed = cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(root, "changed"));

    /* defensive: a hand-edited file must not smuggle an invalid VIN or
       protocol into the prelude / the compare */
    if (has(out->vin) && !ap_veh_vin_valid(out->vin))
    {
        out->vin[0] = '\0';
    }

    if (has(out->protocol) &&
        !ap_veh_proto_valid((char)toupper((unsigned char)out->protocol[0])))
    {
        out->protocol[0] = '\0';
    }

    cJSON_Delete(root);
    return true;
}
