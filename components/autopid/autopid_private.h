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
 * @file autopid_private.h
 * @brief Internal types + pure-module contracts shared by the autopid
 *        .c files and the host suite. Not part of the public API.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- pool bounds (generous by design: meatpi 2026-07-06: PSRAM is
        plentiful; caps exist for static allocation, not rationing) -------- */
#define AP_MAX_GROUPS   32
#define AP_MAX_PIDS     512
#define AP_MAX_FILTERS  128
#define AP_MAX_PARAMS   2048   /* pooled across all PIDs + filters          */
#define AP_PARAMS_PER   256    /* per PID/filter: published vehicle
                                  profiles decode up to 192 values from ONE
                                  DID (Xpeng cell voltages; Hyundai/Kia BMS
                                  DIDs carry 20 to 32), so 16 rejected every
                                  Hyundai/Kia/Genesis/Xpeng profile
                                  (2026-09-16). Sizes the poller's PSRAM
                                  copy + the filter frame slots, never a
                                  stack frame.                              */

#define AP_NAME_LEN     32
#define AP_CMD_LEN      24
#define AP_INIT_LEN     96
#define AP_HDR_LEN      16
#define AP_EXPR_LEN     96
#define AP_MUX_EXPR_LEN 48     /* raw multiplexer-switch slice              */
#define AP_UNIT_LEN     16
#define AP_CLASS_LEN    24

#define AP_PAYLOAD_MAX  128    /* framing-free payload bytes: the parsers
                                  (VIN, DTC, detection), the cross-talk guard */
#define AP_FRAMES_MAX   256    /* the reply as the chip prints it with headers
                                  on, what user expressions index (2026-10-24,
                                  issue #1014): 28 frames of 8 (the Leaf
                                  ZE1's 2102, the Xpeng's 221122) = 224      */
#define AP_RESP_MAX     2048   /* raw chip response text: 28 frames behind a
                                  29-bit id are ~1040 characters              */
#define AP_RESTORE_LEN  128    /* the baseline prelude + every undo command
                                  (ap_chain_restore: 107 chars at most)     */

typedef enum
{
    AP_PID_STD = 0,
    AP_PID_CUSTOM,
    AP_PID_SPECIFIC,
} ap_pid_type_t;

typedef struct
{
    char   name[AP_NAME_LEN];
    char   expression[AP_EXPR_LEN];
    char   mux_expr[AP_MUX_EXPR_LEN]; /* "" = unconditional; else raw
                                         switch slice that must equal
                                         mux_val for expression to apply */
    char   unit[AP_UNIT_LEN];
    char   class[AP_CLASS_LEN];
    bool   enabled;
    float  min;                /* plausibility clamp; NAN = none            */
    float  max;
    float  mux_val;
} ap_param_t;

typedef struct
{
    char          name[AP_NAME_LEN];
    ap_pid_type_t type;
    char          cmd[AP_CMD_LEN];
    char          init[AP_INIT_LEN];      /* per-PID extra init, ';'-sep    */
    char          rxheader[AP_HDR_LEN];   /* -> ATCRA filter when set       */
    int           group;                  /* index into groups              */
    uint32_t      period_ms;              /* 0 = inherit group              */
    bool          enabled;
    uint16_t      param_start;            /* slice of the param pool        */
    uint16_t      param_count;
    /* a J1939 parameter group (autopid_j1939.h): cmd "PGN:..", read at
       config load; served from the listener's store, never the chip */
    bool          j1939;
    bool          j1939_request;          /* '?': sent on request (phase 6) */
    int16_t       j1939_sa;               /* pinned source, -1 = the pick   */
    uint32_t      pgn;
} ap_pid_t;

/** The scheduling class of a row (autopid_j1939.h has the words). */
typedef enum
{
    AP_CLASS_CHIP = 0,     /* a request through the OBD chip (filters too) */
    AP_CLASS_PASSIVE,      /* a PGN row: read from the listener's store    */
    AP_CLASS_BUS_TX,       /* a PGN row with '?': a request on the native
                              bus (phase 6; read like passive until then)  */
} ap_row_class_t;

#define AP_CLASS_BIT(c)   (1u << (c))
#define AP_CLASS_ALL      (AP_CLASS_BIT(AP_CLASS_CHIP) | \
                           AP_CLASS_BIT(AP_CLASS_PASSIVE) | \
                           AP_CLASS_BIT(AP_CLASS_BUS_TX))

typedef struct
{
    uint32_t frame_id;
    bool     is_extended;
    uint32_t monitor_ms;                  /* ATMA window length             */
    int      group;
    uint32_t period_ms;                   /* 0 = inherit group              */
    bool     enabled;
    uint16_t param_start;
    uint16_t param_count;
} ap_filter_t;

typedef struct
{
    char     name[AP_NAME_LEN];
    bool     enabled_default;
    uint32_t period_ms;
} ap_group_t;

/** The loaded config tables (static PSRAM pools; autopid_config.c fills). */
typedef struct
{
    ap_group_t  groups[AP_MAX_GROUPS];
    ap_pid_t    pids[AP_MAX_PIDS];
    ap_filter_t filters[AP_MAX_FILTERS];
    ap_param_t  params[AP_MAX_PARAMS];
    uint16_t    n_groups;
    uint16_t    n_pids;
    uint16_t    n_filters;
    uint16_t    n_params;
} ap_config_t;

/* ---- pure: scheduler (autopid_sched.c, host-tested) ----------------------
 * The PID is the scheduling unit; entries cover pids then filters
 * (entry index i: i < n_pids -> pid i; else filter i - n_pids).          */

typedef struct
{
    int64_t  due_us;
    int64_t  last_run_us;      /* round-robin tiebreak for period 0        */
    uint16_t fail_streak;
} ap_sched_slot_t;

typedef struct
{
    ap_sched_slot_t slots[AP_MAX_PIDS + AP_MAX_FILTERS];
    /* runtime (EPHEMERAL) group state: autopid_group_set() */
    bool    group_enabled[AP_MAX_GROUPS];
    int32_t group_period_override[AP_MAX_GROUPS]; /* <0 = none             */
    /* per-type enables from settings knobs */
    bool    type_enabled[3];
} ap_sched_t;

#define AP_SCHED_BACKOFF_STREAK 3  /* fails before backing off             */
#define AP_SCHED_BACKOFF_MULT   4
#define AP_SCHED_STAGGER_US     10000

/** Measured chip+ECU round-trip floor (Phase 1b bench: 53 ms/request,
 *  ~19 req/s ceiling). Configured periods 1..floor-1 ms are accepted but
 *  flagged in /api/autopid; period 0 = deliberate max-rate mode. */
#define AP_PERIOD_FLOOR_MS      50

/** Yield-to-app window: the poller stays off the chip until an external
 *  ELM client (TCP/BLE/USB/WS app) has been silent this long. Legacy
 *  parity: main.c cleared DEV_AUTOPID_ELM327_APP_BIT on every app
 *  command and a 10 s timer set it back. Bench 2026-09-08 with the two
 *  interleaved: 80 of 200 app requests answered with autopid's lines,
 *  STOPPED or NO DATA (the "choppy RPM dial" report). */
#define AP_CLIENT_YIELD_MS      10000

/** True while the poller must stay off the chip for an external client
 *  (@p idle_ms = ms since the client's last write, UINT32_MAX = never). */
bool ap_sched_client_hold(uint32_t idle_ms);

/** Reset slots (stagger initial due times) + runtime state from config. */
void ap_sched_reset(ap_sched_t *st, const ap_config_t *cfg, int64_t now_us);

/** Effective period of entry @p i (µs; group inheritance + runtime
 *  override + fail backoff). 0 = high-fidelity. */
int64_t ap_sched_period_us(const ap_sched_t *st, const ap_config_t *cfg,
                           int i);

/** Next runnable entry: smallest due (ties -> least recently run).
 *  @return entry index or -1 when nothing is enabled.
 *  @param[out] due_us when to run it (may be in the past = now). */
int ap_sched_next(const ap_sched_t *st, const ap_config_t *cfg,
                  int64_t *due_us);

/** ap_sched_next over the entries of the given classes only (a bit set
 *  of AP_CLASS_BIT(ap_row_class_t); autopid_j1939.h): the poller asks for
 *  the chip's rows only while it may use the chip, for the passive rows
 *  always. */
int ap_sched_next_of(const ap_sched_t *st, const ap_config_t *cfg,
                     unsigned classes, int64_t *due_us);

/** The scheduling class of entry @p i (filters are chip rows). */
ap_row_class_t ap_sched_entry_class(const ap_config_t *cfg, int i);

/** Record a run of entry @p i: reschedule + fail-streak bookkeeping. */
/** Runtime group control (pure): set = the §5b override (period < 0 keeps
 *  the current override), restore = back to the configured defaults (the
 *  undo of a while-rule). @p g is a valid group index. */
void ap_sched_group_set(ap_sched_t *st, int g, bool enabled,
                        int32_t period_override_ms);
void ap_sched_group_restore(ap_sched_t *st, const ap_config_t *cfg, int g);

void ap_sched_ran(ap_sched_t *st, const ap_config_t *cfg, int i,
                  int64_t now_us, bool ok);

/** True when entry @p i is enabled (entry + group + type gates). */
bool ap_sched_entry_enabled(const ap_sched_t *st, const ap_config_t *cfg,
                            int i);

/* ---- cache (autopid_cache.c: target; slot index == param pool index) ----- */
#ifndef AUTOPID_HOST_TEST
#include "cJSON.h"
void  ap_cache_init(void);
void  ap_cache_clear(void);
void  ap_cache_put(uint16_t slot, double value, int64_t ts_us);
bool  ap_cache_get(uint16_t slot, double *value, int64_t *ts_us);
cJSON *ap_cache_snapshot(const ap_config_t *cfg);
cJSON *ap_cache_detail(const ap_config_t *cfg);
int   ap_cache_find(const ap_config_t *cfg, const char *param);

/* External (injected) values: named samples that are NOT config-slot
 * bound (GPS from the ESPNetLink dongle). Merged into snapshot/get so
 * they ride the same autopid_data push, ${autopid.*} pulls, value sink,
 * and autopid.param events as polled parameters. */
int   ap_ext_put(const char *name, const char *unit, double value,
                 int64_t ts_us, bool *out_changed);
bool  ap_ext_get(const char *name, double *value, int64_t *ts_us);

/* config file IO (autopid_config.c, target half) */
esp_err_t autopid_config_load(ap_config_t *cfg);
esp_err_t autopid_config_save(const char *json, size_t len);
const char *autopid_config_path(void);

/* core internals shared with the http/cli surfaces (autopid.c) */
const ap_config_t *ap_core_config(void);
/* autopid_group.c (runtime group control + its JSON) borrows the core's
   lock, scheduler state and poller wake-up through these */
void ap_core_lock(void);
void ap_core_unlock(void);
ap_sched_t *ap_core_sched(void);
void ap_core_wake(void);
esp_err_t ap_core_group_json(cJSON *arr);   /* append group states       */
uint16_t ap_core_sub_floor_count(void);     /* PIDs configured 1..49 ms  */
void ap_core_scan_pause(bool on);           /* std scan owns the chip    */

/* settings (autopid_settings.c: standard §4.1) */
esp_err_t ap_settings_register(void);       /* the "autopid" descriptor  */
bool ap_settings_is_configured(void);       /* boot apply ran (§4.3)     */
int  ap_settings_pause_below_mv(void);      /* 0 = never pause           */
bool ap_settings_pause_follow_sleep(void);  /* legacy parity: pause
                                               requests below sleep_mv   */
bool ap_settings_pause_all(void);           /* pause_mode "all": the
                                               voltage pause stops the
                                               J1939 rows too            */
const char *ap_core_std_protocol(void);     /* std_protocol setting      */
const char *ap_core_specific_init_default(void); /* specific_init knob  */
uint32_t ap_core_min_event_interval_ms(void);

/* on_apply -> runtime state owned by autopid.c */
void ap_core_set_enabled(bool enabled);
void ap_core_set_type_enabled(int type, bool enabled);

/* event_manager glue (autopid_events.c: Phase 3) */
void ap_events_register(void);              /* sources/action/values     */
void ap_events_reset(void);                 /* clear emission memory     */
void ap_events_param(const ap_param_t *prm, uint16_t slot, int group,
                     double value);         /* on-change + min interval  */
void ap_events_external(const char *name, const char *unit, double value,
                        bool changed);      /* injected value → sink+event */
void ap_events_pid_failed(const char *name, uint16_t streak);
void ap_events_scan_done(uint16_t found);
void ap_events_vehicle_changed(const char *vin, const char *name, bool known);
void ap_events_vehicle_evicted(const char *vin, const char *name); /* store */

/* chip-facing runner (autopid_runner.c: poller-task context; the baseline
   half, prelude / restore / send_init, in autopid_baseline.c) */
bool ap_runner_run(const ap_pid_t *pid, int pid_index,
                   const ap_param_t *params);
/** The publish half of a poll, shared with the J1939 runner: every enabled
 *  parameter of @p pid evaluated over @p payload (mux precondition,
 *  expression, plausibility clamp) into the cache and the events, stamped
 *  @p ts_us (the reply's time: now for a chip answer, the message's own
 *  time for a J1939 group read from the store).
 *  @return true when at least one value was published. */
bool ap_runner_publish(const ap_pid_t *pid, const ap_param_t *params,
                       const uint8_t *payload, size_t payload_len,
                       int64_t ts_us);
void ap_runner_set_type_init(int type, const char *init);
void ap_runner_reset(void);        /* replay inits on the next poll     */
/** After an external ELM app had the chip: re-send the protocol prelude
 *  (spaces on, headers off, timeout, protocol, header, mask) and replay
 *  every type/PID init on the next poll. Poller-task context. */
void ap_runner_restore_baseline(void);

/** The bus guard found the stored protocol at the wrong bitrate for this
 *  bus: the prelude uses the chip's search (ATTP0) for the rest of the
 *  boot, exactly like the silent-protocol fallback. Any task. */
void ap_runner_force_search(void);

/* bus guard glue (autopid_guard.c): what the native controller heard on
   the bus decides whether the chip may be pinned to a CAN protocol */
void ap_guard_init(void);
/** Poller, before every transmission: may the chip be used now? Final
 *  once the bus named its bitrate or an ECU answered; until then a
 *  listen-only watch stays on the bus (the first call waits 400 ms). */
bool ap_guard_poll_ok(void);
/** Poller: an ECU answered on the protocol in effect (its bitrate is the
 *  bus's): the verdict is final, the watch is let go. */
void ap_guard_proven(void);
/** A one-shot chip job is about to start (detection, DTC scan / clear,
 *  test-a-PID): false = nothing may be transmitted; @p reason says why. */
bool ap_guard_job_ok(char *reason, size_t cap);
/** Whoever is about to send a prelude that pins @p proto (the runner's
 *  baseline, its restore): does the standing verdict cover THAT protocol?
 *  false = do not send; the poller's next ap_guard_poll_ok() looks at the
 *  bus for it. Plain reads, any task. */
bool ap_guard_pin_ok(char proto);
/** A chain of the tables is about to go to the chip as written (@p owner
 *  names it for the sentence: "the init of row X", "dtc_init"): may it, on
 *  the bus as the guard last saw it (ap_guard_chain_allowed)? false = send
 *  nothing of it nor of the request behind it; counted and explained in the
 *  status (`bus_guard.refused`, `refused_reason`), and for a job (@p job)
 *  in ap_guard_last_reason(). @p last as in ap_guard_chain_allowed(). */
bool ap_guard_chain_ok(const char *chain, const char *owner, bool job,
                       char *last);
void ap_guard_rearm(void);              /* the protocol in effect changed  */
bool ap_guard_parked(void);
void ap_guard_last_reason(char *out, size_t cap); /* why the last job was
                                                     refused            */
void ap_guard_status_json(cJSON *obj);  /* adds "bus_guard": {...}         */

/* ATMA filter window (autopid_filter.c: poller-task context) */
bool ap_runner_run_filter(const ap_filter_t *f, const ap_param_t *params);

/** One-shot test-a-PID through the real runner choreography (§11):
 *  type init chain, per-PID init, ATCRA, request, ATCRA off, exactly
 *  what a poll of that PID sends (@p type: AP_PID_*, -1 = unknown; it
 *  decides the borrowed-header rule). Caller pauses the poller around it
 *  (ap_core_scan_pause). `transcript` (may be NULL) receives one line
 *  per exchange, "> cmd" then "< reply", so the UI can show what went
 *  out and what came back (2026-09-16). ESP_ERR_NOT_ALLOWED = a chain of
 *  the row sets a protocol the bus guard refuses on this bus: nothing of
 *  the row was sent (ap_guard_last_reason() says why). */
esp_err_t ap_runner_test(int type, const char *type_init, const char *init,
                         const char *rxheader, const char *cmd, char *raw,
                         size_t raw_len, int64_t *elapsed_us,
                         char *transcript, size_t transcript_len);

/** Tell the reply parser how to read a 29-bit id for the row about to be
 *  parsed (ap_resp_set_id29_hint): the row's receive filter (@p rxheader,
 *  else an ATCRA in @p init, else one in @p type_init) and whether the
 *  protocol in effect is 29-bit. Before ap_resp_frames() of a poll or a
 *  test shot. */
void ap_runner_reply_hint(const char *type_init, const char *init,
                          const char *rxheader);

/* the transcript of a test (autopid_transcript.c, pure): replies flattened
   to one line and capped, a line that does not fit is dropped */
typedef struct
{
    char  *buf;                 /* NULL = no transcript wanted           */
    size_t cap;
    size_t len;
} ap_tr_t;

void ap_tr_add(ap_tr_t *t, char dir, const char *s);

/* chip-state memory shared with first contact (autopid_contact.c,
   poller-task context) */
void ap_runner_send_init(const char *init);      /* a ';'-separated chain  */
void ap_runner_send_prelude(const char *prelude); /* ... that resets the
                                                    header: nothing borrowed */
/** The boot prelude, once. false = it is due and the bus guard has no
 *  verdict for the protocol it would pin: nothing was sent, do not send a
 *  request on top (the next poll asks again). */
bool ap_runner_baseline_ensure(void);
void ap_runner_baseline_invalidate(void);        /* ... is due again       */
/** The init chain the poller sends when it switches to this PID type
 *  (what autopid_settings composed from the protocol + *_init). */
const char *ap_runner_type_init(int type);

/* standard-PID scan (autopid_std.c, target half) */
esp_err_t autopid_std_scan_start(void);     /* INVALID_STATE if running  */
cJSON *ap_std_scan_status_json(void);       /* {status,found,error,ts}   */
const char *ap_std_prelude(void);           /* ATS1;ATH0;ATST96;ATTP<p>.. */
cJSON *ap_std_table_json(void);             /* the full SAE table for UI */
const char *autopid_std_scan_path(void);    /* /data/autopid/std_scan.json */
/* one table row -> a config row: ap_std_entry_to_json(), autopid_dialect.h */

/* runner-side identity hooks: see autopid_vehicle.h (included below) */
#endif


/* ---- pure: standard-PID helpers (autopid_std.c, host-tested) -------------
 * ap_std_expression maps a legacy table row (bit_start counts within the
 * headers-on buffer [PCI, mode, PID, A, B, ...]) to a v6 expression over
 * our payload (echo included, no PCI): byte = bit_start/8 - 1.
 * Placeholder rows (bit_length 0) return ESP_ERR_NOT_SUPPORTED.        */
esp_err_t ap_std_expression(uint8_t bit_start, uint8_t bit_length,
                            double scale, double offset, char *buf,
                            size_t buf_len);

/** The same mapping for a dialect whose data sits @p shift bytes further
 *  into the payload (ap_dialect_data_shift: `62 F4 0C A B`). */
esp_err_t ap_std_expression_shift(uint8_t bit_start, uint8_t bit_length,
                                  double scale, double offset, int shift,
                                  char *buf, size_t buf_len);

/** The mapping with the row's range (@p min / @p max, NAN = none): when
 *  max is the value at full scale within float noise the scale is written
 *  as the ratio of two integers (`B2*100/255`), so a full-scale raw decodes
 *  to max EXACTLY (issue #1004: the table's float scale printed as
 *  0.3921568692 decoded 0xFF to 100.000001646, above the max of 100); a
 *  range below zero with no offset reads the raw as two's complement
 *  (`[S2:S3]*0.25`). */
esp_err_t ap_std_expression_bounded(uint8_t bit_start, uint8_t bit_length,
                                    double scale, double offset, double min,
                                    double max, int shift, char *buf,
                                    size_t buf_len);

/** OR-merge every "41 <pid> A B C D" line of @p resp (headers on or off,
 *  multi-ECU) into @p bitmap. True when at least one row matched. PURE. */
bool ap_std_scan_parse(const char *resp, uint8_t expect_pid,
                       uint32_t *bitmap);

/* config parse (PURE half of autopid_config.c) */
esp_err_t ap_config_parse(const char *json, ap_config_t *cfg, char *err,
                          size_t err_len);

#include "cJSON.h" /* the name helpers hand cJSON trees around */
/* ---- parameter names made unique (autopid_names.c, PURE, 2026-10-06) ----
 * Names key the cache and the API; the parser refuses a repeat. The scan's
 * rows are made unique before they are stored and a stored file with a
 * repeat is repaired at load (the later name gets _2, _3, ...).         */
/** Rename every repeated parameter name in a "pids" array (config rows or
 *  the scan's `supported` entries). @return how many were renamed. */
int ap_names_dedupe(cJSON *pids);
/** A config text with repeats -> the repaired text (malloc'd, caller
 *  frees) and the count; NULL and 0 when nothing needed renaming or the
 *  text does not parse (the parser then says what is wrong). */
char *ap_config_repair_names(const char *json, int *renamed);

/** The standard rows of a config text re-numbered to the table's current
 *  expressions (autopid_std.c; 2026-10-24, issue #1014: the expressions
 *  count the frame as the chip prints it, and a car the store knows keeps
 *  its rows across detections, so the file is brought up to date where it
 *  is loaded). A row is matched by its `cmd` (`01XX`, `22F4XX`) and each
 *  parameter by the table's name (a `_2` suffix of the repair allowed);
 *  anything else is left alone. @return the fixed text (malloc'd, caller
 *  frees) and how many expressions changed; NULL and 0 when none did or
 *  the text does not parse. */
char *ap_config_refresh_std(const char *json, int *changed);
int ap_std_refresh_rows(cJSON *pids);

/** In-place neutralization of EEPROM-writing user init commands
 *  (case-insensitive, whitespace-tolerant: "atsp6", "AT SP 6"):
 *  ATSP -> ATTP (same protocol switch, RAM only) and ATM1 -> ATM0
 *  (memory-on would make the chip store every subsequent protocol
 *  change). Init strings replay on every type/PID transition, so
 *  letting either through would wear the chip's EEPROM out (legacy
 *  semantics). PURE. Since 2026-09-16 a thin wrapper over the chip
 *  driver's guard (obd_chip_guard.h), which also names the commands
 *  with no RAM twin (ATPP/ATSD/ATCV/STWBR): config parse refuses those. */
void ap_init_sanitize(char *str);

/* ---- pure: a parameter's min / max applied to a decoded value
 * (autopid_bounds.c, host-tested). Issue #1004: a full tank decoded to
 * 100.000001646 against a max of 100 and was dropped. Float noise never
 * drops a reading; beyond the tolerance a standard OBD row clamps (the
 * table's range is its display range, legacy parity), every other row
 * drops (the bounds of a custom, specific or J1939 row are a filter). ---- */
#define AP_BOUNDS_TOL_REL 1e-6   /* of the bound's magnitude: 100 -> 1e-4   */
#define AP_BOUNDS_TOL_ABS 1e-6   /* the floor, for the bounds around zero    */

typedef enum
{
    AP_BOUNDS_KEPT = 0,   /* inside the bounds: the value as decoded       */
    AP_BOUNDS_CLAMPED,    /* beyond a bound, set to it                     */
    AP_BOUNDS_DROPPED,    /* beyond a bound and the row filters: not cached */
} ap_bounds_verdict_t;

/** Apply @p min / @p max (NAN = none) to @p value: within the tolerance of
 *  a bound the value is clamped to it; beyond it, clamped when
 *  @p clamp_beyond (a standard OBD row), dropped otherwise. A NaN value is
 *  dropped. */
ap_bounds_verdict_t ap_bounds_apply(double *value, float min, float max,
                                    bool clamp_beyond);

/* ---- pure: what a chain of the tables sets on the chip (autopid_chain.c,
 * host-tested). Issue #964: a custom row's init moved the chip to a BMW
 * gateway (ATSH6F1;ATCEA60;ATCRA660;ATFCSM1...) and the standard rows that
 * followed went there too. The runner restores the baseline before the next
 * row type when a chain moved any of this. ---------------------------------- */
#define AP_SETS_HEADER    (1u << 0)  /* ATSH: the prelude's functional header */
#define AP_SETS_FILTER    (1u << 1)  /* ATCRA / ATCM / ATCF: the prelude's ATCRA */
#define AP_SETS_PROTOCOL  (1u << 2)  /* ATSP / ATTP: the prelude's ATTP<p>  */
#define AP_SETS_PRINT     (1u << 3)  /* ATH0 / ATS0 / ATST: ATH1 ATS1 ATST96 */
#define AP_SETS_EXT_ADDR  (1u << 4)  /* ATCEA / ATCER -> ATCEA              */
#define AP_SETS_FLOW_CTL  (1u << 5)  /* ATFCSH / ATFCSD / ATFCSM -> ATFCSM0 */
#define AP_SETS_FC_OFF    (1u << 6)  /* ATCFC0 -> ATCFC1                    */
#define AP_SETS_TIMING    (1u << 7)  /* ATAT0 / ATAT2 -> ATAT1              */
#define AP_SETS_RAW       (1u << 8)  /* ATCAF0 -> ATCAF1                    */
#define AP_SETS_PRIORITY  (1u << 9)  /* ATCP -> ATCP18                      */
#define AP_SETS_ECHO      (1u << 10) /* ATE1 -> ATE0                        */
#define AP_SETS_LINEFEED  (1u << 11) /* ATL1 -> ATL0                        */
#define AP_SETS_QUIET     (1u << 12) /* ATR0 -> ATR1                        */
#define AP_SETS_DLC       (1u << 13) /* ATV1 / ATD1 -> ATV0 ATD0            */

/** The AP_SETS_* bits a ';'-separated chain (a type's or a row's init, an
 *  AT command as a row's cmd) sets on the chip beyond the baseline. Read as
 *  the chip reads it: spaces ignored, any case; a command the table does not
 *  know sets nothing; a bare `ATCEA` / `ATCRA` (the off / reset forms) set
 *  nothing. NULL or no AT command = 0. */
unsigned ap_chain_sets(const char *chain);

/** The chain that puts the baseline back for @p sets: @p prelude (which
 *  covers header, filter, protocol and print format) followed by one undo
 *  command per remaining bit (`;ATCEA;ATFCSM0;...`). @return the length
 *  written (cap - 1 when it did not fit: AP_RESTORE_LEN always does). */
size_t ap_chain_restore(unsigned sets, const char *prelude, char *out,
                        size_t cap);

/** Drop from a chain of the tables the print-format commands the poller
 *  cannot live with, as the legacy firmware did (its send_commands skipped
 *  `ATH0`, `ATS0` and `ATE1`): autopid reads the chip with headers and
 *  spaces on and the echo off, and a published profile's `ATS0` (the Mini
 *  Cooper SE) or `ATH0` (the Ford profiles) would break every reply after
 *  it. In place; spaces and case ignored; the separators tidied.
 *  @return how many commands were dropped. PURE. */
int ap_chain_strip_print(char *chain);

/** The receive filter a chain sets (`ATCRA<id>`, the last one wins): the
 *  id and whether it is 29-bit (8 hex digits). @return false when the chain
 *  sets none, or resets it (a bare `ATCRA`) last. PURE. */
bool ap_chain_cra_id(const char *chain, uint32_t *id, bool *is_29bit);

/* ---- pure: bus guard (autopid_bus_guard.c, host-tested) -------------------
 * May the OBD chip transmit on a protocol, given what the native
 * controller heard on the bus (can_manager_probe)? A request on a pinned
 * CAN protocol at the wrong bitrate destroys the bus traffic; the chip's
 * own search does not. */
typedef enum
{
    AP_BUS_UNKNOWN = 0,   /* not probed, or the probe was not possible      */
    AP_BUS_SILENT,        /* nothing on the bus (a gatewayed OBD port)      */
    AP_BUS_LIVE,          /* frames read at `kbps`                          */
    AP_BUS_UNREADABLE,    /* traffic that neither 250 nor 500 kbit/s reads  */
} ap_bus_kind_t;

typedef struct
{
    ap_bus_kind_t kind;
    uint16_t      kbps;   /* AP_BUS_LIVE only */
} ap_bus_t;

typedef enum
{
    AP_GUARD_ALLOW = 0,   /* send on the protocol asked for                 */
    AP_GUARD_SEARCH,      /* not on that one: the chip's search (ATTP0)     */
    AP_GUARD_PARK,        /* nothing is transmitted                         */
} ap_guard_verdict_t;

/** kbit/s the chip transmits at on CAN protocol @p proto ('6'..'9', 'A');
 *  0 = not judged (the search '0', K-line / J1850, user CAN B and C). */
uint16_t ap_guard_proto_kbps(char proto);

/** The decision. @p proto = the protocol the prelude would pin ('0' = the
 *  search); @p pinned = the `std_protocol` SETTING names it (the user's
 *  word), not the vehicle store (ours, which may give way to the search). */
ap_guard_verdict_t ap_guard_decide(const ap_bus_t *bus, char proto,
                                   bool pinned);

/** May a prelude pin @p proto now? @p ruled = the protocol the last positive
 *  verdict was taken for ('0' = the search, '\0' = none stands). A verdict
 *  is about ONE protocol on the bus as it was looked at: when the protocol
 *  in effect changes between the look and the prelude (another car made
 *  current), the prelude waits for the next look. Bench 2026-10-05: a 500
 *  kbit/s car activated on a 250 kbit/s truck was pinned on the strength of
 *  the truck's verdict, and the truck's adapter went bus-off in 40 ms. */
bool ap_guard_pin_allowed(char proto, char ruled);

/** The CAN protocol an AT command sets, as the chip reads it (`ATSP6`,
 *  `at tp a7`, `ATSP00`): '0'..'9', 'A'..'C'; '\0' = the command sets none. */
char ap_guard_cmd_proto(const char *cmd, size_t len);

/** True for the commands that reset the chip (`ATZ`, `ATD`, `ATWS`): it
 *  comes back on the protocol stored in its EEPROM, the last vehicle
 *  DETECTED, whatever bus the device is plugged into now. Whoever sends one
 *  out of a chain of the tables sends the baseline prelude right behind it. */
bool ap_guard_cmd_resets(const char *cmd, size_t len);

/** A ';'-separated chain of the tables (a row's or a type's init, a cmd,
 *  dtc_init): is EVERY protocol it sets allowed on @p bus, by the ruling a
 *  pinned setting gets (no search to give way to)? @p last (may be NULL) =
 *  the protocol the chain leaves the chip on, '\0' when it sets none (or
 *  resets the chip after the last one it set);
 *  @p refused (may be NULL) = the first one refused. Bench 2026-10-05: a row
 *  with `ATSP6` tested on a 250 kbit/s truck, the truck's adapter bus-off
 *  19 ms after its first error. */
bool ap_guard_chain_allowed(const ap_bus_t *bus, const char *chain,
                            char *last, char *refused);

/** The sentence of such a refusal: "<owner> sets protocol 6 (500 kbit/s) and
 *  the vehicle bus runs at 250 kbit/s: not sent". @return its length. */
size_t ap_guard_chain_reason(const ap_bus_t *bus, char proto,
                             const char *owner, char *out, size_t cap);

const char *ap_guard_verdict_name(ap_guard_verdict_t verdict);
const char *ap_bus_kind_name(ap_bus_kind_t kind);

/** One sentence for the log and the status: what the bus is and what that
 *  means for @p proto. @return its length (0 on a NULL / empty buffer). */
size_t ap_guard_reason(const ap_bus_t *bus, char proto, bool pinned,
                       char *out, size_t cap);

/* ---- pure: ELM response text -> bytes (autopid_resp.c) ---------------------
 * Handles: echo/blank/SEARCHING lines, error markers (NO DATA, ERROR,
 * STOPPED, ?), headers-off single line ("41 0C 1A F8"), headers-off
 * ISO-TP multi-line ("014" + "0: 49 02 .." + "1: .."), headers-on frames
 * ("7E8 06 41 00 ..") incl. ISO-TP single/first/consecutive reassembly
 * from the lowest-ID responder, an extended address byte before the PCI
 * ("607 F1 10 09 62 ..", the BMW scheme), the chip's four-token 29-bit id.
 * The PAYLOAD is framing-free, from the service echo (B0 = 0x41): what the
 * parsers (VIN, DTC, the detection) and the cross-talk guard read.      */
esp_err_t ap_resp_to_payload(const char *resp, uint8_t *payload,
                             size_t payload_max, size_t *out_len);

/** The reply as the chip prints it with headers on, which is what user
 *  expressions index since 2026-10-24 (issue #1014; the legacy firmware's
 *  parse_elm327_response() read the same bytes, so the published vehicle
 *  profiles count this way): every line's bytes after the CAN id, the
 *  ISO-TP PCI bytes and an extended address byte included, the lines of
 *  the LOWEST responder id concatenated in arrival order, `7F xx 78`
 *  response-pending frames dropped. The chip trims a single frame to its
 *  length and prints consecutive frames whole, padding included. A reply
 *  that arrives without ids (a K-line protocol, whose prelude keeps
 *  headers off) is rebuilt into that shape from its rows: `0L` + the
 *  bytes, or `1L LL` + six bytes then `2N` + seven per consecutive frame,
 *  so `[B3:B4]/4` is the RPM on every protocol. PURE.
 *  @return ESP_ERR_NOT_FOUND = no data line, ESP_FAIL = an error line. */
esp_err_t ap_resp_frames(const char *resp, uint8_t *out, size_t out_max,
                         size_t *out_len);

/** How the tokenizer reads a 29-bit id printed as four byte tokens: the
 *  legislated `18 DA F1 xx` is always taken (take_id29); with this hint a
 *  row's own receive filter id (`ATCRA17FE007B`, the MEB profile) is taken
 *  too, and on a 29-bit protocol any four tokens followed by a plausible
 *  PCI. Set by the runner before a row's reply is parsed (the poller task)
 *  and by the test route (the httpd task): never at once (the chip job).
 *  0 / false = the legislated rule alone. */
void ap_resp_set_id29_hint(uint32_t rx_id, bool proto29);

#define AP_RESP_ECUS_MAX 8      /* 7E8..7EF: the functional-response set */

typedef struct
{
    uint32_t header;            /* CAN id; UINT32_MAX = headers were off */
    uint8_t  payload[AP_PAYLOAD_MAX];
    size_t   len;
} ap_resp_ecu_t;

/** Like ap_resp_to_payload but keeps EVERY responder when headers are
 *  on: one entry per distinct CAN id (ascending), each ISO-TP
 *  reassembled independently. Headers off = single entry (identical to
 *  ap_resp_to_payload). @return entry count, 0 = no payload,
 *  -1 = chip/bus error line seen. */
int ap_resp_to_payloads(const char *resp, ap_resp_ecu_t *out,
                        int max_ecus);

/** One ATMA monitor line -> frame data bytes when it carries @p frame_id
 *  (legacy-compatible header shapes: contiguous "7E8"/"18DAF110" or the
 *  id split into 2-hex byte tokens). Expressions index from B0 = first
 *  DATA byte (no id echo: the legacy filter frame-of-reference). PURE. */
bool ap_filter_frame(const char *line, size_t len, uint32_t frame_id,
                     uint8_t *payload, size_t payload_max,
                     size_t *out_len);

/** Incremental ATMA stream collector: feed raw monitor bytes in ANY
 *  chunking; returns true the moment a completed line carries
 *  @p frame_id (first match wins: repeats of the id in the same
 *  window are simply never reached). Overlong lines are truncated
 *  safely; non-matching/noise lines are skipped. PURE. */
typedef struct
{
    char   line[160];
    size_t n;
    bool   overflow;            /* current line exceeded the buffer      */
} ap_flt_stream_t;

void ap_flt_stream_init(ap_flt_stream_t *st);
bool ap_flt_stream_feed(ap_flt_stream_t *st, const uint8_t *bytes,
                        size_t len, uint32_t frame_id, uint8_t *payload,
                        size_t payload_max, size_t *out_len);

/** ap_flt_stream_feed that reports how many input bytes were consumed
 *  when a frame matched (up to and including its line terminator), so
 *  the caller can resume mid-chunk and capture EVERY matching frame:
 *  a multiplexed message needs more than the first one. On false the
 *  whole chunk was consumed. */
bool ap_flt_stream_feed_ex(ap_flt_stream_t *st, const uint8_t *bytes,
                           size_t len, uint32_t frame_id,
                           uint8_t *payload, size_t payload_max,
                           size_t *out_len, size_t *consumed);

/** True when @p payload plausibly answers @p cmd: hex service commands
 *  must echo (service | 0x40) + the identifier byte. Rejects cross-talk
 *  when another chip master's response lands in our request window
 *  (bench-proven under WS-OBD contention, Phase 1b). Service 22 echoes
 *  both identifier bytes, and both are compared: `62 F4 0D` does not
 *  answer `22F40C` (an ECU's late answer to the previous request, bench
 *  2026-10-03). AT/ST/VT commands return true (nothing to verify). PURE. */
bool ap_payload_matches_cmd(const char *cmd, const uint8_t *payload,
                            size_t payload_len);

#include "autopid_dtc_private.h" /* DTC codecs, report, engine, database */
#include "autopid_dbc_private.h" /* DBC codec + store                    */

#ifndef AUTOPID_HOST_TEST
/** One-shot chip-job serialization (std scan / test-a-PID / dtc jobs
 *  never interleave; each still pauses the poller via
 *  ap_core_scan_pause). autopid.c owns the flag. */
bool ap_core_job_acquire(void);
void ap_core_job_release(void);
#endif

#include "autopid_vehicle.h" /* vehicle identity: pure core + file owner */
#include "autopid_dialect.h" /* OBD dialects: requests, parsers, addressing */
#include "autopid_j1939.h"   /* J1939 rows: grammar, classes, the std table */

#ifdef __cplusplus
}
#endif

