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
 * @file autopid_chain.c
 * @brief PURE: what a ';'-separated chain of the tables (a type's init, a
 *        row's init, an AT command as a row's cmd) sets on the chip beyond
 *        the baseline, and the chain that puts the baseline back
 *        (host-tested, host_test/main/test_chain.c).
 *
 * Issue #964 (2026-09-29, a BMW G32): a custom row's init moved the chip
 * to the gateway (`ATSH6F1;ATCEA60;ATCRA660;ATFCSH6F1;ATFCSD60300000;
 * ATFCSM1`), the standard rows that followed went there too, the gateway
 * answered `7F 01 11` and every Mode 01 value froze. The runner restores
 * the baseline before the next row type when a chain moved any of this.
 * The prelude (`ATS1;ATH1;ATST96;ATTP<p>;ATSH<functional>;ATCRA` on CAN)
 * puts back the print format, the protocol, the request header and the
 * receive filter; the rest needs a command of its own, listed below. The
 * commands come from what the published vehicle profiles use (2026-10-20:
 * ATST, ATSP, ATSH, ATFCSM/SD/SH, ATAT, ATCRA, ATH, ATE, ATS, ATCEA, ATL,
 * ATR, ATCAF, ATCP) plus their neighbours in the ELM327 command set.
 *
 * A command is read as the chip reads it: spaces ignored, any case. A
 * command the table does not know sets nothing here (the bus guard rules
 * on the protocol commands, the chip driver's guard on the EEPROM ones).
 */
#include "autopid_private.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define CMD_MAX 32  /* a command of the tables, without spaces */

/** How a rule matches the command body (the text after "AT"). */
typedef enum
{
    MATCH_EXACT,    /* the body is the name                                 */
    MATCH_ARG,      /* the body starts with the name and carries an argument */
} match_t;

typedef struct
{
    const char *name;
    match_t     how;
    unsigned    sets;   /* 0 = a known command that leaves the baseline alone */
} rule_t;

/* First match wins: a longer name stands before the shorter one that
   prefixes it (CFC0 before CF, FCSM0 before FCSM, CAF0 before CF is not
   needed: CAF does not start with CF). */
static const rule_t RULES[] =
{
    { "FCSM0", MATCH_EXACT, 0 },                   /* the default mode   */
    { "FCSH",  MATCH_ARG,   AP_SETS_FLOW_CTL },
    { "FCSD",  MATCH_ARG,   AP_SETS_FLOW_CTL },
    { "FCSM",  MATCH_ARG,   AP_SETS_FLOW_CTL },
    { "CFC0",  MATCH_EXACT, AP_SETS_FC_OFF },
    { "CFC1",  MATCH_EXACT, 0 },
    { "CAF0",  MATCH_EXACT, AP_SETS_RAW },
    { "CAF1",  MATCH_EXACT, 0 },
    { "CEA",   MATCH_ARG,   AP_SETS_EXT_ADDR },    /* bare ATCEA = off    */
    { "CER",   MATCH_ARG,   AP_SETS_EXT_ADDR },
    { "CRA",   MATCH_ARG,   AP_SETS_FILTER },      /* bare ATCRA = reset  */
    { "CM",    MATCH_ARG,   AP_SETS_FILTER },
    { "CF",    MATCH_ARG,   AP_SETS_FILTER },
    { "CP",    MATCH_ARG,   AP_SETS_PRIORITY },
    { "SH",    MATCH_ARG,   AP_SETS_HEADER },
    { "SP",    MATCH_ARG,   AP_SETS_PROTOCOL },
    { "TP",    MATCH_ARG,   AP_SETS_PROTOCOL },
    { "ST",    MATCH_ARG,   AP_SETS_PRINT },
    { "H0",    MATCH_EXACT, AP_SETS_PRINT },     /* headers are on   */
    { "H1",    MATCH_EXACT, 0 },
    { "S0",    MATCH_EXACT, AP_SETS_PRINT },
    { "AT0",   MATCH_EXACT, AP_SETS_TIMING },
    { "AT2",   MATCH_EXACT, AP_SETS_TIMING },
    { "E1",    MATCH_EXACT, AP_SETS_ECHO },
    { "L1",    MATCH_EXACT, AP_SETS_LINEFEED },
    { "R0",    MATCH_EXACT, AP_SETS_QUIET },
    { "V1",    MATCH_EXACT, AP_SETS_DLC },
    { "D1",    MATCH_EXACT, AP_SETS_DLC },
};

/* What puts each bit back, after the prelude. */
static const struct
{
    unsigned    bit;
    const char *cmd;
} UNDO[] =
{
    { AP_SETS_EXT_ADDR, "ATCEA" },
    { AP_SETS_FLOW_CTL, "ATFCSM0" },
    { AP_SETS_FC_OFF,   "ATCFC1" },
    { AP_SETS_TIMING,   "ATAT1" },
    { AP_SETS_RAW,      "ATCAF1" },
    { AP_SETS_PRIORITY, "ATCP18" },
    { AP_SETS_ECHO,     "ATE0" },
    { AP_SETS_LINEFEED, "ATL0" },
    { AP_SETS_QUIET,    "ATR1" },
    { AP_SETS_DLC,      "ATV0;ATD0" },
};

/** One command (spaces already dropped, upper case) -> its bits. */
static unsigned command_sets(const char *body)
{
    for (size_t i = 0; i < sizeof(RULES) / sizeof(RULES[0]); i++)
    {
        const rule_t *r = &RULES[i];
        size_t n = strlen(r->name);

        if (strncmp(body, r->name, n) != 0)
        {
            continue;
        }

        if (r->how == MATCH_EXACT ? body[n] == '\0' : body[n] != '\0')
        {
            return r->sets;
        }
    }

    return 0;
}

unsigned ap_chain_sets(const char *chain)
{
    unsigned sets = 0;
    char cmd[CMD_MAX];
    size_t n = 0;
    bool overflow = false;

    if (chain == NULL)
    {
        return 0;
    }

    for (const char *p = chain;; p++)
    {
        char c = *p;

        if (c == ';' || c == '\0')
        {
            cmd[n] = '\0';

            /* an AT command only; a longer one is none of the table's */
            if (!overflow && n > 2 && cmd[0] == 'A' && cmd[1] == 'T')
            {
                sets |= command_sets(cmd + 2);
            }

            n = 0;
            overflow = false;

            if (c == '\0')
            {
                break;
            }
        }
        else if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        {
            continue;
        }
        else if (n < sizeof(cmd) - 1)
        {
            cmd[n++] = (char)toupper((unsigned char)c);
        }
        else
        {
            overflow = true;
        }
    }

    return sets;
}

size_t ap_chain_restore(unsigned sets, const char *prelude, char *out,
                        size_t cap)
{
    if (out == NULL || cap == 0)
    {
        return 0;
    }

    int n = snprintf(out, cap, "%s", (prelude != NULL) ? prelude : "");

    if (n < 0)
    {
        out[0] = '\0';
        return 0;
    }

    for (size_t i = 0; i < sizeof(UNDO) / sizeof(UNDO[0]); i++)
    {
        if ((sets & UNDO[i].bit) == 0)
        {
            continue;
        }

        if ((size_t)n >= cap)
        {
            break;
        }

        int m = snprintf(out + n, cap - (size_t)n, "%s%s",
                         (n > 0) ? ";" : "", UNDO[i].cmd);

        if (m < 0)
        {
            break;
        }

        n += m;
    }

    if ((size_t)n >= cap)
    {
        /* truncated: the whole chain did not fit, say so with the count */
        out[cap - 1] = '\0';
        return cap - 1;
    }

    return (size_t)n;
}

/** One element of a chain, spaces dropped and upper-cased, into @p out
 *  (cap - 1 characters at most). @return false when it did not fit. */
static bool element_body(const char *p, size_t len, char *out, size_t cap)
{
    size_t n = 0;

    for (size_t i = 0; i < len; i++)
    {
        char c = p[i];

        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        {
            continue;
        }

        if (n + 1 >= cap)
        {
            return false;
        }

        out[n++] = (char)toupper((unsigned char)c);
    }

    out[n] = '\0';
    return true;
}

int ap_chain_strip_print(char *chain)
{
    static const char *const DROP[] = { "ATH0", "ATS0", "ATE1" };
    int dropped = 0;

    if (chain == NULL)
    {
        return 0;
    }

    char *w = chain;
    const char *p = chain;

    while (*p != '\0')
    {
        const char *e = p;

        while (*e != '\0' && *e != ';')
        {
            e++;
        }

        size_t len = (size_t)(e - p);
        bool had_sep = (*e == ';');
        char body[8];
        bool drop = false;

        if (element_body(p, len, body, sizeof(body)))
        {
            for (size_t i = 0; i < sizeof(DROP) / sizeof(DROP[0]); i++)
            {
                if (strcmp(body, DROP[i]) == 0)
                {
                    drop = true;
                    break;
                }
            }
        }

        if (drop)
        {
            dropped++;

            /* the element goes with the separator after it; the last one,
               without a separator, takes the one before it */
            if (!had_sep && w > chain && w[-1] == ';')
            {
                w--;
            }
        }
        else
        {
            memmove(w, p, len);
            w += len;

            if (had_sep)
            {
                *w++ = ';';
            }
        }

        p = had_sep ? e + 1 : e;
    }

    *w = '\0';
    return dropped;
}

bool ap_chain_cra_id(const char *chain, uint32_t *id, bool *is_29bit)
{
    bool found = false;

    if (chain == NULL || id == NULL || is_29bit == NULL)
    {
        return false;
    }

    const char *p = chain;

    while (*p != '\0')
    {
        const char *e = p;

        while (*e != '\0' && *e != ';')
        {
            e++;
        }

        char body[CMD_MAX];

        if (element_body(p, (size_t)(e - p), body, sizeof(body)) &&
            strncmp(body, "ATCRA", 5) == 0)
        {
            const char *arg = body + 5;
            size_t alen = strlen(arg);
            bool hex = true;

            for (size_t i = 0; i < alen; i++)
            {
                if (!isxdigit((unsigned char)arg[i]))
                {
                    hex = false;
                }
            }

            if (alen == 0)
            {
                found = false;      /* a bare ATCRA: the filter reset */
            }
            else if (hex && (alen == 3 || alen == 8))
            {
                *id = (uint32_t)strtoul(arg, NULL, 16);
                *is_29bit = (alen == 8);
                found = true;
            }
        }

        p = (*e == ';') ? e + 1 : e;
    }

    return found;
}
