/**
 * @file test_main.c
 * @brief Host tests for the pure parts of external_storage: the card-detect
 *        debouncer (glitch rejection, clean insert/remove edges,
 *        agreement-resets-pending-change) and, since 2026-10-25, the probe
 *        of what a card holds (FatFs's boot record rules, the exFAT and
 *        NTFS names, the MBR walk, blank and damaged sectors) with the
 *        state words and the format's cluster size.
 */
#include <string.h>

#include "unity.h"

#include "external_storage.h"

#include "external_storage_private.h"

void setUp(void)
{
}

void tearDown(void)
{
}

/* ---- the debouncer ----------------------------------------------------------- */

static void test_glitches_never_surface(void)
{
    es_detect_t d;

    es_detect_init(&d, false);

    /* 1..3 present samples then back: below the threshold, no change */
    for (int burst = 1; burst < ES_DEBOUNCE_SAMPLES; burst++)
    {
        for (int i = 0; i < burst; i++)
        {
            TEST_ASSERT_FALSE(es_detect_feed(&d, true));
        }

        TEST_ASSERT_FALSE(es_detect_feed(&d, false)); /* back to agree */
        TEST_ASSERT_FALSE(d.stable_present);
    }
}

static void test_clean_insert_and_remove(void)
{
    es_detect_t d;

    es_detect_init(&d, false);

    for (int i = 0; i < ES_DEBOUNCE_SAMPLES - 1; i++)
    {
        TEST_ASSERT_FALSE(es_detect_feed(&d, true));
    }

    TEST_ASSERT_TRUE(es_detect_feed(&d, true)); /* the edge fires ONCE */
    TEST_ASSERT_TRUE(d.stable_present);
    TEST_ASSERT_FALSE(es_detect_feed(&d, true)); /* steady: no repeat */

    for (int i = 0; i < ES_DEBOUNCE_SAMPLES - 1; i++)
    {
        TEST_ASSERT_FALSE(es_detect_feed(&d, false));
    }

    TEST_ASSERT_TRUE(es_detect_feed(&d, false));
    TEST_ASSERT_FALSE(d.stable_present);
}

static void test_bouncing_insertion_settles(void)
{
    es_detect_t d;

    es_detect_init(&d, false);

    /* seating wobble: alternating samples never accumulate */
    for (int i = 0; i < 6; i++)
    {
        TEST_ASSERT_FALSE(es_detect_feed(&d, (i % 2) == 0));
    }

    /* then it settles: exactly ES_DEBOUNCE_SAMPLES to flip. The wobble
       ended on a present sample (i=4 true, i=5 false, so candidate=false)
       -> a full fresh run of present samples is required */
    int flips = 0;

    for (int i = 0; i < ES_DEBOUNCE_SAMPLES; i++)
    {
        if (es_detect_feed(&d, true))
        {
            flips++;
        }
    }

    TEST_ASSERT_EQUAL(1, flips);
    TEST_ASSERT_TRUE(d.stable_present);
}

/* ---- the probe: synthetic disks ---------------------------------------------- */

typedef struct
{
    uint8_t  s0[ES_SECTOR_BYTES];   /* sector 0                            */
    uint8_t  vbr[ES_SECTOR_BYTES];  /* the partition's boot record         */
    uint32_t vbr_lba;               /* where it is                         */
    bool     fail0;                 /* sector 0 unreadable                 */
    bool     failv;                 /* the partition's boot record unreadable */
    int      reads;
} disk_t;

static bool disk_read(void *ctx, uint32_t lba, uint8_t *out)
{
    disk_t *d = ctx;

    d->reads++;

    if (lba == 0)
    {
        if (d->fail0)
        {
            return false;
        }

        memcpy(out, d->s0, ES_SECTOR_BYTES);
        return true;
    }

    if (lba == d->vbr_lba && !d->failv)
    {
        memcpy(out, d->vbr, ES_SECTOR_BYTES);
        return true;
    }

    return false;
}

static void sig(uint8_t *s)
{
    s[510] = 0x55;
    s[511] = 0xAA;
}

static void st16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void st32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/** A FAT32 boot record as Windows writes it: a jump, the type string, the
 *  signature. */
static void fat32_vbr(uint8_t *s)
{
    memset(s, 0, ES_SECTOR_BYTES);
    s[0] = 0xEB; s[1] = 0x58; s[2] = 0x90;
    memcpy(s + 3, "MSWIN4.1", 8);
    st16(s + 11, 512);
    s[13] = 64;
    st16(s + 14, 32);
    s[16] = 2;
    st32(s + 32, 124735488u);
    memcpy(s + 82, "FAT32   ", 8);
    sig(s);
}

/** An early-DOS FAT16 boot record: a plausible BPB, no type string, no
 *  signature (FatFs accepts it on the BPB alone). */
static void fat16_vbr_bare(uint8_t *s)
{
    memset(s, 0, ES_SECTOR_BYTES);
    s[0] = 0xE9;
    st16(s + 11, 512);
    s[13] = 4;
    st16(s + 14, 1);
    s[16] = 2;
    st16(s + 17, 512);
    st16(s + 19, 32768);
    st16(s + 22, 32);
}

static void exfat_vbr(uint8_t *s)
{
    memset(s, 0, ES_SECTOR_BYTES);
    memcpy(s, "\xEB\x76\x90" "EXFAT   ", 11);
    sig(s);
}

static void ntfs_vbr(uint8_t *s)
{
    memset(s, 0, ES_SECTOR_BYTES);
    s[0] = 0xEB; s[1] = 0x52; s[2] = 0x90;
    memcpy(s + 3, "NTFS    ", 8);
    sig(s);
}

/** An MBR with one partition of @p type at @p lba. */
static void mbr(uint8_t *s, uint8_t type, uint32_t lba, uint32_t size)
{
    memset(s, 0, ES_SECTOR_BYTES);
    st32(s + 440, 0x12345678); /* a disk signature */
    s[446 + 4] = type;
    st32(s + 446 + 8, lba);
    st32(s + 446 + 12, size);
    sig(s);
}

static void test_vbr_fat32_windows(void)
{
    uint8_t s[ES_SECTOR_BYTES];

    fat32_vbr(s);
    TEST_ASSERT_EQUAL(ES_FS_FAT, es_probe_vbr(s));
    s[511] = 0x00; /* without the signature the FAT32 string is not enough */
    TEST_ASSERT_EQUAL(ES_FS_UNKNOWN, es_probe_vbr(s));
}

static void test_vbr_fat16_bpb_only(void)
{
    uint8_t s[ES_SECTOR_BYTES];

    fat16_vbr_bare(s);
    TEST_ASSERT_EQUAL(ES_FS_FAT, es_probe_vbr(s));
    s[13] = 3; /* a cluster size that is not a power of two */
    TEST_ASSERT_EQUAL(ES_FS_UNKNOWN, es_probe_vbr(s));
    s[13] = 4;
    s[0] = 0x00; /* no jump: not a boot record */
    TEST_ASSERT_EQUAL(ES_FS_UNKNOWN, es_probe_vbr(s));
}

static void test_vbr_exfat_and_ntfs(void)
{
    uint8_t s[ES_SECTOR_BYTES];

    exfat_vbr(s);
    TEST_ASSERT_EQUAL(ES_FS_EXFAT, es_probe_vbr(s));
    s[510] = 0; /* exFAT needs its signature */
    TEST_ASSERT_EQUAL(ES_FS_UNKNOWN, es_probe_vbr(s));
    ntfs_vbr(s);
    TEST_ASSERT_EQUAL(ES_FS_NTFS, es_probe_vbr(s));
}

static void test_probe_super_floppy_fat(void)
{
    disk_t d;
    es_probe_t pr;

    memset(&d, 0, sizeof(d));
    fat32_vbr(d.s0);
    es_probe(disk_read, &d, &pr);
    TEST_ASSERT_EQUAL(ES_FS_FAT, pr.kind);
    TEST_ASSERT_FALSE(pr.mbr);
    TEST_ASSERT_FALSE(pr.read_error);
    TEST_ASSERT_EQUAL(1, d.reads); /* sector 0 was enough */
}

static void test_probe_blank_cards(void)
{
    disk_t d;
    es_probe_t pr;

    memset(&d, 0, sizeof(d));
    es_probe(disk_read, &d, &pr);
    TEST_ASSERT_EQUAL(ES_FS_BLANK, pr.kind);
    memset(d.s0, 0xFF, ES_SECTOR_BYTES); /* an erased flash card */
    es_probe(disk_read, &d, &pr);
    TEST_ASSERT_EQUAL(ES_FS_BLANK, pr.kind);
}

static void test_probe_mbr_fat32_partition(void)
{
    disk_t d;
    es_probe_t pr;

    memset(&d, 0, sizeof(d));
    d.vbr_lba = 8192;
    mbr(d.s0, 0x0C, 8192, 31000000u);
    fat32_vbr(d.vbr);
    es_probe(disk_read, &d, &pr);
    TEST_ASSERT_EQUAL(ES_FS_FAT, pr.kind);
    TEST_ASSERT_TRUE(pr.mbr);
    TEST_ASSERT_EQUAL_HEX8(0x0C, pr.part_type);
    TEST_ASSERT_EQUAL_UINT32(8192, pr.part_lba);
    TEST_ASSERT_EQUAL(2, d.reads);
}

static void test_probe_factory_exfat_card(void)
{
    /* a 64 GB card as it leaves the factory: an MBR, one 0x07 partition,
       exFAT inside: the case the bench unit holds since 2026-10-25 */
    disk_t d;
    es_probe_t pr;

    memset(&d, 0, sizeof(d));
    d.vbr_lba = 32768;
    mbr(d.s0, 0x07, 32768, 124700000u);
    exfat_vbr(d.vbr);
    es_probe(disk_read, &d, &pr);
    TEST_ASSERT_EQUAL(ES_FS_EXFAT, pr.kind);
    TEST_ASSERT_TRUE(pr.mbr);
    TEST_ASSERT_EQUAL_HEX8(0x07, pr.part_type);
    TEST_ASSERT_EQUAL_STRING("exfat", es_fs_kind_str(pr.kind));
}

static void test_probe_mbr_other_partitions(void)
{
    disk_t d;
    es_probe_t pr;

    memset(&d, 0, sizeof(d));
    mbr(d.s0, 0xEE, 1, 0xFFFFFFFFu); /* GPT protective */
    es_probe(disk_read, &d, &pr);
    TEST_ASSERT_EQUAL(ES_FS_GPT, pr.kind);
    TEST_ASSERT_EQUAL(1, d.reads); /* nothing more is read */

    memset(&d, 0, sizeof(d));
    d.vbr_lba = 2048;
    mbr(d.s0, 0x83, 2048, 1000000u); /* Linux: an unknown boot record */
    memset(d.vbr, 0x5A, ES_SECTOR_BYTES);
    sig(d.vbr);
    es_probe(disk_read, &d, &pr);
    TEST_ASSERT_EQUAL(ES_FS_UNKNOWN, pr.kind);
    TEST_ASSERT_TRUE(pr.mbr);
    TEST_ASSERT_FALSE(pr.read_error);

    memset(&d, 0, sizeof(d));
    d.vbr_lba = 2048;
    mbr(d.s0, 0x0C, 2048, 1000000u); /* a partition never formatted */
    es_probe(disk_read, &d, &pr);
    TEST_ASSERT_EQUAL(ES_FS_BLANK, pr.kind);

    memset(&d, 0, sizeof(d));
    mbr(d.s0, 0x0C, 0, 1000000u); /* a table pointing at itself */
    es_probe(disk_read, &d, &pr);
    TEST_ASSERT_EQUAL(ES_FS_UNKNOWN, pr.kind);
    TEST_ASSERT_FALSE(pr.read_error);
}

static void test_probe_signature_without_partitions(void)
{
    disk_t d;
    es_probe_t pr;

    memset(&d, 0, sizeof(d));
    st32(d.s0 + 440, 0x12345678);
    sig(d.s0);
    es_probe(disk_read, &d, &pr);
    TEST_ASSERT_EQUAL(ES_FS_UNKNOWN, pr.kind);
    TEST_ASSERT_FALSE(pr.mbr);

    memset(d.s0, 0x33, ES_SECTOR_BYTES); /* no signature at all */
    es_probe(disk_read, &d, &pr);
    TEST_ASSERT_EQUAL(ES_FS_UNKNOWN, pr.kind);
}

static void test_probe_read_errors(void)
{
    disk_t d;
    es_probe_t pr;

    memset(&d, 0, sizeof(d));
    d.fail0 = true;
    es_probe(disk_read, &d, &pr);
    TEST_ASSERT_TRUE(pr.read_error);
    TEST_ASSERT_EQUAL(ES_FS_UNKNOWN, pr.kind);

    memset(&d, 0, sizeof(d));
    d.vbr_lba = 8192;
    d.failv = true;
    mbr(d.s0, 0x0C, 8192, 31000000u);
    es_probe(disk_read, &d, &pr);
    TEST_ASSERT_TRUE(pr.read_error);
    TEST_ASSERT_EQUAL(ES_FS_UNKNOWN, pr.kind);
    TEST_ASSERT_TRUE(pr.mbr);

    es_probe(NULL, &d, &pr); /* no reader at all */
    TEST_ASSERT_TRUE(pr.read_error);
}

static void test_words_and_cluster_size(void)
{
    TEST_ASSERT_EQUAL_STRING("fat", es_fs_kind_str(ES_FS_FAT));
    TEST_ASSERT_EQUAL_STRING("blank", es_fs_kind_str(ES_FS_BLANK));
    TEST_ASSERT_EQUAL_STRING("fat_corrupt", es_fs_kind_str(ES_FS_FAT_CORRUPT));
    TEST_ASSERT_EQUAL_STRING("unknown", es_fs_kind_str((es_fs_kind_t)99));
    TEST_ASSERT_EQUAL_STRING("absent",
                             external_storage_state_str(EXTERNAL_STORAGE_ABSENT));
    TEST_ASSERT_EQUAL_STRING("unreadable",
                             external_storage_state_str(EXTERNAL_STORAGE_UNREADABLE));
    TEST_ASSERT_EQUAL_STRING("formatting",
                             external_storage_state_str(EXTERNAL_STORAGE_FORMATTING));
    TEST_ASSERT_EQUAL_STRING("idle",
                             external_storage_format_state_str(EXTERNAL_STORAGE_FORMAT_IDLE));
    TEST_ASSERT_EQUAL_STRING("failed",
                             external_storage_format_state_str(EXTERNAL_STORAGE_FORMAT_FAILED));
    /* 16 KB up to and including 32 GiB, 32 KB above (the 64 GB bench card) */
    TEST_ASSERT_EQUAL_UINT32(16384, es_format_cluster_bytes(15931539456ull));
    TEST_ASSERT_EQUAL_UINT32(16384, es_format_cluster_bytes(32ull << 30));
    TEST_ASSERT_EQUAL_UINT32(32768, es_format_cluster_bytes(63864569856ull));
}

void app_main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_glitches_never_surface);
    RUN_TEST(test_clean_insert_and_remove);
    RUN_TEST(test_bouncing_insertion_settles);
    RUN_TEST(test_vbr_fat32_windows);
    RUN_TEST(test_vbr_fat16_bpb_only);
    RUN_TEST(test_vbr_exfat_and_ntfs);
    RUN_TEST(test_probe_super_floppy_fat);
    RUN_TEST(test_probe_blank_cards);
    RUN_TEST(test_probe_mbr_fat32_partition);
    RUN_TEST(test_probe_factory_exfat_card);
    RUN_TEST(test_probe_mbr_other_partitions);
    RUN_TEST(test_probe_signature_without_partitions);
    RUN_TEST(test_probe_read_errors);
    RUN_TEST(test_words_and_cluster_size);
    UNITY_END();
}
