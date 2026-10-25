# external_storage: HTTP API reference

> **Implemented 2026-10-25** (the SD card format feature). Conventions:
> `components/HTTP_API.md` §1. Registered by `external_storage_register_http()`
> (main, HTTP compositions). The slot's one-line summary rides
> `GET /api/status` as `sd` (`dev_status_manager/HTTP_API.md`).

## GET /api/sdcard

The slot in full: what is in it, whether WiCAN can read it, the card's
identity, the mounted volume's size, and the last format.

**Response 200**
```json
{
  "present": true,
  "mounted": false,
  "state": "unreadable",
  "reason": "exfat",
  "card": { "name": "SD64G", "type": "SDHC/SDXC", "capacity_bytes": 63864569856,
            "sector_size": 512, "speed_khz": 40000, "serial": "6b2f91a4" },
  "format": { "state": "idle", "error": "", "took_ms": 0, "age_s": 0,
              "cluster_kb": 32, "phrase": "format sdcard" }
}
```

- `state`: `absent` (no card on the detect pin), `mounted` (a FAT card at
  `/sd`), `unreadable` (the card answers but holds no FAT volume WiCAN can
  mount: a format is offered), `failed` (the card does not answer the host),
  `formatting`.
- `reason`: for `unreadable` what the probe of the card's boot record found
  BEFORE any mount was tried: `exfat` (the factory format of every card over
  32 GB; WiCAN's FatFs has no exFAT), `ntfs`, `blank` (sector 0 all 0x00 or
  0xFF, or a partition never formatted), `gpt`, `unknown` (something else, or
  a damaged boot sector), `fat_corrupt` (looked like FAT, the mount refused
  it); for `failed` the esp_err name (`ESP_ERR_TIMEOUT` for a card that
  never answered); `""` otherwise.
- `card`: `null` until the card answered the host; `serial` is the CID
  serial as 8 hex characters (the web UI's "Not now" is per card).
- `fs`: `{"total","used"}` bytes, only while mounted (FatFs's free-cluster
  count; whole numbers, 64-bit on the device).
- `format`: `state` `idle` | `running` | `done` | `failed` (the last format
  since boot), `error` the esp_err name when it failed, `took_ms`, `age_s`
  since it ended, `cluster_kb` the allocation unit a format uses for this
  card (32 KB above 32 GB, 16 KB otherwise), `phrase` the words the POST
  below must carry.

## POST /api/sdcard/format

Format the card in the slot as FAT32 (one MBR partition, two FATs, the
cluster size above) and mount it. Everything on the card is erased. Only a
card WiCAN cannot read is ever formatted: a mounted card is refused, so
nothing readable is lost through this route.

**Body** `{"confirm":"format sdcard"}` (exactly these words; the web UI has
the user type them).

**Response 202** `{"state":"formatting"}`: the request is taken, the detect
task formats on its next 250 ms lap; poll `GET /api/sdcard` once a second
until `state` is no longer `formatting`: `mounted` with `format.state`
`done` (and `fs`), or `unreadable` with `format.state` `failed` and
`format.error`. A 64 GB card takes a few seconds; a slow card up to a
minute.

**Errors**
| Code | Body | When |
|---|---|---|
| 400 | `{"error":"confirm with {\"confirm\":\"format sdcard\"}"}` | no body, or the words differ |
| 409 | `{"error":"no card in the slot"}` | `state` absent |
| 409 | `{"error":"the card is mounted and in use: nothing to format"}` | `state` mounted |
| 409 | `{"error":"the card does not answer: it cannot be formatted"}` | `state` failed |
| 409 | `{"error":"a format is already running"}` | `state` formatting |

A card pulled out mid-format fails the format (an I/O error) and the state
follows the detect pin to `absent`.

## Console

`sdcard` prints the same state with its reason and the card; `sdcard
--format` prints the warning, `sdcard --format --yes` requests the format
and waits for its outcome (`cmdline_manager/README.md`).
