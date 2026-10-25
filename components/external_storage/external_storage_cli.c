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
 * @file external_storage_cli.c
 * @brief The component's CLI command (`sdcard`): registered into
 *        cmdline_manager by external_storage_register_cli() (the settings
 *        boot apply, behind the `cli` setting). Legacy option interface
 *        preserved (-i/--info, -t/--test); bare = the slot's state;
 *        --format --yes (2026-10-25) = the format of a card WiCAN cannot
 *        read, waited for and reported. FILESYSTEM usage lives in the `fs`
 *        command (filesystem owns that).
 */
#include <stdio.h>
#include <string.h>

#include "argtable3/argtable3.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cmdline_manager.h"

#include "external_storage.h"
#include "external_storage_private.h"

#define ES_TEST_FILE      "/sd/.wican_selftest"
#define ES_FORMAT_WAIT_MS 180000

static struct
{
    struct arg_lit *info;
    struct arg_lit *test;
    struct arg_lit *format;
    struct arg_lit *yes;
    struct arg_end *end;
} s_args;

static void print_card(const external_storage_status_t *st)
{
    if (!st->card_known)
    {
        return;
    }

    cmdline_printf("Card: %s %s %.2f GB (serial %08lx)\n", st->name, st->type,
                   (double)st->capacity_bytes / (1024.0 * 1024 * 1024),
                   (unsigned long)st->serial);
}

static int sdcard_status(void)
{
    external_storage_status_t st;

    if (external_storage_status(&st) != ESP_OK)
    {
        cmdline_printf("Error: no status\n");
        return 1;
    }

    cmdline_printf("Present: %s\n",
                   external_storage_is_present() ? "yes" : "no");
    cmdline_printf("Mounted: %s\n",
                   external_storage_is_mounted() ? "yes" : "no");
    cmdline_printf("State: %s%s%s%s\n", external_storage_state_str(st.state),
                   st.reason[0] != '\0' ? " (" : "", st.reason,
                   st.reason[0] != '\0' ? ")" : "");
    print_card(&st);

    if (st.state == EXTERNAL_STORAGE_UNREADABLE)
    {
        cmdline_printf("The card holds no FAT volume WiCAN can mount: "
                       "`sdcard --format --yes` formats it as FAT32 "
                       "(everything on it is erased).\n");
    }

    if (st.format.state != EXTERNAL_STORAGE_FORMAT_IDLE)
    {
        cmdline_printf("Last format: %s%s%s, %lu ms, %lu s ago\n",
                       external_storage_format_state_str(st.format.state),
                       st.format.error[0] != '\0' ? " " : "", st.format.error,
                       (unsigned long)st.format.took_ms,
                       (unsigned long)st.format.age_s);
    }

    cmdline_printf("OK\n");
    return 0;
}

static int sdcard_info(void)
{
    es_card_details_t card;

    if (!es_card_details(&card))
    {
        cmdline_printf("Error: Failed to read SD card info\n");
        return 1;
    }

    cmdline_printf("SD Card Info:\n");
    cmdline_printf("Name: %s\n", card.name);
    cmdline_printf("Type: %s\n", card.type);
    cmdline_printf("Capacity: %.2f GB\n",
                   (float)card.capacity_bytes / (1024.0f * 1024 * 1024));
    cmdline_printf("Sector Size: %d bytes\n", card.sector_size);
    cmdline_printf("Speed: %lu KHz\n", (unsigned long)card.speed_khz);
    cmdline_printf("OK\n");
    return 0;
}

/** Write/read-back/delete a marker file on the card (legacy -t). The
 *  card is SDMMC: no internal-flash cache involvement (§2-safe). */
static int sdcard_test(void)
{
    static const char PATTERN[] =
        "WiCAN SD self-test 0123456789 abcdefghijklmnopqrstuvwxyz";
    char readback[sizeof(PATTERN)];
    FILE *f = fopen(ES_TEST_FILE, "w");
    size_t n = 0;

    if (f != NULL)
    {
        n = fwrite(PATTERN, 1, sizeof(PATTERN), f);
        fclose(f);
    }

    if (f == NULL || n != sizeof(PATTERN))
    {
        cmdline_printf("Error: SD card test failed\n");
        return 1;
    }

    f = fopen(ES_TEST_FILE, "r");
    n = 0;

    if (f != NULL)
    {
        n = fread(readback, 1, sizeof(readback), f);
        fclose(f);
    }

    remove(ES_TEST_FILE);

    if (n != sizeof(PATTERN) ||
        memcmp(readback, PATTERN, sizeof(PATTERN)) != 0)
    {
        cmdline_printf("Error: SD card test failed\n");
        return 1;
    }

    cmdline_printf("SD card test passed successfully\n");
    cmdline_printf("OK\n");
    return 0;
}

/** `sdcard --format [--yes]`: the same gate as the HTTP route (the card
 *  must answer and hold no FAT volume), the warning without --yes, the
 *  request and its outcome with it. Runs on the dispatcher's internal
 *  stack; the detect task does the work. */
static int sdcard_format(bool yes)
{
    external_storage_status_t st;

    (void)external_storage_status(&st);

    if (st.state != EXTERNAL_STORAGE_UNREADABLE)
    {
        cmdline_printf("Error: %s\n",
                       st.state == EXTERNAL_STORAGE_ABSENT ? "no card in the slot"
                       : st.state == EXTERNAL_STORAGE_MOUNTED
                           ? "the card is mounted and in use: nothing to format"
                       : st.state == EXTERNAL_STORAGE_FAILED
                           ? "the card does not answer: it cannot be formatted"
                           : "a format is already running");
        return 1;
    }

    print_card(&st);
    cmdline_printf("Found: %s. A format writes FAT32 (%lu KB clusters, one "
                   "partition) and ERASES everything on the card.\n",
                   st.reason, (unsigned long)(st.cluster_bytes / 1024));

    if (!yes)
    {
        cmdline_printf("Run `sdcard --format --yes` to do it.\n");
        return 1;
    }

    esp_err_t err = external_storage_format_request();

    if (err != ESP_OK)
    {
        cmdline_printf("Error: %s\n", esp_err_to_name(err));
        return 1;
    }

    cmdline_printf("Formatting");

    for (uint32_t waited = 0; waited < ES_FORMAT_WAIT_MS; waited += 250)
    {
        vTaskDelay(pdMS_TO_TICKS(250));
        (void)external_storage_status(&st);

        if (waited % 2000 == 0)
        {
            cmdline_printf(".");
        }

        if (st.state != EXTERNAL_STORAGE_FORMATTING &&
            st.format.state != EXTERNAL_STORAGE_FORMAT_RUNNING)
        {
            break;
        }
    }

    cmdline_printf("\n");

    if (st.format.state == EXTERNAL_STORAGE_FORMAT_DONE &&
        st.state == EXTERNAL_STORAGE_MOUNTED)
    {
        cmdline_printf("Formatted in %lu ms: FAT32, mounted at /sd (%s %.2f GB)\n",
                       (unsigned long)st.format.took_ms, st.name,
                       (double)st.capacity_bytes / (1024.0 * 1024 * 1024));
        cmdline_printf("OK\n");
        return 0;
    }

    cmdline_printf("Error: format %s%s%s (state %s)\n",
                   external_storage_format_state_str(st.format.state),
                   st.format.error[0] != '\0' ? ": " : "", st.format.error,
                   external_storage_state_str(st.state));
    return 1;
}

static int cmd_sdcard(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&s_args);

    if (nerrors != 0)
    {
        arg_print_errors(stderr, s_args.end, argv[0]);
        return 1;
    }

    if (s_args.format->count > 0)
    {
        return sdcard_format(s_args.yes->count > 0);
    }

    if (s_args.info->count > 0)
    {
        return sdcard_info();
    }

    if (s_args.test->count > 0)
    {
        if (!external_storage_is_mounted())
        {
            cmdline_printf("Error: SD card not mounted\n");
            return 1;
        }

        return sdcard_test();
    }

    return sdcard_status();
}

esp_err_t external_storage_register_cli(void)
{
    static const esp_console_cmd_t CMD =
    {
        .command = "sdcard",
        .help = "SD card control and status",
        .hint = "Options: -i/--info, -t/--test, --format [--yes]",
        .func = cmd_sdcard,
        .argtable = &s_args,
    };

    s_args.info = arg_lit0("i", "info", "Get SD card information");
    s_args.test = arg_lit0("t", "test", "Test SD card read/write");
    s_args.format = arg_lit0(NULL, "format",
                             "Format a card WiCAN cannot read as FAT32 (erases it)");
    s_args.yes = arg_lit0("y", "yes", "Really format: without it only the warning");
    s_args.end = arg_end(4);
    return cmdline_manager_register(&CMD);
}
