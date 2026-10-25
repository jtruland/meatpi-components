# external_storage: host unit suite

Compiles ONLY the pure parts (`es_detect.c`, the card-detect debouncer, and
`es_probe.c`, the probe of what a card holds: no GPIO/SDMMC) on the IDF
**linux** target. Runs via `.\test.ps1 host external_storage`, or without
the Pi through `bash tools/testbench/host_gcc_wsl.sh external_storage`
(system gcc + Unity, ASan and UBSan).

## What is covered (14 tests)

The debouncer (3): glitches shorter than the 4-sample threshold never
surface (1 to 3 sample bursts); a clean insert and remove each fire the
change edge exactly once (steady state never re-fires); alternating
"seating wobble" samples never accumulate and the state flips exactly once
after it settles.

The probe (11, 2026-10-25): a Windows FAT32 boot record (and the same
without its signature: refused), an early-DOS FAT16 record accepted on its
BPB alone (a bad cluster size or a missing jump refused), exFAT (its
signature required) and NTFS by their OEM names; whole disks: a
super-floppy FAT card read in one sector, blank cards (all 0x00 and all
0xFF), an MBR with a FAT32 partition (two reads, the partition's type and
first sector reported), the factory exFAT card (an MBR, one 0x07
partition, exFAT inside: the 64 GB bench card), a GPT protective entry
(nothing more read), a Linux partition (unknown), a partition never
formatted (blank), a table pointing at sector 0 (unknown, no read error), a
disk signature without partitions and a sector of noise (unknown), read
errors on sector 0 and on the partition's record, no reader at all; the
kind and state words, and the format's cluster size (16 KB up to 32 GiB,
32 KB above).

## Expected result

```
14 Tests 0 Failures 0 Ignored
OK
```

Last verified green: 2026-10-25, under WSL gcc (`host_gcc_wsl.sh`, ASan +
UBSan) and on rpi001's IDF linux target (`.\test.ps1 host external_storage`,
22 s).
