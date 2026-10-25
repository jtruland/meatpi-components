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
 * @file es_probe.c
 * @brief Pure probe of what a card holds (2026-10-25): the boot record
 *        rules FatFs applies in check_fs() plus the exFAT and NTFS names,
 *        walked over sector 0 and the MBR's first partition. No IDF
 *        dependency: host-tested with synthetic disks. Also the state
 *        words the API and the CLI print.
 */
#include <string.h>

#include "external_storage.h"

#include "external_storage_private.h"

/* boot record offsets (FatFs ff.c names) */
#define BS_JMP_BOOT      0
#define BS_OEM_NAME      3
#define BPB_BYTS_PER_SEC 11
#define BPB_SEC_PER_CLUS 13
#define BPB_RSVD_SEC_CNT 14
#define BPB_NUM_FATS     16
#define BPB_ROOT_ENT_CNT 17
#define BPB_TOT_SEC16    19
#define BPB_FAT_SZ16     22
#define BPB_TOT_SEC32    32
#define BS_FIL_SYS_TYPE  54
#define BS_FIL_SYS_TYPE32 82
#define MBR_TABLE        446
#define BS_55AA          510

#define MIN_FAT12_SEC_VOL 128 /* FatFs: the smallest FAT12 volume */

static uint16_t ld16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t ld32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static bool pow2(uint32_t v)
{
    return v != 0 && (v & (v - 1)) == 0;
}

static bool all_bytes(const uint8_t *s, uint8_t v)
{
    for (size_t i = 0; i < ES_SECTOR_BYTES; i++)
    {
        if (s[i] != v)
        {
            return false;
        }
    }

    return true;
}

static bool blank_sector(const uint8_t *s)
{
    return all_bytes(s, 0x00) || all_bytes(s, 0xFF);
}

es_fs_kind_t es_probe_vbr(const uint8_t *s)
{
    uint16_t sign = ld16(s + BS_55AA);

    if (sign == 0xAA55 &&
        memcmp(s + BS_JMP_BOOT, "\xEB\x76\x90" "EXFAT   ", 11) == 0)
    {
        return ES_FS_EXFAT;
    }

    if (memcmp(s + BS_OEM_NAME, "NTFS    ", 8) == 0)
    {
        return ES_FS_NTFS;
    }

    uint8_t b = s[BS_JMP_BOOT];

    if (b == 0xEB || b == 0xE9 || b == 0xE8) /* a jump: a BIOS parameter block follows */
    {
        if (sign == 0xAA55 && memcmp(s + BS_FIL_SYS_TYPE32, "FAT32   ", 8) == 0)
        {
            return ES_FS_FAT;
        }

        /* FAT12/16 (early DOS volumes carry neither the signature nor the
           type string): the BPB must be plausible, FatFs's own test */
        uint16_t bps = ld16(s + BPB_BYTS_PER_SEC);
        uint8_t  spc = s[BPB_SEC_PER_CLUS];

        if (pow2(bps) && bps >= 512 && bps <= 4096 && pow2(spc) &&
            ld16(s + BPB_RSVD_SEC_CNT) != 0 &&
            (unsigned)s[BPB_NUM_FATS] - 1u <= 1u &&
            ld16(s + BPB_ROOT_ENT_CNT) != 0 &&
            (ld16(s + BPB_TOT_SEC16) >= MIN_FAT12_SEC_VOL ||
             ld32(s + BPB_TOT_SEC32) >= 0x10000) &&
            ld16(s + BPB_FAT_SZ16) != 0)
        {
            return ES_FS_FAT;
        }
    }

    return ES_FS_UNKNOWN;
}

void es_probe(es_read_sector_fn read, void *ctx, es_probe_t *out)
{
    uint8_t s0[ES_SECTOR_BYTES];

    memset(out, 0, sizeof(*out));
    out->kind = ES_FS_UNKNOWN;

    if (read == NULL || !read(ctx, 0, s0))
    {
        out->read_error = true;
        return;
    }

    if (blank_sector(s0))
    {
        out->kind = ES_FS_BLANK;
        return;
    }

    es_fs_kind_t k = es_probe_vbr(s0); /* a super-floppy card */

    if (k != ES_FS_UNKNOWN)
    {
        out->kind = k;
        return;
    }

    if (ld16(s0 + BS_55AA) != 0xAA55)
    {
        return; /* not a boot sector at all */
    }

    for (int i = 0; i < 4; i++) /* the MBR's first used partition */
    {
        const uint8_t *e = s0 + MBR_TABLE + 16 * i;
        uint8_t type = e[4];
        uint32_t lba = ld32(e + 8);
        uint32_t size = ld32(e + 12);

        if (type == 0 || size == 0)
        {
            continue;
        }

        out->mbr = true;
        out->part_type = type;
        out->part_lba = lba;

        if (type == 0xEE)
        {
            out->kind = ES_FS_GPT;
            return;
        }

        uint8_t vbr[ES_SECTOR_BYTES];

        if (lba == 0)
        {
            return; /* a table pointing at itself: damaged */
        }

        if (!read(ctx, lba, vbr))
        {
            out->read_error = true;
            return;
        }

        out->kind = blank_sector(vbr) ? ES_FS_BLANK : es_probe_vbr(vbr);
        return;
    }

    /* a signature with no partition: a disk with only a disk signature */
}

const char *es_fs_kind_str(es_fs_kind_t k)
{
    switch (k)
    {
        case ES_FS_FAT:         return "fat";
        case ES_FS_EXFAT:       return "exfat";
        case ES_FS_NTFS:        return "ntfs";
        case ES_FS_BLANK:       return "blank";
        case ES_FS_GPT:         return "gpt";
        case ES_FS_FAT_CORRUPT: return "fat_corrupt";
        default:                return "unknown";
    }
}

uint32_t es_format_cluster_bytes(uint64_t capacity_bytes)
{
    return capacity_bytes > (32ULL * 1024 * 1024 * 1024) ? 32768u : 16384u;
}

const char *external_storage_state_str(external_storage_state_t s)
{
    switch (s)
    {
        case EXTERNAL_STORAGE_MOUNTED:    return "mounted";
        case EXTERNAL_STORAGE_UNREADABLE: return "unreadable";
        case EXTERNAL_STORAGE_FAILED:     return "failed";
        case EXTERNAL_STORAGE_FORMATTING: return "formatting";
        default:                          return "absent";
    }
}

const char *external_storage_format_state_str(external_storage_format_state_t s)
{
    switch (s)
    {
        case EXTERNAL_STORAGE_FORMAT_RUNNING: return "running";
        case EXTERNAL_STORAGE_FORMAT_DONE:    return "done";
        case EXTERNAL_STORAGE_FORMAT_FAILED:  return "failed";
        default:                              return "idle";
    }
}
