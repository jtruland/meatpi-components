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
 * @file autopid_resp_lines.c
 * @brief PURE: one line of chip text -> its bytes, CAN header and ISO-TP
 *        row index (host-tested). Split out of autopid_resp.c 2026-10-03
 *        (700-line rule); the shapes are listed there.
 */
#include "autopid_private.h"
#include "autopid_resp_private.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static bool is_hex_str(const char *s, size_t len)
{
    if (len == 0)
    {
        return false;
    }

    for (size_t i = 0; i < len; i++)
    {
        if (!isxdigit((unsigned char)s[i]))
        {
            return false;
        }
    }

    return true;
}

/* Tokenize one line into hex bytes / header / iso-row-index. */
static bool parse_line(const char *line, size_t len, ap_line_t *out)
{
    memset(out, 0, sizeof(*out));
    out->iso_index = -1;

    char tok[AP_LINE_MAX];
    size_t pos = 0;
    int n_tok = 0;

    while (pos < len)
    {
        while (pos < len && (line[pos] == ' ' || line[pos] == '\t'))
        {
            pos++;
        }

        size_t start = pos;

        while (pos < len && line[pos] != ' ' && line[pos] != '\t')
        {
            pos++;
        }

        size_t tlen = pos - start;

        if (tlen == 0)
        {
            continue;
        }

        if (tlen >= sizeof(tok))
        {
            return false;
        }

        memcpy(tok, &line[start], tlen);
        tok[tlen] = '\0';

        /* "N:" ISO-TP row marker (first token only) */
        if (n_tok == 0 && tlen >= 2 && tok[tlen - 1] == ':' &&
            is_hex_str(tok, tlen - 1))
        {
            out->iso_index = (int)strtol(tok, NULL, 16);
            n_tok++;
            continue;
        }

        if (!is_hex_str(tok, tlen))
        {
            return false;
        }

        if (tlen == 2 || (tlen == 1 && n_tok > 0))
        {
            if (out->n >= sizeof(out->bytes))
            {
                return false;
            }

            out->bytes[out->n++] = (uint8_t)strtol(tok, NULL, 16);
        }
        else if (n_tok == 0)
        {
            /* first token, not a byte: CAN header (3 or 8 hex) or a bare
               ISO-TP length line ("014") */
            if (tlen == 3 || tlen == 8)
            {
                out->header = (uint32_t)strtoul(tok, NULL, 16);
                out->has_header = true;
            }

            out->bare_value = strtol(tok, NULL, 16);
            out->is_bare = true; /* provisional: bare iff no bytes follow */
        }
        else
        {
            return false; /* long hex token mid-line */
        }

        n_tok++;
    }

    if (out->n > 0 || out->iso_index >= 0)
    {
        out->is_bare = false;
    }

    return (out->n > 0) || out->is_bare || out->iso_index >= 0;
}

/* ap_resp_set_id29_hint: the row's own receive filter id and whether the
   protocol in effect is 29-bit (the runner sets both before it parses;
   the host tests set them by hand) */
static uint32_t s_hint_rx_id;
static bool     s_hint_proto29;

void ap_resp_set_id29_hint(uint32_t rx_id, bool proto29)
{
    s_hint_rx_id = rx_id;
    s_hint_proto29 = proto29;
}

/** A plausible ISO-TP PCI byte for the @p rest bytes that follow it. */
static bool pci_fits(uint8_t pci, size_t rest)
{
    switch (pci >> 4)
    {
        case 0x0:                       /* single frame: 1..7 bytes      */
            return (pci & 0x0F) >= 1 && (size_t)(pci & 0x0F) <= rest;

        case 0x1:                       /* first frame: length + data    */
            return rest >= 2;

        case 0x2:                       /* consecutive frame             */
            return true;

        default:
            return false;
    }
}

/** A whole ISO-TP frame as the chip prints it with headers on: a single
 *  frame trimmed to its length (or padded to eight), a first frame of
 *  eight bytes announcing at least eight, a consecutive frame of eight. */
static bool frame_whole(const uint8_t *d, size_t dn)
{
    switch (d[0] >> 4)
    {
        case 0x0:
        {
            size_t len = d[0] & 0x0F;

            return len >= 1 && len <= 7 && (dn == len + 1 || dn == 8);
        }

        case 0x1:
            return dn == 8 && ((((size_t)(d[0] & 0x0F)) << 8) | d[1]) >= 8;

        case 0x2:
            return dn == 8;

        default:
            return false;
    }
}

/**
 * With headers on the chip prints a 29-bit id as four byte tokens, and
 * until 2026-10-03 such a line was read as headers-off data: every 29-bit
 * answer failed the cross-talk guard (a DTC scan on a 29-bit car found no
 * ECU). Taken as an id when the bytes are `18 DA F1 xx` (an ECU answering
 * the tester F1 on ISO 15765-4 normal fixed addressing), or the id the
 * row's own receive filter names (the MEB profile's `ATCRA17FE007B`), or,
 * on a 29-bit protocol, any four bytes (the hint, 2026-10-24); in every
 * case a PCI byte that fits the rest of the line must follow, directly or
 * behind an extended address byte. A headers-off payload never starts with
 * 0x18: no service answers with it, and a raw first frame of 0x8DA bytes
 * does not exist on this chip.
 */
static void take_id29(ap_line_t *line)
{
    const uint8_t *b = line->bytes;

    if (line->has_header || line->iso_index >= 0 || line->n < 6)
    {
        return;
    }

    uint32_t id = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
                  ((uint32_t)b[2] << 8) | b[3];
    bool legislated = (b[0] == 0x18 && b[1] == 0xDA && b[2] == 0xF1);
    bool hinted = (s_hint_rx_id != 0 && id == s_hint_rx_id);

    if (!legislated && !hinted && !s_hint_proto29)
    {
        return;
    }

    /* a known id: the PCI after it, or after an extended address byte. The
       protocol hint alone wants a whole frame behind the four bytes (a
       single frame trimmed to its length, a first or a consecutive frame
       of eight), so a plain seven-byte reply is never mistaken for one */
    const uint8_t *d = b + 4;
    size_t dn = line->n - 4;
    bool fits;

    if (legislated || hinted)
    {
        fits = pci_fits(d[0], dn - 1) ||
               (dn >= 3 && (d[0] >> 4) > 0x3 && pci_fits(d[1], dn - 2));
    }
    else
    {
        fits = frame_whole(d, dn);
    }

    if (!fits)
    {
        return;
    }

    line->header = id;
    line->has_header = true;
    line->n -= 4;
    memmove(line->bytes, &line->bytes[4], line->n);
}

/**
 * ISO 15765-2 extended addressing (the BMW F/G scheme): every frame
 * carries the target address before the PCI, and with headers on the chip
 * prints it (`607 F1 10 09 62 E5 CE 20 6E`, bench 2026-10-24). A byte that
 * is no PCI (its high nibble above 3) followed by one that is, and fits
 * the rest, marks the line: the framing-free assembly and the pending
 * check start behind it; the frames view keeps it (B0 = F1, as the
 * legacy firmware and the published BMW rows count).
 */
static void take_ea(ap_line_t *line)
{
    const uint8_t *b = line->bytes;

    if (line->has_header && line->n >= 3 && (b[0] >> 4) > 0x3 &&
        pci_fits(b[1], line->n - 2))
    {
        line->ea = 1;
    }
}

/** `7F <service> 78`: the ECU asks for more time; the answer follows. */
static bool line_is_pending(const ap_line_t *line)
{
    const uint8_t *b = line->bytes;

    if (line->iso_index >= 0)
    {
        return false;
    }

    if (!line->has_header)
    {
        return line->n == 3 && b[0] == 0x7F && b[2] == 0x78;
    }

    /* headers on: a single frame of three bytes (padding may follow),
       behind an extended address byte when the line carries one */
    const uint8_t *d = b + line->ea;
    size_t dn = line->n - line->ea;

    return dn >= 4 && d[0] == 0x03 && d[1] == 0x7F && d[3] == 0x78;
}

static bool line_is_noise(const char *line, size_t len)
{
    static const char *const NOISE[] =
    {
        "SEARCHING", "BUS INIT", "OK", "STOPPED",
    };

    for (size_t i = 0; i < sizeof(NOISE) / sizeof(NOISE[0]); i++)
    {
        if (strncmp(line, NOISE[i], strlen(NOISE[i])) == 0)
        {
            return true;
        }
    }

    return false;
}

static bool line_is_error(const char *line, size_t len)
{
    static const char *const ERR[] =
    {
        "NO DATA", "ERROR", "CAN ERROR", "UNABLE TO CONNECT", "?",
    };

    for (size_t i = 0; i < sizeof(ERR) / sizeof(ERR[0]); i++)
    {
        if (strncmp(line, ERR[i], strlen(ERR[i])) == 0)
        {
            return true;
        }
    }

    return false;
}

/** Tokenize raw chip text into parsed data lines. @return line count;
 *  error lines set @p saw_error, a bare length line sets @p iso_total. */
int ap_resp_collect_lines(const char *resp, ap_line_t *lines, int max,
                          bool *saw_error, long *iso_total)
{
    int n_lines = 0;
    const char *p = resp;

    while (*p != '\0' && n_lines < max)
    {
        const char *eol = p;

        while (*eol != '\0' && *eol != '\r' && *eol != '\n')
        {
            eol++;
        }

        size_t len = (size_t)(eol - p);

        /* trim */
        while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '>'))
        {
            len--;
        }

        while (len > 0 && *p == ' ')
        {
            p++;
            len--;
        }

        if (len > 0)
        {
            char buf[AP_LINE_MAX];

            if (len >= sizeof(buf))
            {
                len = sizeof(buf) - 1;
            }

            memcpy(buf, p, len);
            buf[len] = '\0';

            if (line_is_error(buf, len))
            {
                *saw_error = true;
            }
            else if (!line_is_noise(buf, len))
            {
                ap_line_t parsed;

                if (parse_line(buf, len, &parsed))
                {
                    take_id29(&parsed);
                    take_ea(&parsed);

                    if (parsed.is_bare)
                    {
                        *iso_total = parsed.bare_value;
                    }
                    else if (!line_is_pending(&parsed))
                    {
                        lines[n_lines++] = parsed;
                    }
                }
                /* unparseable line (command echo remnants): skip */
            }
        }

        p = eol;

        while (*p == '\r' || *p == '\n')
        {
            p++;
        }
    }

    return n_lines;
}
