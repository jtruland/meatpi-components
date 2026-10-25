# external_storage: the SD/MMC card owner (HAL)

Owns the SD card as a DEVICE (Architecture §4): card-detect supervision
with hot-plug, the SDMMC 4-bit host, the probe of what a card holds, the
card's FAT mount at the `/sd` VFS root, and, since 2026-10-25, the format
of a card WiCAN cannot read. It knows nothing about what's stored: file
access goes through `filesystem` (`/sd/...` paths); the composition root
glues this component's mount events to `filesystem_sd_set_mounted()`
(neither depends on the other) and provides the slot summary on
`GET /api/status` (`sd`) through dev_status_manager's provider.

## Hardware (WiCAN Pro, meatpi 2026-07-04)

SDMMC slot via the GPIO matrix: CLK=21, CMD=47, D0=14, D1=13, D2=12,
D3=48; 4-bit width, internal pullups, `SDMMC_FREQ_HIGHSPEED` (driver
derates as needed). Card-detect = **GPIO40, active low** (socket switch to
GND, internal pullup: polarity confirmed live 2026-07-04).

## API

| Call | Behavior |
|---|---|
| `external_storage_init()` | Detect-pin GPIO, the state lock + log descriptor; no card access. |
| `external_storage_start()` | Starts the detect task (250 ms poll, 4-sample debounce: the pure `es_detect` machine); probes and mounts immediately when a card is seated. |
| `external_storage_stop()` | Unmount + stop the task. |
| `external_storage_is_present()` / `is_mounted()` | Debounced presence / mount state. |
| `external_storage_status(out)` | The slot in full: `state` (absent, mounted, unreadable, failed, formatting), `reason`, the card's identity (name, type, capacity, sector size, speed, CID serial) once it answered, the cluster size a format would use, the last format (state, error, took, age). |
| `external_storage_state_str()` / `_format_state_str()` | The wire words (pure). |
| `external_storage_format_request()` | Formats a card WiCAN cannot read as FAT32 and mounts it; answers at once, the detect task does the work. `ESP_ERR_INVALID_STATE` for no card, a card that does not answer or a MOUNTED card (nothing readable is ever erased), `ESP_ERR_NOT_FINISHED` while one runs. |
| `external_storage_set_callback(cb)` | The ONE mount-event callback (composition root → filesystem backend flip). Fires `false` BEFORE unmounting so consumers stop routing first. |
| `external_storage_register_http()` | `GET /api/sdcard`, `POST /api/sdcard/format` (`HTTP_API.md`). |

Publishes `DEV_STATUS_BIT_SDCARD_MOUNTED`. A yanked card degrades `/sd`
operations to `ESP_ERR_INVALID_STATE`, never a crash.

## What happens when a card arrives (2026-10-25)

1. **The probe.** The host and the card are brought up without a
   filesystem (`sdmmc_host_init`, `sdmmc_card_init`), the card's identity is
   kept, sector 0 is read and judged by the pure `es_probe` (FatFs's own
   `check_fs()` rules for a FAT boot record, the exFAT and NTFS OEM names,
   the MBR's first partition and its boot record, blank sectors), then the
   host is released. Two card inits per insertion (this one and the
   mount's) cost about 100 ms and keep every call on the public IDF API.
2. **A FAT card is mounted** as before (`format_if_mount_failed = false`:
   it is the USER'S card). A FAT the mount still refuses is `unreadable`
   with the reason `fat_corrupt`.
3. **Anything else is `unreadable`** with ONE W line naming what the card
   holds (`exfat`, `ntfs`, `blank`, `gpt`, `unknown`) and the way out. No
   mount is attempted, so there are no E lines and no `boot_errors` fault:
   before this, an exFAT card (the factory format of every card over 32 GB)
   cost two IDF E lines plus one of ours per insertion, and a fault at boot.
4. **A card that does not answer** is `failed` with the esp_err name (the
   sdmmc driver's own E lines for a dead card stay: they are a fault).

**The format** (`external_storage_format_request()`, the route, `sdcard
--format --yes`) is allowed for `unreadable` only. The detect task runs
the IDF's own format-on-mount path (`esp_vfs_fat_sdmmc_mount` with
`format_if_mount_failed`: `f_fdisk` one partition, `f_mkfs` FM_ANY, which
is FAT32 for any card over 2 GB since FatFs here has no exFAT, two FATs,
the allocation unit from `es_format_cluster_bytes()`: 32 KB above 32 GB,
16 KB otherwise) and mounts; the status carries `formatting`, then
`mounted` + `format.state done` (`took_ms`) or `unreadable` + `failed`
with the error, after which the card is probed again. Removal mid-format
fails the format and the state follows the pin. The detect task's stack
headroom is logged after every format (the stack audit's rule).

## Dependencies

`esp_driver_sdmmc`, `fatfs`, `esp_driver_gpio`, `esp_timer`,
`dev_status_manager`, `log_manager`, `cmdline_manager` (the `_cli.c` only),
`http_server_manager` (the `_http.c` only), `settings_manager`: all
private. No filesystem edge (glued in main).

## Memory footprint (2026-07-26 measured; 2026-10-25 estimated additions)

| Where | What | Size |
|---|---|---|
| Flash | code + rodata | 2.2 KiB (2026-07-26) + the probe, the format path, the routes and the CLI options: about 4 KiB estimated |
| PSRAM `.bss` | detect-task stack (SD I/O uses the driver's own DMA buffers; no cache-off work on this stack, §2); 4 KB until 2026-10-25, 6 KB since (the probe keeps two sectors on it, a format runs f_mkfs here) + the probe pass's `sdmmc_card_t` | **6,144 B** + about 200 B |
| Internal `.bss` | TCB, the mutex, state, the one DMA sector buffer | **about 1,000 B** (389 B before) |
| Internal heap | SDMMC driver DMA buffers + FATFS work areas (driver/FATFS-owned, while mounted); a format adds two 4 KB FatFs work buffers for its duration | ~10 KB (estimated) |

Flash writes: none from this component (the card is SDMMC, not the
internal flash).

## Tests

- **Host (`host_test/`, 14 tests)**: the pure detect debouncer (3) and the
  probe (11): boot records (FAT32, early-DOS FAT16 on its BPB, exFAT, NTFS,
  refused variants), whole disks (super-floppy FAT, blank, an MBR with a
  FAT32 partition, the factory exFAT card, GPT, Linux, a never-formatted
  partition, a table pointing at itself, a signature without partitions,
  noise), read errors, the words, the cluster size. Green under WSL gcc and
  on rpi001's IDF linux target, 2026-10-25.
- **Live (2026-07-04, composed main, deployed via OTA)**: the bench card
  mounted at boot (`sdcard_mounted` bit set; a 16 GB SanDisk SC16G,
  `mounted /sd: SC16G 15193MB`, which this note called "a real 2.8 GB card"
  because `/api/fs/info` said so: the 32-bit cast of issue #982 in
  `filesystem_info()`, fixed 2026-10-11), existing FAT content listed
  via `/api/fs/list?path=/sd`, capacity via `/api/fs/info` (FATFS
  free-cluster query), upload → byte-identical download → delete on the
  card through the UI file-manager API. Hot-plug (remove/re-insert while
  running) exercises the same debounce+mount path: physically verify at
  the bench when convenient.
- **Bench (2026-10-25)**: `tools/testbench/system/sdcard_format_bench.py`
  (`.\test.ps1 sdformat`, TESTING.md): the slot's state as found, the
  unreadable card's reason and the quiet log, the refusals, the format
  through the route and the mount after it, a file round trip, the boot
  after, then the real page in Chrome (`sdcard_ui_check.mjs`). First run
  2026-10-25 with the card already FAT32: `SD FORMAT PARTIAL PASS (legs: s0
  s2 s5)` + `SD CARD UI PASS` (the routes, the summary, the refusals, the
  page); the format legs want a card made unreadable on a computer.
- **Live over the console (2026-10-25, the bench unit, a Lexar LX64G 64 GB
  card as it left the factory)**: the boot reported `unreadable (exfat)`
  with the card named and no fault latched; `sdcard --format --yes` formatted
  it in 5,482 ms (32 KB clusters) and mounted it (`mounted /sd: LX64G
  60350MB`, detect task headroom 3,504 B of 6,144); `sdcard -t` passed,
  `fs` read 58.9 GB; no E line from the storage tags through the run.

## CLI

`external_storage_register_cli()` (the settings boot apply, behind `cli`)
registers the `sdcard` command with cmdline_manager
(`external_storage_cli.c`): bare = present / mounted / the state with its
reason / the card / the last format; `-i` the card's name, type, capacity,
sector size and speed (for a mounted or merely answering card); `-t` the
write/read-back self-test; `--format` the warning, `--format --yes` the
format, waited for and reported. FILESYSTEM usage is the `fs` command
(filesystem owns that).

## Settings (`"external_storage"`, version 1)

Minimal descriptor, one knob: `cli` (bool, default true), register the `sdcard` console command(s) with cmdline_manager on the settings boot apply (reboot-to-apply). Registered via `external_storage_register_settings()`, wired by main right after settings_manager_init because this component inits before it (the log_manager_register_settings pattern, 2026-07-05).
