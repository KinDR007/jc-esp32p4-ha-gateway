#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* On-flash format written by scripts/pack_c6fw.py. */
#define C6FW_DIR_MAGIC "C6FW"
#define C6FW_BUNDLE_MAGIC "C6BN"
#define C6FW_FORMAT_VERSION 1
#define C6FW_MAX_ENTRIES 8
#define C6FW_SEG_WRITE 1
#define C6FW_SEG_ERASE 2
#define C6FW_SEG_PRESERVE 4

typedef struct __attribute__((packed)) {
    char name[16];
    char version[32];
    uint32_t offset;
    uint32_t size;
    uint8_t sha256[32];
} c6fw_entry_t;

typedef struct __attribute__((packed)) {
    char magic[4];
    uint32_t format;
    uint32_t count;
    c6fw_entry_t entries[C6FW_MAX_ENTRIES];
    uint32_t crc32;
} c6fw_dir_t;

typedef struct __attribute__((packed)) {
    uint32_t addr;
    uint32_t size;
    uint32_t data_off;
    uint32_t flags;
    uint8_t md5[16];
} c6fw_segment_t;

typedef struct {
    bool busy;
    int percent;
    char stage[48];
    char last_error[96];
} c6_progress_t;

/* Pins and UART from Kconfig (GW_C6_*). Drives reset high and leaves BOOT released. */
void c6_init_pins(void);
/* Pulses CHIP_PU; the C6 boots its application. */
void c6_reset(void);
/* Holds the C6 in reset (true) or releases it. */
void c6_hold_reset(bool hold);

/* Reads and validates the directory of the c6fw partition. */
esp_err_t c6fw_read_dir(c6fw_dir_t *dir);
esp_err_t c6fw_find(const char *name, c6fw_entry_t *entry);
/* Recomputes the bundle sha256 and compares it to the directory. */
esp_err_t c6fw_verify(const c6fw_entry_t *entry);
/* Validates a complete c6fw image held in the partition (after an upload). */
esp_err_t c6fw_verify_all(void);

/*
 * Flashes a bundle to the C6 through its ROM bootloader. When the bundle being replaced
 * (current_name) has PRESERVE segments, their content is first saved to the c6data
 * partition; when the new bundle has them, its saved content is written back (or the
 * region erased if that bundle has no snapshot yet).
 */
esp_err_t c6_flash_bundle(const c6fw_entry_t *entry, const char *current_name);

/* Snapshot of a bundle's preserved C6 regions (Zigbee network data), one slot per bundle. */
esp_err_t c6_snapshot_info(const char *bundle, uint32_t *total_size, uint32_t *generation);
esp_err_t c6_snapshot_read(const char *bundle, uint32_t offset, void *buf, size_t len);

const c6_progress_t *c6_progress(void);

#ifdef __cplusplus
}
#endif
