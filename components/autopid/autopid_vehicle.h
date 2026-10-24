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
 * @file autopid_vehicle.h
 * @brief PRIVATE: the vehicle store (TASK_quick_setup.md, second pass).
 *        Pure contracts (autopid_vehicle_core.c: reply parsers and the
 *        fingerprint; autopid_vehicle_index.c: the index operations and
 *        its JSON, both host-tested) and the target store
 *        (autopid_vehicle.c + autopid_vehicle_switch.c). Included by
 *        autopid_private.h; never include it directly from outside.
 *
 * One entry per car, keyed by its VIN (or `fp:<8 hex>` when the car has
 * none), at most AP_VEH_MAX of them, the least recently seen evicted:
 * /data/autopid/vehicles.json is the index, /data/autopid/vehicles/
 * <key>.json each car's PID tables (the config.json shape). The active
 * config.json is a COPY of the current car's tables: the poller and every
 * existing route never learn about the store. With `std_protocol` = "0"
 * the chip prelude pins the current car's learned protocol.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- pure: reply parsers + fingerprint (autopid_vehicle_core.c) ---------- */

#define AP_VIN_LEN       18     /* 17 chars + NUL                        */
#define AP_FP_LEN        9      /* 8 hex chars + NUL                     */
#define AP_VEH_PROTO_LEN 2      /* one ELM protocol char + NUL           */
#define AP_VEH_JSON_MAX  320    /* the first-pass vehicle.json bound     */

/** The dialect a car's legislated diagnostics speak: what the standard
 *  PIDs, the VIN and the trouble codes are asked with. Requests, parsers
 *  and addressing live in autopid_dialect.h. */
typedef enum
{
    AP_DIALECT_OBD2 = 0,    /* SAE J1979: services 01..0A                */
    AP_DIALECT_UDS,         /* ISO 27145 / SAE J1979-2: service 22, F4xx */
    AP_DIALECT_J1939,       /* SAE J1939: broadcast, no request rows     */
} ap_dialect_t;

/** The first-pass single document (vehicle.json, version 1). Kept only
 *  so ap_vehicle_load() can import it into the store once. */
typedef struct
{
    char    vin[AP_VIN_LEN];               /* "" = unknown              */
    char    protocol[AP_VEH_PROTO_LEN];    /* "" = none stored          */
    char    fingerprint[AP_FP_LEN];        /* "" = none                 */
    char    seen_vin[AP_VIN_LEN];
    char    seen_fingerprint[AP_FP_LEN];
    int64_t detected_ts;                   /* epoch s, 0 = clock unset  */
    bool    changed;
} ap_vehicle_doc_t;

typedef struct
{
    uint32_t id;         /* responder CAN id; UINT32_MAX = headers off   */
    uint32_t bitmap;     /* its support bitmap of range 00 (the answer
                            to `0100`, or to `22F400`)                   */
} ap_veh_ecu_t;

/** ATDPN reply -> protocol char: "A6" / "6" / "A8>" -> "6"/"8". Only
 *  1..9 and A..C are accepted; a leading 'A' is the auto-detected flag
 *  (always present after ATTP0), so a bare "A" is rejected. */
bool ap_veh_parse_dpn(const char *reply, char out[AP_VEH_PROTO_LEN]);

/** 1..9 or A..C (upper case). */
bool ap_veh_proto_valid(char c);

/** 17 chars, A-Z0-9 without I/O/Q. */
bool ap_veh_vin_valid(const char *vin);

/** VIN from a 0902 reply as the chip prints it (single line, headers-off
 *  "014/0:/1:/2:" ISO-TP rows, headers-on multi-frame): `49 02 01` then
 *  17 ASCII bytes. False on error lines, short/padded or invalid VINs. */
bool ap_veh_parse_vin_0902(const char *resp, char vin[AP_VIN_LEN]);

/** VIN from a UDS 22F190 reply: `62 F1 90` + 17 bytes. */
bool ap_veh_parse_vin_22f190(const char *resp, char vin[AP_VIN_LEN]);

/** VIN from the 22F802 reply of an ISO 27145 / J1979-2 vehicle:
 *  `62 F8 02` + 17 bytes; a count byte `01` before the text (the 0902
 *  habit) is skipped. */
bool ap_veh_parse_vin_22f802(const char *resp, char vin[AP_VIN_LEN]);

/** Responder table from a headers-on 0100 reply: one entry per CAN id
 *  (headers off = one UINT32_MAX entry, rows OR-merged). The obd2 case of
 *  ap_dialect_bitmaps(), which also reads the chip's 29-bit print.
 *  @return entry count (0 = nothing parsable). */
int ap_veh_ecus_from_0100(const char *resp, ap_veh_ecu_t *out, size_t max);

/** FNV-1a 32 over the (id, bitmap) pairs sorted by id (duplicates OR-
 *  merged) as 8 lowercase hex chars; "" when n == 0. Order independent. */
void ap_veh_fingerprint(const ap_veh_ecu_t *ecus, size_t n,
                        char out[AP_FP_LEN]);

/** Which protocol the chip prelude pins: a pinned setting (6..9) wins;
 *  setting "0" uses the stored char unless @p search_fallback; else '0'
 *  (chip auto search). */
char ap_veh_effective_protocol(const char *setting, const char *stored,
                               bool search_fallback);

/** The ';'-separated chip prelude for @p proto: ATS1;ATH<h>;ATST96;ATTP<p>
 *  plus the functional header + CRA clear for the CAN protocols 6..9.
 *  Headers are ON for every CAN protocol (6..9, A..C; 2026-10-24, issue
 *  #1014: the replies are read as the chip prints them with the id) and
 *  off for K-line / J1850 (1..5) and the auto search. */
const char *ap_veh_prelude_for(char proto);

/** 29-bit CAN protocols (7, 9, A..C). */
bool ap_veh_proto_is_29bit(char proto);

/** The chip command that puts the FUNCTIONAL request header of an ISO
 *  15765-4 protocol back ("ATSH7DF" for 6 and 8, "ATSH18DB33F1" for 7 and
 *  9); NULL for the search and every other protocol (the caller sends the
 *  whole prelude instead). */
const char *ap_veh_func_header(char proto);

/** The first-pass document from its JSON (import only). Resets @p out
 *  and returns false on garbage / another version. */
bool ap_veh_doc_from_json(const char *json, ap_vehicle_doc_t *out);
bool ap_veh_doc_empty(const ap_vehicle_doc_t *doc);

/* ---- pure: the store index (autopid_vehicle_index.c) ----------------------- */

#define AP_VEH_MAX            8     /* cars kept; the 9th evicts the LRU   */
#define AP_VEH_KEY_LEN        18    /* VIN, or "fp:" + 8 hex, + NUL        */
#define AP_VEH_NAME_LEN       32
#define AP_VEH_PROFILE_LEN    64
#define AP_VEH_INIT_LEN       96    /* = AP_INIT_LEN                       */
#define AP_VEH_ECUS_MAX       8     /* responders kept per car             */
#define AP_VEH_ECUS_STR_LEN   (AP_VEH_ECUS_MAX * 18 + 1) /* "id:bitmap,"  */
#define AP_VEH_TOUCH_PERIOD_S 86400 /* last_seen written once a day        */
#define AP_VEH_INDEX_JSON_MAX 6144  /* vehicles.json upper bound           */

typedef struct
{
    char         key[AP_VEH_KEY_LEN];        /* fixed at first contact    */
    char         vin[AP_VIN_LEN];            /* "" = no VIN readable      */
    char         fingerprint[AP_FP_LEN];     /* over ecus[], "" = none    */
    char         name[AP_VEH_NAME_LEN];      /* user label                */
    char         protocol[AP_VEH_PROTO_LEN]; /* detected base protocol    */
    char         chip_protocol[AP_VEH_PROTO_LEN]; /* last ATSP saved      */
    uint8_t      dialect;                    /* ap_dialect_t              */
    bool         j1939;                      /* the vehicle network is
                                                J1939: alone (dialect
                                                j1939) or beside the OBD
                                                dialect (an EU truck)     */
    char         profile[AP_VEH_PROFILE_LEN];/* "" = none                 */
    char         specific_init[AP_VEH_INIT_LEN];
    ap_veh_ecu_t ecus[AP_VEH_ECUS_MAX];      /* the fingerprint's inputs  */
    uint8_t      n_ecus;
    uint16_t     std_supported;              /* standard PIDs found       */
    bool         pending_profile;            /* profile step still due    */
    int64_t      first_seen;                 /* epoch s, 0 = clock unset  */
    int64_t      last_seen;
    int64_t      scan_ts;                    /* last detection            */
} ap_veh_entry_t;

typedef struct
{
    ap_veh_entry_t v[AP_VEH_MAX];
    uint8_t        n;
    int8_t         current;                  /* index, -1 = none          */
} ap_veh_index_t;

void ap_vidx_init(ap_veh_index_t *idx);

/** The key for a car: the VIN, else "fp:<fingerprint>", else "". */
void ap_vidx_key_for(const char *vin, const char *fp,
                     char out[AP_VEH_KEY_LEN]);
/** A VIN (17 chars) or "fp:" + 8 lowercase hex: safe as a file name. */
bool ap_vidx_key_valid(const char *key);
/** "<WMI> <last 4 of the VIN>" or "Car <first 4 hex of the fingerprint>". */
void ap_vidx_default_name(const char *vin, const char *fp,
                          char out[AP_VEH_NAME_LEN]);

int ap_vidx_find_key(const ap_veh_index_t *idx, const char *key);
int ap_vidx_find_vin(const ap_veh_index_t *idx, const char *vin);
int ap_vidx_find_fp(const ap_veh_index_t *idx, const char *fp);

/** The SUBSET rule: the same car when the main ECU (lowest id) answers
 *  with the same bitmap in both sets and one set is a subset of the
 *  other (an EV in accessory mode shows fewer ECUs than when ready). */
bool ap_veh_ecus_subset(const ap_veh_ecu_t *a, size_t na,
                        const ap_veh_ecu_t *b, size_t nb);

/** Which entry is this car: a VIN decides alone (an entry learned
 *  without its VIN is adopted when the fingerprint says so); without a
 *  VIN the fingerprint exactly, then the subset rule over the ECU sets.
 *  @param[out] exact false when the subset rule matched (may be NULL).
 *  @return entry index or -1. */
int ap_vidx_match(const ap_veh_index_t *idx, const char *vin,
                  const ap_veh_ecu_t *ecus, size_t n_ecus, const char *fp,
                  bool *exact);

/** The eviction pick: the least recently seen entry that is not the
 *  current one (ties: oldest first_seen, then the lowest index). -1 when
 *  nothing can go. */
int ap_vidx_lru(const ap_veh_index_t *idx);

/** Append @p e (its key must be set). A full index evicts ap_vidx_lru()
 *  first and copies it into @p evicted (key "" when nothing was evicted;
 *  may be NULL). Fixes `current`. @return the new entry's index, -1 when
 *  the key already exists or nothing could be evicted. */
int ap_vidx_add(ap_veh_index_t *idx, const ap_veh_entry_t *e,
                ap_veh_entry_t *evicted);

/** Remove entry @p i (fixes `current`; a removed current = -1). */
void ap_vidx_remove(ap_veh_index_t *idx, int i);

/** Change-guarded last_seen: true (write it) only when the clock is set
 *  and last_seen is unset or a day old; fills an unset first_seen too. */
bool ap_vidx_touch(ap_veh_entry_t *e, int64_t now);

/** `{"version":1,"current":"<key>","vehicles":[...]}` <-> the index.
 *  from_json resets @p out and returns false on garbage / another
 *  version; it keeps at most AP_VEH_MAX entries, drops entries without a
 *  valid key or with a duplicate one and sanitizes VIN/protocol fields.
 *  to_json returns the length, or -1 when @p cap is too small. */
bool ap_vidx_from_json(const char *json, ap_veh_index_t *out);
int  ap_vidx_to_json(const ap_veh_index_t *idx, char *out, size_t cap);

/** "7E8:BE7FB813,7E9:80000001" <-> the responder table (upper hex,
 *  headers-off ids print as "*"). @return entry count. */
int ap_veh_ecus_to_str(const ap_veh_ecu_t *ecus, size_t n, char *out,
                       size_t cap);
int ap_veh_ecus_from_str(const char *s, ap_veh_ecu_t *out, size_t max);

/** One entry as the API/file JSON object (plus "current" when
 *  @p with_current). Caller owns the result (cJSON; the header stays
 *  free of cJSON.h for the pure half). */
struct cJSON;
struct cJSON *ap_vidx_entry_json(const ap_veh_entry_t *e,
                                 bool with_current, bool current);

#ifndef AUTOPID_HOST_TEST
#include "cJSON.h"

/* ---- the target store (autopid_vehicle.c, autopid_vehicle_switch.c) -------
 * RAM copy of the index under one lock; every file operation runs on an
 * INTERNAL stack: the httpd task and the scan task directly, the poller
 * through the one-shot `apid_veh` worker (standard §2). ------------------ */

/** What first contact found (the runner) or the detection job measured. */
typedef struct
{
    char         protocol[AP_VEH_PROTO_LEN]; /* "" = not (re)detected     */
    char         vin[AP_VIN_LEN];            /* "" = none                 */
    ap_veh_ecu_t ecus[AP_VEH_ECUS_MAX];
    uint8_t      n_ecus;
    uint8_t      dialect;                    /* ap_dialect_t the identity
                                                requests were answered in */
    bool         j1939;                      /* the listener (or the bus
                                                probe) saw a J1939 network */
    uint16_t     std_supported;              /* detection job only        */
} ap_veh_seen_t;

typedef enum
{
    AP_VEH_RES_NONE = 0,  /* nothing identifiable answered: no change     */
    AP_VEH_RES_SAME,      /* the current car (last_seen touched)          */
    AP_VEH_RES_SWITCHED,  /* another known car: the switch is under way   */
    AP_VEH_RES_NEW,       /* an unknown car: run the detection job        */
} ap_veh_result_t;

void ap_vehicle_load(void);                 /* autopid_init (main task)  */
/** The current car's protocol char as a string ("6".."9", "1".."5",
 *  "A".."C") or "" when there is no current car or none learned.
 *  Lock-free read of a 2-byte static. */
const char *autopid_vehicle_protocol(void);
/** The current car's dialect (obd2 when there is no current car).
 *  Lock-free read of a one-byte static. */
ap_dialect_t autopid_vehicle_dialect(void);
/** Is the current car's network J1939 (its `j1939` flag; false when there
 *  is no current car)? Lock-free read of a one-byte static. */
bool autopid_vehicle_j1939(void);
/** Point the runner's SPECIFIC type init at the current car's
 *  `specific_init` (the settings value when the car has none). Called
 *  from settings on_apply and after every switch/edit. */
void ap_vehicle_apply_type_init(void);

/** GET /api/autopid/vehicles. Caller frees. */
cJSON *autopid_vehicles_json(void);
/** One entry (+ "current") or NULL when @p key is unknown. Caller frees. */
cJSON *autopid_vehicle_entry_json(const char *key);
/** PUT /api/autopid/vehicles/<key> (internal-stack caller): NULL leaves a
 *  field alone; `profile` set (even "") clears `pending_profile`, and ""
 *  also clears `specific_init`; the SPECIFIC init applies live when the
 *  car is current. ESP_ERR_NOT_FOUND for an unknown key. */
esp_err_t autopid_vehicle_update(const char *key, const char *name,
                                 const char *profile,
                                 const char *specific_init);
/** POST .../activate (internal-stack caller): the switch by hand, the
 *  same path the automatic switch takes. ESP_ERR_NOT_FOUND unknown key. */
esp_err_t autopid_vehicle_activate(const char *key);
/** DELETE .../<key> (internal-stack caller): entry + its tables file;
 *  deleting the current car keeps config.json and clears `current`. */
esp_err_t autopid_vehicle_delete(const char *key);
/** autopid_config_save() mirror (internal-stack caller): the saved
 *  tables also go into the current car's file when they differ. */
void ap_vehicle_config_saved(const char *json, size_t len);

/** First contact per boot (poller task: PSRAM stack, never blocks on
 *  flash). SAME touches last_seen (daily) and records a re-detected
 *  protocol; SWITCHED set `current` already and queued the file work
 *  (snapshot, copy, reload, init, event) on the worker; NEW stored
 *  nothing: the caller starts the detection job. */
ap_veh_result_t autopid_vehicle_seen(const ap_veh_seen_t *seen);

/** The detection job's result (scan task: internal stack). A known car
 *  gets its record refreshed (and the switch when it is not current); an
 *  unknown car becomes a new entry (`pending_profile`, LRU eviction)
 *  whose tables are @p std_config (the scanned standard rows, config.json
 *  shape) copied into config.json; then the chip learns the base
 *  protocol once (obd_chip_protocol_save) when the entry's chip_protocol
 *  differs. @p out_entry / @p out_known describe the result. */
esp_err_t autopid_vehicle_detected(const ap_veh_seen_t *seen,
                                   const char *std_config, size_t len,
                                   ap_veh_entry_t *out_entry,
                                   bool *out_known);

/* ---- runner-side identity hooks (autopid_runner.c) ------------------------ */
void ap_runner_poll_result(bool ok);        /* poller: after every poll  */
bool ap_runner_proto_fallback(void);        /* stored protocol silent:
                                               ATTP0 this boot           */
/** Nothing scheduled this cycle: while the per-boot identity check has
 *  not run yet, probe the bus (one 0100 every 10 s) so first contact
 *  happens on a device with empty tables too. Poller-task context. */
void ap_runner_idle(void);
/** The current car changed (store switch / detection): re-send the boot
 *  prelude before the next poll so it pins that car's protocol. Any
 *  task (plain flag writes). */
void ap_runner_rebaseline(void);

/* ---- store internals shared by autopid_vehicle.c and
 * autopid_vehicle_switch.c (not for the rest of the component) --------- */
#define AP_VEH_INDEX_PATH   "/data/autopid/vehicles.json"
#define AP_VEH_DIR          "/data/autopid/vehicles"
#define AP_VEH_LEGACY_PATH  "/data/autopid/vehicle.json"
#define AP_VEH_EMPTY_CONFIG "{\"groups\":[],\"pids\":[],\"filters\":[]}"

void ap_veh_lock(void);
void ap_veh_unlock(void);
ap_veh_index_t *ap_veh_index(void);         /* under the lock only       */
void ap_veh_refresh_protocol_cache(void);   /* under the lock            */
int64_t ap_veh_epoch_now(void);             /* 0 when the clock is unset */
void ap_veh_car_path(const char *key, char *out, size_t cap);
esp_err_t ap_veh_persist(void);             /* internal-stack caller     */
void ap_veh_persist_async(void);            /* any task: worker hop      */
void ap_veh_queue_switch(const char *prev_key); /* any task: worker hop  */
/** Fold a sighting into a matched entry (VIN learned, protocol
 *  re-detected, responder set refreshed behind a VIN match); under the
 *  lock. @return true when the entry changed. */
bool ap_veh_refine_entry(ap_veh_entry_t *e, const ap_veh_seen_t *seen,
                         const char *fp);
/** The switch body (internal-stack caller): snapshot @p prev_key's
 *  tables from config.json, copy the current car's file over config.json,
 *  reload, type init, persist, event. */
void ap_veh_switch_files(const char *prev_key, bool known);
/** Byte copy of one tables file onto another (missing src = the empty
 *  tables), skipped when @p dst already holds the same bytes. */
esp_err_t ap_veh_copy_tables(const char *src, const char *dst);
/** Write @p json to @p path unless it already holds the same bytes. */
esp_err_t ap_veh_write_guarded(const char *path, const char *json,
                               size_t len);
/** Snapshot + LRU eviction + the new car's tables for an unknown car
 *  (internal-stack caller; the entry is already in the index). */
void ap_veh_new_car_files(const char *prev_key, const char *new_key,
                          const char *std_config, size_t len);
#endif /* !AUTOPID_HOST_TEST */

#ifdef __cplusplus
}
#endif
