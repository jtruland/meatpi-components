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
 * @file external_storage_private.h
 * @brief Internal API + the pure parts (host-testable, no GPIO, no SDMMC):
 *        the detect-pin debouncer (es_detect.c) and the probe of what is on
 *        a card before any mount is tried (es_probe.c, 2026-10-25).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* consecutive identical samples required to accept a state change:
 * SD sockets bounce on insertion, and 4 × 250 ms also rides out the
 * card seating wobble */
#define ES_DEBOUNCE_SAMPLES 4

typedef struct
{
    bool stable_present;   /* the debounced truth                       */
    bool candidate;        /* level being counted toward a change       */
    int  count;            /* consecutive samples of `candidate`        */
} es_detect_t;

/** Start with an assumed state (first real sample overrides quickly). */
void es_detect_init(es_detect_t *d, bool initial_present);

/**
 * Feed one raw sample; returns true when the DEBOUNCED state just changed
 * (read the new state from d->stable_present). Glitches shorter than
 * ES_DEBOUNCE_SAMPLES never surface.
 */
bool es_detect_feed(es_detect_t *d, bool raw_present);

/* ---- what is on the card: the probe (pure, es_probe.c) -------------------- */

#define ES_SECTOR_BYTES 512

typedef enum
{
    ES_FS_FAT = 0,      /**< a FAT12/16/32 volume FatFs would mount            */
    ES_FS_EXFAT,        /**< exFAT: the factory format of cards over 32 GB     */
    ES_FS_NTFS,         /**< NTFS                                               */
    ES_FS_BLANK,        /**< sector 0 (or the partition) all 0x00 or all 0xFF  */
    ES_FS_GPT,          /**< a GPT disk (protective MBR entry 0xEE)            */
    ES_FS_UNKNOWN,      /**< something else, or a damaged boot sector          */
    ES_FS_FAT_CORRUPT,  /**< looked like FAT, the mount refused it (the glue)  */
} es_fs_kind_t;

/** Read one 512-byte sector into @p out; false on a read error. */
typedef bool (*es_read_sector_fn)(void *ctx, uint32_t lba, uint8_t *out);

typedef struct
{
    es_fs_kind_t kind;
    bool         mbr;        /**< a partition table was found on sector 0     */
    uint8_t      part_type;  /**< its first used entry's type byte             */
    uint32_t     part_lba;   /**< and first sector (the boot record judged)    */
    bool         read_error; /**< a sector could not be read                   */
} es_probe_t;

/**
 * Walk the card the way FatFs's find_volume() does: sector 0 as a volume
 * boot record (a super-floppy card), else the MBR's first used partition and
 * its boot record. Pure: the sector reader is injected, so the host test
 * feeds synthetic disks. The result says what the card holds in words the
 * web UI can show BEFORE a mount is attempted, which is how an exFAT card
 * costs one W line instead of three E lines and a boot_errors fault.
 */
void es_probe(es_read_sector_fn read, void *ctx, es_probe_t *out);

/**
 * Classify one boot record: FatFs's check_fs() rules for FAT (the FAT32
 * signature behind a jump opcode, or a plausible FAT12/16 BPB), the exFAT
 * and NTFS OEM names. ES_FS_UNKNOWN for anything else.
 */
es_fs_kind_t es_probe_vbr(const uint8_t *vbr);

/** The wire spelling of a kind: "fat" | "exfat" | "ntfs" | "blank" | "gpt"
 *  | "unknown" | "fat_corrupt". */
const char *es_fs_kind_str(es_fs_kind_t k);

/**
 * The allocation unit a format uses for a card of this size: 32 KB above
 * 32 GB (a quarter of the FAT entries, a faster format, the Windows default
 * for FAT32 of that size), 16 KB otherwise (the mount hint the component
 * always used).
 */
uint32_t es_format_cluster_bytes(uint64_t capacity_bytes);

/** Mounted or probed card's identity for the `sdcard` CLI (-i/--info). */
typedef struct
{
    char        name[8];        /* CID product name                    */
    const char *type;           /* "SDHC/SDXC" / "SDSC" / "MMC" / "SDIO" */
    uint64_t    capacity_bytes;
    int         sector_size;
    uint32_t    speed_khz;
} es_card_details_t;

/** Fill @p out from the card in the slot (mounted or merely answering).
 *  False when no card answered. */
bool es_card_details(es_card_details_t *out);

#ifdef __cplusplus
}
#endif
