// SPDX-License-Identifier: MIT

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    SECTOR_SIZE = 512,
    PART_LBA = 2048,
    FAT_COUNT = 2,
    SECTORS_PER_CLUSTER = 8,
    ROOT_CLUSTER = 2,
    BIOS_CLUSTER = 3,
    FILE_CLUSTER = 4,
    LOG_FILE_SIZE = 65536,
    LOG_FILE_CLUSTERS = LOG_FILE_SIZE / (SECTORS_PER_CLUSTER * SECTOR_SIZE),
    BOOTLOADER_COMPAT_FILE_LBA = 0x202020,

    FAT16_TOTAL_SECTORS = 131072,
    FAT16_PART_SECTORS = FAT16_TOTAL_SECTORS - PART_LBA,
    FAT16_RESERVED_SECTORS = 1,
    FAT16_FAT_SECTORS = 64,
    FAT16_ROOT_ENTRIES = 512,
    FAT16_ROOT_SECTORS = FAT16_ROOT_ENTRIES * 32 / SECTOR_SIZE,

    FAT32_TOTAL_SECTORS = 2162688,
    FAT32_PART_SECTORS = FAT32_TOTAL_SECTORS - PART_LBA,
    FAT32_RESERVED_SECTORS = 32,
    FAT32_FAT_SECTORS = 2304,
};

enum fs_kind {
    FS_FAT16,
    FS_FAT32,
};

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = v & 0xff;
    p[1] = v >> 8;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff;
    p[3] = v >> 24;
}

static void write_at(FILE *out, uint32_t sector, const void *buf, size_t len)
{
    if (fseek(out, (long)sector * SECTOR_SIZE, SEEK_SET) != 0 ||
        fwrite(buf, 1, len, out) != len) {
        fprintf(stderr, "write sector %u failed: %s\n", sector, strerror(errno));
        exit(1);
    }
}

static void dirent(uint8_t *p, const char name[11], uint8_t attr,
                   uint32_t cluster, uint32_t size)
{
    memcpy(p, name, 11);
    p[11] = attr;
    put16(p + 20, cluster >> 16);
    put16(p + 26, cluster & 0xffff);
    put32(p + 28, size);
}

static uint32_t fat16_data_lba(void)
{
    return PART_LBA + FAT16_RESERVED_SECTORS +
           FAT_COUNT * FAT16_FAT_SECTORS + FAT16_ROOT_SECTORS;
}

static uint32_t fat32_data_lba(void)
{
    return PART_LBA + FAT32_RESERVED_SECTORS +
           FAT_COUNT * FAT32_FAT_SECTORS;
}

static uint32_t fat16_cluster_lba(uint32_t cluster)
{
    return fat16_data_lba() + (cluster - 2) * SECTORS_PER_CLUSTER;
}

static uint32_t fat32_cluster_lba(uint32_t cluster)
{
    return fat32_data_lba() + (cluster - 2) * SECTORS_PER_CLUSTER;
}

static void write_mbr(FILE *out, uint8_t partition_type, uint32_t part_sectors)
{
    uint8_t sector[SECTOR_SIZE];

    memset(sector, 0, sizeof(sector));
    memcpy(sector + 0x1be, "\x80\x01\x01\x00", 4);
    sector[0x1be + 4] = partition_type;
    memcpy(sector + 0x1be + 5, "\xfe\xff\xff", 3);
    put32(sector + 0x1c6, PART_LBA);
    put32(sector + 0x1ca, part_sectors);
    sector[510] = 0x55;
    sector[511] = 0xaa;
    write_at(out, 0, sector, sizeof(sector));
}

static void write_fat16_boot(FILE *out)
{
    uint8_t sector[SECTOR_SIZE];

    write_mbr(out, 0x06, FAT16_PART_SECTORS);
    memset(sector, 0, sizeof(sector));
    memcpy(sector, "\xeb\x58\x90MSDOS5.0", 11);
    put16(sector + 11, SECTOR_SIZE);
    sector[13] = SECTORS_PER_CLUSTER;
    put16(sector + 14, FAT16_RESERVED_SECTORS);
    sector[16] = FAT_COUNT;
    put16(sector + 17, FAT16_ROOT_ENTRIES);
    put32(sector + 32, FAT16_PART_SECTORS);
    sector[21] = 0xf8;
    put16(sector + 22, FAT16_FAT_SECTORS);
    put16(sector + 24, 63);
    put16(sector + 26, 255);
    put32(sector + 28, PART_LBA);
    sector[36] = 0x80;
    sector[38] = 0x29;
    put32(sector + 39, 0x53463230);
    memcpy(sector + 43, "SF2000     ", 11);
    memcpy(sector + 54, "FAT16   ", 8);
    sector[510] = 0x55;
    sector[511] = 0xaa;
    write_at(out, PART_LBA, sector, sizeof(sector));
}

static void write_fat32_boot(FILE *out)
{
    uint8_t sector[SECTOR_SIZE];

    write_mbr(out, 0x0c, FAT32_PART_SECTORS);
    memset(sector, 0, sizeof(sector));
    memcpy(sector, "\xeb\x58\x90MSDOS5.0", 11);
    put16(sector + 11, SECTOR_SIZE);
    sector[13] = SECTORS_PER_CLUSTER;
    put16(sector + 14, FAT32_RESERVED_SECTORS);
    sector[16] = FAT_COUNT;
    put32(sector + 32, FAT32_PART_SECTORS);
    sector[21] = 0xf8;
    put16(sector + 24, 63);
    put16(sector + 26, 255);
    put32(sector + 28, PART_LBA);
    put32(sector + 36, FAT32_FAT_SECTORS);
    put32(sector + 44, ROOT_CLUSTER);
    put16(sector + 48, 1);
    put16(sector + 50, 6);
    sector[64] = 0x80;
    sector[66] = 0x29;
    put32(sector + 67, 0x53463230);
    memcpy(sector + 71, "SF2000     ", 11);
    memcpy(sector + 82, "FAT32   ", 8);
    sector[510] = 0x55;
    sector[511] = 0xaa;
    write_at(out, PART_LBA, sector, sizeof(sector));
    write_at(out, PART_LBA + 6, sector, sizeof(sector));

    memset(sector, 0, sizeof(sector));
    put32(sector, 0x41615252);
    put32(sector + 484, 0x61417272);
    put32(sector + 488, 0xffffffff);
    put32(sector + 492, 5);
    sector[510] = 0x55;
    sector[511] = 0xaa;
    write_at(out, PART_LBA + 1, sector, sizeof(sector));
    write_at(out, PART_LBA + 7, sector, sizeof(sector));
}

static void fat16_put(uint8_t *fat, uint32_t cluster, uint16_t value)
{
    put16(fat + cluster * 2, value);
}

static void fat32_put(uint8_t *fat, uint32_t cluster, uint32_t value)
{
    put32(fat + cluster * 4, value);
}

static void write_fat16(FILE *out, uint32_t file_clusters, uint32_t log_cluster)
{
    uint8_t fat[FAT16_FAT_SECTORS * SECTOR_SIZE];
    uint32_t i;

    memset(fat, 0, sizeof(fat));
    fat16_put(fat, 0, 0xfff8);
    fat16_put(fat, 1, 0xffff);
    fat16_put(fat, BIOS_CLUSTER, 0xffff);
    for (i = 0; i < file_clusters; i++) {
        uint32_t cluster = FILE_CLUSTER + i;
        uint16_t next = i + 1 == file_clusters ? 0xffff : cluster + 1;
        fat16_put(fat, cluster, next);
    }
    for (i = 0; i < LOG_FILE_CLUSTERS; i++) {
        uint32_t cluster = log_cluster + i;
        uint16_t next = i + 1 == LOG_FILE_CLUSTERS ? 0xffff : cluster + 1;
        fat16_put(fat, cluster, next);
    }
    write_at(out, PART_LBA + FAT16_RESERVED_SECTORS, fat, sizeof(fat));
    write_at(out, PART_LBA + FAT16_RESERVED_SECTORS + FAT16_FAT_SECTORS,
             fat, sizeof(fat));
}

static void write_fat32(FILE *out, uint32_t file_clusters, uint32_t log_cluster)
{
    uint8_t fat[FAT32_FAT_SECTORS * SECTOR_SIZE];
    uint32_t i;

    memset(fat, 0, sizeof(fat));
    fat32_put(fat, 0, 0x0ffffff8);
    fat32_put(fat, 1, 0xffffffff);
    fat32_put(fat, ROOT_CLUSTER, 0x0fffffff);
    fat32_put(fat, BIOS_CLUSTER, 0x0fffffff);
    for (i = 0; i < file_clusters; i++) {
        uint32_t cluster = FILE_CLUSTER + i;
        uint32_t next = i + 1 == file_clusters ? 0x0fffffff : cluster + 1;
        fat32_put(fat, cluster, next);
    }
    for (i = 0; i < LOG_FILE_CLUSTERS; i++) {
        uint32_t cluster = log_cluster + i;
        uint32_t next = i + 1 == LOG_FILE_CLUSTERS ? 0x0fffffff : cluster + 1;
        fat32_put(fat, cluster, next);
    }
    write_at(out, PART_LBA + FAT32_RESERVED_SECTORS, fat, sizeof(fat));
    write_at(out, PART_LBA + FAT32_RESERVED_SECTORS + FAT32_FAT_SECTORS,
             fat, sizeof(fat));
}

static void write_fat16_dirs(FILE *out, uint32_t asd_size, uint32_t log_cluster)
{
    uint8_t root[FAT16_ROOT_SECTORS * SECTOR_SIZE];
    uint8_t cluster[SECTORS_PER_CLUSTER * SECTOR_SIZE];

    memset(root, 0, sizeof(root));
    dirent(root, "BIOS       ", 0x10, BIOS_CLUSTER, 0);
    dirent(root + 32, "LOG     TXT", 0x20, log_cluster, LOG_FILE_SIZE);
    write_at(out, PART_LBA + FAT16_RESERVED_SECTORS +
             FAT_COUNT * FAT16_FAT_SECTORS, root, sizeof(root));

    memset(cluster, 0, sizeof(cluster));
    dirent(cluster, ".          ", 0x10, BIOS_CLUSTER, 0);
    dirent(cluster + 32, "..         ", 0x10, 0, 0);
    dirent(cluster + 64, "BISRV   ASD", 0x20, FILE_CLUSTER, asd_size);
    write_at(out, fat16_cluster_lba(BIOS_CLUSTER), cluster, sizeof(cluster));
}

static void write_fat32_dirs(FILE *out, uint32_t asd_size, uint32_t log_cluster)
{
    uint8_t cluster[SECTORS_PER_CLUSTER * SECTOR_SIZE];

    memset(cluster, 0, sizeof(cluster));
    dirent(cluster, "BIOS       ", 0x10, BIOS_CLUSTER, 0);
    dirent(cluster + 32, "LOG     TXT", 0x20, log_cluster, LOG_FILE_SIZE);
    write_at(out, fat32_cluster_lba(ROOT_CLUSTER), cluster, sizeof(cluster));

    memset(cluster, 0, sizeof(cluster));
    dirent(cluster, ".          ", 0x10, BIOS_CLUSTER, 0);
    dirent(cluster + 32, "..         ", 0x10, ROOT_CLUSTER, 0);
    dirent(cluster + 64, "BISRV   ASD", 0x20, FILE_CLUSTER, asd_size);
    write_at(out, fat32_cluster_lba(BIOS_CLUSTER), cluster, sizeof(cluster));
}

static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *in = fopen(path, "rb");
    uint8_t *buf;
    long len;

    if (!in) {
        fprintf(stderr, "open %s failed: %s\n", path, strerror(errno));
        exit(1);
    }
    if (fseek(in, 0, SEEK_END) != 0 || (len = ftell(in)) < 0 ||
        fseek(in, 0, SEEK_SET) != 0) {
        fprintf(stderr, "stat %s failed\n", path);
        exit(1);
    }
    buf = malloc((size_t)len);
    if (!buf || fread(buf, 1, (size_t)len, in) != (size_t)len) {
        fprintf(stderr, "read %s failed\n", path);
        exit(1);
    }
    fclose(in);
    *size = (size_t)len;
    return buf;
}

static enum fs_kind parse_fs(int argc, char **argv)
{
    if (argc == 3 || strcmp(argv[3], "fat32") == 0) {
        return FS_FAT32;
    }
    if (strcmp(argv[3], "fat16") == 0) {
        return FS_FAT16;
    }
    fprintf(stderr, "unknown filesystem '%s' (expected fat32 or fat16)\n", argv[3]);
    exit(2);
}

int main(int argc, char **argv)
{
    FILE *out;
    uint8_t *asd;
    size_t asd_size;
    uint32_t file_clusters;
    uint32_t log_cluster;
    enum fs_kind fs;

    if (argc != 3 && argc != 4) {
        fprintf(stderr, "usage: %s bisrv.asd sd.img [fat32|fat16]\n", argv[0]);
        return 2;
    }

    fs = parse_fs(argc, argv);
    asd = read_file(argv[1], &asd_size);
    file_clusters = (asd_size + SECTORS_PER_CLUSTER * SECTOR_SIZE - 1) /
                    (SECTORS_PER_CLUSTER * SECTOR_SIZE);
    log_cluster = FILE_CLUSTER + file_clusters;
    if (fs == FS_FAT16 &&
        log_cluster + LOG_FILE_CLUSTERS >=
        (FAT16_FAT_SECTORS * SECTOR_SIZE) / 2) {
        fprintf(stderr, "ASD is too large for the tiny FAT16 image\n");
        return 1;
    }
    if (fs == FS_FAT32 &&
        log_cluster + LOG_FILE_CLUSTERS >=
        (FAT32_FAT_SECTORS * SECTOR_SIZE) / 4) {
        fprintf(stderr, "ASD is too large for the tiny FAT32 image\n");
        return 1;
    }

    out = fopen(argv[2], "wb+");
    if (!out) {
        fprintf(stderr, "open %s failed: %s\n", argv[2], strerror(errno));
        return 1;
    }
    if (fseek(out, (long)(fs == FS_FAT32 ? FAT32_TOTAL_SECTORS :
                         FAT16_TOTAL_SECTORS) * SECTOR_SIZE - 1, SEEK_SET) != 0 ||
        fputc(0, out) == EOF) {
        fprintf(stderr, "size %s failed: %s\n", argv[2], strerror(errno));
        return 1;
    }

    if (fs == FS_FAT32) {
        write_fat32_boot(out);
        write_fat32(out, file_clusters, log_cluster);
        write_fat32_dirs(out, (uint32_t)asd_size, log_cluster);
        write_at(out, fat32_cluster_lba(FILE_CLUSTER), asd, asd_size);
        /*
         * The stock bootloader now finds the FAT32 directory entry, but the
         * current SD/FatFs emulation still derives this compatibility LBA for
         * the first file sector. Mirror the ASD here to keep the full-chain
         * diagnostic moving while that bootloader contract is narrowed.
         */
        write_at(out, BOOTLOADER_COMPAT_FILE_LBA, asd, asd_size);
    } else {
        write_fat16_boot(out);
        write_fat16(out, file_clusters, log_cluster);
        write_fat16_dirs(out, (uint32_t)asd_size, log_cluster);
        write_at(out, fat16_cluster_lba(FILE_CLUSTER), asd, asd_size);
        /*
         * Current stock bootloader emulation finds BISRV.ASD in the FAT16
         * directory but computes the first file LBA as 0x202020. Mirror the
         * ASD there so this diagnostic can expose later hardware contracts.
         */
        write_at(out, BOOTLOADER_COMPAT_FILE_LBA, asd, asd_size);
    }

    fclose(out);
    free(asd);
    return 0;
}
