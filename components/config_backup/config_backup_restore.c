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
 * @file config_backup_restore.c
 * @brief The staged restore: ONE slot in PSRAM holding an uploaded file or
 *        a copy loaded from the device, unsealed when the key is known,
 *        indexed by part (ranges, no parse of the big strings), dry-run per
 *        part; the manifest the UI shows; the apply that writes the ticked
 *        parts and restarts. The slot lives ten minutes.
 */
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "config_backup_private.h"

static const char *TAG = "config_backup";

typedef struct
{
    bool        used;
    char        source[9];           /* file | internal | sd */
    char        name[64];
    char        copy_dir[80];        /* a copy: its folder, for a retry */
    int64_t     staged_at;
    char       *raw;                 /* an encrypted upload, until unsealed */
    size_t      raw_len;
    char       *doc;                 /* the plain document */
    size_t      doc_len;
    bool        encrypted;
    bool        redacted;
    bool        locked;
    bool        refused;
    bool        newer;
    bool        bad_key;
    unsigned    version;
    char        device_id[CB_ID_LEN];
    char        fw[CB_FW_LEN];
    char        created[CB_ISO_LEN];
    cb_member_t parts[CB_PART_COUNT];
    bool        present[CB_PART_COUNT];
    cJSON      *info;                /* per part counts (the dry run) */
    cJSON      *errors;
    uint8_t     unlock_key[CB_KEY_LEN];
    bool        have_unlock;
} stage_t;

static stage_t s_st EXT_RAM_BSS_ATTR;

static void clear_slot(void)
{
    free(s_st.raw);
    free(s_st.doc);
    cJSON_Delete(s_st.info);
    cJSON_Delete(s_st.errors);
    memset(&s_st, 0, sizeof(s_st));
}

void cb_restore_drop(void)
{
    clear_slot();
}

bool cb_restore_staged(void)
{
    if (!s_st.used)
    {
        return false;
    }

    int64_t now = cb_now_epoch();

    if (now > 0 && s_st.staged_at > 0 &&
        now - s_st.staged_at > CB_STAGE_LIFE_S)
    {
        ESP_LOGI(TAG, "staged backup expired");
        clear_slot();
        return false;
    }

    return true;
}

/* ---- the header and the parts ----------------------------------------------------------- */

static bool read_header(const char *buf, size_t len)
{
    cb_member_t m;

    if (!cb_json_find(buf, len, "wican_backup", &m))
    {
        return false;
    }

    long v = cb_json_long(&m);

    if (v < 1)
    {
        return false;
    }

    s_st.version = (unsigned)v;
    s_st.encrypted = cb_json_find(buf, len, "encrypted", &m) && cb_json_bool(&m);
    s_st.redacted = cb_json_find(buf, len, "redacted", &m) && cb_json_bool(&m);

    if (cb_json_find(buf, len, "device_id", &m))
    {
        (void)cb_json_string(&m, s_st.device_id, sizeof(s_st.device_id));
    }

    if (cb_json_find(buf, len, "fw_version", &m))
    {
        (void)cb_json_string(&m, s_st.fw, sizeof(s_st.fw));
    }

    if (cb_json_find(buf, len, "created", &m))
    {
        (void)cb_json_string(&m, s_st.created, sizeof(s_st.created));
    }

    return true;
}

static esp_err_t parts_cb(const cb_member_t *m, void *ctx)
{
    (void)ctx;

    int p = cb_part_from_name(m->key, m->key_len);

    if (p >= 0 && !(p == CB_PART_CERTS && strcmp(s_st.source, "file") == 0))
    {
        s_st.parts[p] = *m;
        s_st.present[p] = true;
    }

    return ESP_OK;
}

static void dry_run(void)
{
    cJSON_Delete(s_st.info);
    cJSON_Delete(s_st.errors);
    s_st.info = cJSON_CreateObject();
    s_st.errors = cJSON_CreateObject();

    cb_apply_opts_t o = { .redacted = s_st.redacted };

    for (int p = 0; p < CB_PART_COUNT; p++)
    {
        if (!s_st.present[p])
        {
            continue;
        }

        cJSON    *info = cJSON_AddObjectToObject(s_st.info, CB_PART_NAMES[p]);
        char      err[CB_ERR_LEN] = "";
        esp_err_t r = cb_check_part((cb_part_t)p, &s_st.parts[p], &o, info, err,
                                    sizeof(err));

        if (r == ESP_ERR_INVALID_VERSION)
        {
            s_st.newer = true;
        }

        if (r != ESP_OK)
        {
            cJSON_AddStringToObject(s_st.errors, CB_PART_NAMES[p],
                                    err[0] ? err : esp_err_to_name(r));
            ESP_LOGW(TAG, "staged backup: part %s rejected: %s",
                     CB_PART_NAMES[p], err[0] ? err : esp_err_to_name(r));
        }
    }

    s_st.refused = s_st.newer || cJSON_GetArraySize(s_st.errors) > 0;
}

static esp_err_t index_doc(void)
{
    cb_member_t m;

    memset(s_st.present, 0, sizeof(s_st.present));

    if (s_st.version == 1)
    {
        /* today's settings backup: the settings part alone */
        if (!cb_json_find(s_st.doc, s_st.doc_len, "components", &m))
        {
            return ESP_ERR_INVALID_ARG;
        }

        s_st.parts[CB_PART_SETTINGS] = m;
        s_st.present[CB_PART_SETTINGS] = true;
        return ESP_OK;
    }

    if (!cb_json_find(s_st.doc, s_st.doc_len, "parts", &m))
    {
        return ESP_ERR_INVALID_ARG;
    }

    return cb_json_members(m.val, m.val_len, parts_cb, NULL);
}

/* an encrypted upload: nonce + data (ciphertext||tag) in base64 */
static esp_err_t unseal_raw(const uint8_t *key)
{
    cb_member_t nonce_m;
    cb_member_t data_m;
    char        nonce_b64[32];

    if (!cb_json_find(s_st.raw, s_st.raw_len, "nonce", &nonce_m) ||
        !cb_json_find(s_st.raw, s_st.raw_len, "data", &data_m) ||
        !cb_json_string(&nonce_m, nonce_b64, sizeof(nonce_b64)) ||
        data_m.val_len < 2 || data_m.val[0] != '"')
    {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t nonce[CB_NONCE_LEN];
    size_t  nlen = 0;

    if (cb_b64_decode(nonce_b64, strlen(nonce_b64), nonce, sizeof(nonce),
                      &nlen) != ESP_OK || nlen != CB_NONCE_LEN)
    {
        return ESP_ERR_INVALID_ARG;
    }

    size_t   b64_len = data_m.val_len - 2;
    size_t   csize = (b64_len / 4) * 3 + 3;
    uint8_t *ct = heap_caps_malloc(csize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (ct == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    size_t clen = 0;

    if (cb_b64_decode(data_m.val + 1, b64_len, ct, csize, &clen) != ESP_OK ||
        clen < CB_TAG_LEN)
    {
        free(ct);
        return ESP_ERR_INVALID_ARG;
    }

    char  aad[96];
    char *plain = heap_caps_malloc(clen - CB_TAG_LEN + 1,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (plain == NULL)
    {
        free(ct);
        return ESP_ERR_NO_MEM;
    }

    snprintf(aad, sizeof(aad), "wican_backup:%u:%s:%s", s_st.version,
             s_st.device_id, s_st.created);

    size_t    plen = 0;
    esp_err_t err = cb_unseal(key, aad, nonce, ct, clen, (uint8_t *)plain,
                              clen - CB_TAG_LEN, &plen);

    free(ct);

    if (err != ESP_OK)
    {
        free(plain);
        return err;
    }

    plain[plen] = '\0';
    free(s_st.raw);
    s_st.raw = NULL;
    s_st.raw_len = 0;
    s_st.doc = plain;
    s_st.doc_len = plen;

    /* #986: the encrypted wrapper carries no `redacted`; the sealed document
       does. Without this, a redacted+encrypted backup restores its blanked
       secrets as "" (or is refused by the VPN check) and never shows the
       "passwords removed" notice. */
    cb_member_t red_m;

    if (cb_json_find(plain, plen, "redacted", &red_m) && cb_json_bool(&red_m))
    {
        s_st.redacted = true;
    }

    return ESP_OK;
}

/* the key for the staged backup: ours for our own, the unlock key for
   another device's, none = locked */
static const uint8_t *key_for_staged(void)
{
    if (strcmp(s_st.device_id, cb_device_id()) == 0)
    {
        return cb_device_key();
    }

    return s_st.have_unlock ? s_st.unlock_key : NULL;
}

static esp_err_t finish_file(void)
{
    if (s_st.version > CB_DOC_VERSION)
    {
        s_st.newer = true;
        s_st.refused = true;
        free(s_st.raw);
        s_st.raw = NULL;
        return ESP_OK;
    }

    if (s_st.encrypted && s_st.raw != NULL)
    {
        const uint8_t *key = key_for_staged();

        if (key == NULL)
        {
            s_st.locked = true;
            return ESP_OK;
        }

        esp_err_t err = unseal_raw(key);

        if (err == ESP_ERR_INVALID_CRC)
        {
            s_st.bad_key = true;
            s_st.locked = (key != cb_device_key());
            s_st.refused = !s_st.locked;
            return err;
        }

        if (err != ESP_OK)
        {
            s_st.refused = true;
            return err;
        }

        s_st.locked = false;
        s_st.bad_key = false;
    }

    if (index_doc() != ESP_OK)
    {
        s_st.refused = true;
        ESP_LOGW(TAG, "staged backup: no parts found");
        return ESP_ERR_INVALID_ARG;
    }

    dry_run();
    return ESP_OK;
}

esp_err_t cb_restore_stage_file(char *raw, size_t len, const char *name)
{
    clear_slot();
    s_st.used = true;
    s_st.staged_at = cb_now_epoch();
    snprintf(s_st.source, sizeof(s_st.source), "file");
    snprintf(s_st.name, sizeof(s_st.name), "%s", name ? name : "backup.json");

    if (!read_header(raw, len))
    {
        free(raw);
        clear_slot();
        return ESP_ERR_INVALID_ARG; /* not a WiCAN backup */
    }

    if (s_st.encrypted)
    {
        s_st.raw = raw;
        s_st.raw_len = len;
    }
    else
    {
        s_st.doc = raw;
        s_st.doc_len = len;
    }

    esp_err_t err = finish_file();

    return (err == ESP_ERR_INVALID_CRC) ? ESP_OK : err;
}

static esp_err_t load_copy(void)
{
    cb_manifest_t m;

    if (cb_copies_read_manifest(s_st.copy_dir, &m) != ESP_OK)
    {
        return ESP_ERR_NOT_FOUND;
    }

    snprintf(s_st.device_id, sizeof(s_st.device_id), "%s", m.device_id);
    snprintf(s_st.fw, sizeof(s_st.fw), "%s", m.fw);
    cb_iso_from_epoch(m.refreshed, s_st.created);
    s_st.version = CB_DOC_VERSION;
    s_st.encrypted = true;

    cb_buf_t doc;

    if (cb_buf_init(&doc, 32768) != ESP_OK)
    {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = cb_copies_load_doc(s_st.copy_dir, key_for_staged(), &m,
                                       &doc);

    if (err == ESP_ERR_INVALID_STATE)
    {
        cb_buf_free(&doc);
        s_st.locked = true;
        return ESP_OK;
    }

    if (err == ESP_ERR_INVALID_CRC)
    {
        cb_buf_free(&doc);
        s_st.bad_key = true;
        s_st.locked = (strcmp(s_st.device_id, cb_device_id()) != 0);
        s_st.refused = !s_st.locked;
        return ESP_ERR_INVALID_CRC;
    }

    if (err != ESP_OK)
    {
        cb_buf_free(&doc);
        s_st.refused = true;
        return err;
    }

    free(s_st.doc);
    s_st.doc = doc.data;
    s_st.doc_len = doc.len;
    s_st.locked = false;

    if (index_doc() != ESP_OK)
    {
        s_st.refused = true;
        return ESP_ERR_INVALID_ARG;
    }

    dry_run();
    return ESP_OK;
}

esp_err_t cb_restore_stage_copy(const char *where)
{
    char dir[80];
    char id[CB_ID_LEN];

    esp_err_t err = cb_copies_dir(where, dir, sizeof(dir), id);

    if (err != ESP_OK)
    {
        return err;
    }

    clear_slot();
    s_st.used = true;
    s_st.staged_at = cb_now_epoch();
    snprintf(s_st.source, sizeof(s_st.source), "%s", where);
    snprintf(s_st.copy_dir, sizeof(s_st.copy_dir), "%s", dir);
    snprintf(s_st.name, sizeof(s_st.name), "%s",
             strcmp(where, "sd") == 0 ? "The copy on the SD card"
                                      : "The copy on the internal flash");
    err = load_copy();

    if (err == ESP_ERR_NOT_FOUND)
    {
        clear_slot();
    }

    return (err == ESP_ERR_INVALID_CRC) ? ESP_OK : err;
}

esp_err_t cb_restore_unlock(const uint8_t key[CB_KEY_LEN])
{
    if (!cb_restore_staged())
    {
        return ESP_ERR_INVALID_STATE;
    }

    memcpy(s_st.unlock_key, key, CB_KEY_LEN);
    s_st.have_unlock = true;

    if (!s_st.locked)
    {
        return ESP_OK;
    }

    s_st.bad_key = false;

    if (strcmp(s_st.source, "file") == 0)
    {
        return finish_file();
    }

    return load_copy();
}

cJSON *cb_restore_manifest(void)
{
    if (!cb_restore_staged())
    {
        return NULL;
    }

    cJSON *j = cJSON_CreateObject();
    bool   other = strcmp(s_st.device_id, cb_device_id()) != 0;

    cJSON_AddStringToObject(j, "source", s_st.source);
    cJSON_AddStringToObject(j, "name", s_st.name);
    cJSON_AddStringToObject(j, "created", s_st.created);
    cJSON_AddStringToObject(j, "device_id", s_st.device_id);
    cJSON_AddStringToObject(j, "fw_version", s_st.fw);
    cJSON_AddNumberToObject(j, "version", s_st.version);
    cJSON_AddBoolToObject(j, "encrypted", s_st.encrypted);
    cJSON_AddBoolToObject(j, "redacted", s_st.redacted);
    cJSON_AddBoolToObject(j, "locked", s_st.locked);
    cJSON_AddBoolToObject(j, "refused", s_st.refused);
    cJSON_AddBoolToObject(j, "other_device", other);

    cJSON *w = cJSON_AddArrayToObject(j, "warnings");

    if (s_st.locked)
    {
        cJSON_AddItemToArray(w, cJSON_CreateString("key_missing"));
    }

    if (s_st.bad_key)
    {
        cJSON_AddItemToArray(w, cJSON_CreateString("bad_key"));
    }

    if (other && !s_st.locked)
    {
        cJSON_AddItemToArray(w, cJSON_CreateString("other_device"));
    }

    if (s_st.redacted)
    {
        cJSON_AddItemToArray(w, cJSON_CreateString("redacted"));
    }

    if (s_st.newer)
    {
        cJSON_AddItemToArray(w, cJSON_CreateString("newer"));
    }

    cJSON_AddItemToObject(j, "parts",
                          s_st.info ? cJSON_Duplicate(s_st.info, true)
                                    : cJSON_CreateObject());
    cJSON_AddItemToObject(j, "errors",
                          s_st.errors ? cJSON_Duplicate(s_st.errors, true)
                                      : cJSON_CreateObject());
    return j;
}

esp_err_t cb_restore_apply(uint32_t mask, bool keep_identity, bool keep_vpn,
                           cJSON *resp, char *err, size_t err_len)
{
    if (!cb_restore_staged())
    {
        snprintf(err, err_len, "nothing staged");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_st.locked || s_st.refused || s_st.doc == NULL)
    {
        snprintf(err, err_len, s_st.locked ? "the backup is locked"
                                           : "the backup was refused");
        return ESP_ERR_INVALID_STATE;
    }

    cb_apply_opts_t o =
    {
        .keep_identity = keep_identity,
        .keep_vpn = keep_vpn,
        .redacted = s_st.redacted,
    };
    cJSON *restored = cJSON_AddArrayToObject(resp, "restored");
    bool   any = false;

    for (int p = 0; p < CB_PART_COUNT; p++)
    {
        if ((mask & CB_MASK(p)) == 0 || !s_st.present[p])
        {
            continue;
        }

        esp_err_t r = cb_apply_part((cb_part_t)p, &s_st.parts[p], &o, err,
                                    err_len);

        if (r != ESP_OK)
        {
            ESP_LOGW(TAG, "restore: part %s failed: %s", CB_PART_NAMES[p], err);
            return r;
        }

        cJSON_AddItemToArray(restored, cJSON_CreateString(CB_PART_NAMES[p]));
        any = true;
    }

    if (!any)
    {
        snprintf(err, err_len, "no part to restore");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "restored from %s (%s): restarting", s_st.source,
             s_st.device_id);
    clear_slot();
    cJSON_AddBoolToObject(resp, "reboot", true);
    cb_schedule_restart();
    return ESP_OK;
}
