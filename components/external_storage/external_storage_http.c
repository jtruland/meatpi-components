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
 * @file external_storage_http.c
 * @brief The component's routes (2026-10-25): GET /api/sdcard = the slot's
 *        state in full, POST /api/sdcard/format = the format of a card
 *        WiCAN cannot read, behind the typed phrase. Reference:
 *        external_storage/HTTP_API.md; main calls
 *        external_storage_register_http() in HTTP compositions.
 */
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"

#include "http_server_manager.h"

#include "external_storage.h"

static const char *TAG = "external_storage";

#define ES_FORMAT_PHRASE "format sdcard"
#define ES_MOUNT_POINT   "/sd"

static esp_err_t send_json(httpd_req_t *req, cJSON *o, const char *status)
{
    char *txt = cJSON_PrintUnformatted(o);

    cJSON_Delete(o);

    if (txt == NULL)
    {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    if (status != NULL)
    {
        httpd_resp_set_status(req, status);
    }

    httpd_resp_set_type(req, "application/json");

    esp_err_t err = httpd_resp_send(req, txt, HTTPD_RESP_USE_STRLEN);

    cJSON_free(txt);
    return err;
}

static esp_err_t send_error(httpd_req_t *req, const char *status,
                            const char *msg)
{
    cJSON *o = cJSON_CreateObject();

    cJSON_AddStringToObject(o, "error", msg);
    return send_json(req, o, status);
}

static cJSON *status_doc(const external_storage_status_t *st)
{
    cJSON *o = cJSON_CreateObject();
    bool mounted = st->state == EXTERNAL_STORAGE_MOUNTED;

    cJSON_AddBoolToObject(o, "present", st->state != EXTERNAL_STORAGE_ABSENT);
    cJSON_AddBoolToObject(o, "mounted", mounted);
    cJSON_AddStringToObject(o, "state", external_storage_state_str(st->state));
    cJSON_AddStringToObject(o, "reason", st->reason);

    if (st->card_known)
    {
        cJSON *c = cJSON_AddObjectToObject(o, "card");
        char serial[12];

        snprintf(serial, sizeof(serial), "%08lx", (unsigned long)st->serial);
        cJSON_AddStringToObject(c, "name", st->name);
        cJSON_AddStringToObject(c, "type", st->type);
        cJSON_AddNumberToObject(c, "capacity_bytes", (double)st->capacity_bytes);
        cJSON_AddNumberToObject(c, "sector_size", st->sector_size);
        cJSON_AddNumberToObject(c, "speed_khz", st->speed_khz);
        cJSON_AddStringToObject(c, "serial", serial);
    }
    else
    {
        cJSON_AddNullToObject(o, "card");
    }

    if (mounted)
    {
        uint64_t total = 0;
        uint64_t free_b = 0;

        if (esp_vfs_fat_info(ES_MOUNT_POINT, &total, &free_b) == ESP_OK)
        {
            cJSON *f = cJSON_AddObjectToObject(o, "fs");

            cJSON_AddNumberToObject(f, "total", (double)total);
            cJSON_AddNumberToObject(f, "used", (double)(total - free_b));
        }
    }

    cJSON *f = cJSON_AddObjectToObject(o, "format");

    cJSON_AddStringToObject(f, "state",
                            external_storage_format_state_str(st->format.state));
    cJSON_AddStringToObject(f, "error", st->format.error);
    cJSON_AddNumberToObject(f, "took_ms", st->format.took_ms);
    cJSON_AddNumberToObject(f, "age_s", st->format.age_s);
    cJSON_AddNumberToObject(f, "cluster_kb", st->cluster_bytes / 1024);
    cJSON_AddStringToObject(f, "phrase", ES_FORMAT_PHRASE);
    return o;
}

/* GET /api/sdcard */
static esp_err_t sdcard_handler(httpd_req_t *req)
{
    external_storage_status_t st;

    if (external_storage_status(&st) != ESP_OK)
    {
        return send_error(req, "500 Internal Server Error", "no status");
    }

    return send_json(req, status_doc(&st), NULL);
}

/* POST /api/sdcard/format {"confirm":"format sdcard"}: 202 and the detect
 * task formats; 400 without the phrase; 409 for a slot the format cannot
 * help (no card, a card that does not answer, a mounted card: nothing is
 * ever erased that WiCAN can read) or a format already running. */
static esp_err_t format_handler(httpd_req_t *req)
{
    char buf[128];
    int len = httpd_req_recv(req, buf, (req->content_len < sizeof(buf) - 1)
                                           ? req->content_len
                                           : sizeof(buf) - 1);
    bool confirmed = false;

    if (len > 0)
    {
        buf[len] = '\0';

        cJSON *in = cJSON_Parse(buf);
        const cJSON *c = cJSON_GetObjectItemCaseSensitive(in, "confirm");

        confirmed = cJSON_IsString(c) &&
                    strcmp(c->valuestring, ES_FORMAT_PHRASE) == 0;
        cJSON_Delete(in);
    }

    if (!confirmed)
    {
        return send_error(req, "400 Bad Request",
                          "confirm with {\"confirm\":\"" ES_FORMAT_PHRASE "\"}");
    }

    external_storage_status_t st;

    (void)external_storage_status(&st);

    esp_err_t err = external_storage_format_request();

    if (err == ESP_ERR_NOT_FINISHED)
    {
        return send_error(req, "409 Conflict", "a format is already running");
    }

    if (err != ESP_OK)
    {
        switch (st.state)
        {
            case EXTERNAL_STORAGE_ABSENT:
                return send_error(req, "409 Conflict", "no card in the slot");
            case EXTERNAL_STORAGE_MOUNTED:
                return send_error(req, "409 Conflict",
                                  "the card is mounted and in use: nothing to format");
            case EXTERNAL_STORAGE_FAILED:
                return send_error(req, "409 Conflict",
                                  "the card does not answer: it cannot be formatted");
            default:
                return send_error(req, "409 Conflict", esp_err_to_name(err));
        }
    }

    ESP_LOGI(TAG, "format requested over HTTP");

    cJSON *o = cJSON_CreateObject();

    cJSON_AddStringToObject(o, "state", "formatting");
    return send_json(req, o, "202 Accepted");
}

esp_err_t external_storage_register_http(void)
{
    static const httpd_uri_t URIS[] =
    {
        { .uri = "/api/sdcard", .method = HTTP_GET,
          .handler = sdcard_handler },
        { .uri = "/api/sdcard/format", .method = HTTP_POST,
          .handler = format_handler },
    };

    esp_err_t err = http_server_manager_register_handlers(
        URIS, sizeof(URIS) / sizeof(URIS[0]));

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "/api/sdcard registered");
    }

    return err;
}
