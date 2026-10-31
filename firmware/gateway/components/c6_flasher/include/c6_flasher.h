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
#define C6FW_FORMAT_VERSION 3 /* 3 adds the slots layout; version 2 images are still read */
#define C6FW_FORMAT_MIN 2
#define C6FW_MAX_ENTRIES 8
#define C6FW_SEG_WRITE 1
#define C6FW_SEG_ERASE 2
#define C6FW_SEG_PRESERVE 4
#define C6FW_SEG_DEFLATE 8 /* stored data is a zlib stream of stored_size bytes */
/* Marker (no data): the bundle only selects its slot of the layout the "slots" bundle installs;
 * md5 holds the first 16 bytes of that bundle's sha256. */
#define C6FW_SEG_SLOTS 16
#define C6FW_SLOTS_BUNDLE "slots"
/* entry offset of a bundle a radio's local image does not hold: it is read from the package in PSRAM */
#define C6FW_REMOTE 0xFFFFFFFFu
/* chip of a package and of a radio (esp_image_header_t.chip_id) */
#define C6FW_CHIP_C6 13
#define C6FW_CHIP_C5 23

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
    uint32_t stored_size;
    uint32_t flags;
    uint8_t md5[16];
} c6fw_segment_t;

typedef struct {
    bool busy;
    int percent;
    int64_t started_us; /* esp_timer time when the current operation started */
    char stage[48];
    char last_error[96];
} c6_progress_t;

/* The radios: A is the C6 of the board (Kconfig GW_C6_*), B the external radio on JP1 (GW_RADIO_B_*,
 * a C6 or C5, docs/dual-radio.md). Each has its own progress, chip data and Zigbee snapshots. */
typedef enum { C6_RADIO_A, C6_RADIO_B, C6_RADIOS } c6_radio_t;

/* A radio's UART, reset and boot lines. */
typedef struct {
    int uart_port;
    int tx, rx; /* P4 GPIOs to the radio's RX and from its TX */
    int reset, boot;
    bool reset_open_drain; /* reset only pulled low, released otherwise (the radio's own pull-up holds EN) */
} c6_radio_pins_t;

const c6_radio_pins_t *c6_radio_pins(c6_radio_t radio);
/* Drives reset high and leaves BOOT released (the radio runs its application). */
void c6_radio_init_pins(const c6_radio_pins_t *pins);
/* Pulses the reset line; the radio boots its application. */
void c6_reset(c6_radio_t radio);
/* Holds the radio in reset (true) or releases it. */
void c6_hold_reset(c6_radio_t radio, bool hold);

/* Directory of the bundles for a radio: the package in PSRAM when it is for the radio's chip, else
 * the radio's local image (NVS: what it was last flashed from, the large bundles C6FW_REMOTE). */
esp_err_t c6fw_read_dir(c6_radio_t radio, c6fw_dir_t *dir);
esp_err_t c6fw_find(c6_radio_t radio, const char *name, c6fw_entry_t *entry);
/* Recomputes the bundle sha256 and compares it to the directory. */
esp_err_t c6fw_verify(const c6fw_entry_t *entry);
/* The data of the bundle can be read now (PSRAM package or a local image). */
bool c6fw_available(const c6fw_entry_t *entry);

/*
 * A whole package (c6fw.bin for the C6, c5fw.bin for the C5) goes into a PSRAM buffer that a software
 * restart keeps: fill c6fw_stage_buffer(), then c6fw_stage_commit() checks it (directory, sha256 of
 * every bundle) and tells its chip (C6FW_CHIP_*). The radio of that chip is flashed from it at the
 * next start; then it is dropped. c6fw_stage_load() at start takes over a package staged before a
 * software restart. Nothing of it is written to the gateway's flash.
 */
uint8_t *c6fw_stage_buffer(uint32_t *capacity);
esp_err_t c6fw_stage_commit(uint32_t len, uint32_t *chip);
bool c6fw_stage_load(void);
/* C6FW_CHIP_* of the package in PSRAM, 0 when none */
uint32_t c6fw_stage_chip(void);
void c6fw_stage_drop(void);
/* sha256 of the whole package the radio was last flashed from, false when unknown */
bool c6fw_package_sha(c6_radio_t radio, uint8_t sha[32]);
/* C6FW_CHIP_* of a radio (radio B: what its bootloader reported, 0 before it was ever reached) */
uint32_t c6fw_radio_chip(c6_radio_t radio);

/*
 * Flashes a bundle to the C6 through its ROM bootloader. When the bundle being replaced
 * (current_name) has PRESERVE segments, their content is first saved to the c6data
 * partition; when the new bundle has them, its saved content is written back (or the
 * region erased if that bundle has no snapshot yet). A bundle with a SLOTS marker first
 * installs the "slots" bundle when the C6 does not hold that exact layout yet.
 */
esp_err_t c6_flash_bundle(c6_radio_t radio, const c6fw_entry_t *entry, const char *current_name);
/* Everything c6_flash_bundle() would write for entry is readable now (else a package is needed). */
bool c6_flash_ready(c6_radio_t radio, const c6fw_entry_t *entry);
/* Forgets that the slots layout is on the radio: the next slot bundle installs it again. */
void c6_slots_invalidate(c6_radio_t radio);

/* Snapshot of a bundle's preserved regions on a radio (Zigbee network data), one slot per radio and bundle. */
/* saved_unix (may be NULL): when it was taken, 0 when unknown. */
esp_err_t c6_snapshot_info(c6_radio_t radio, const char *bundle, uint32_t *total_size, uint32_t *generation, uint32_t *saved_unix);
/* Saves the preserved regions of the running bundle without flashing (the radio restarts). */
esp_err_t c6_snapshot_take(c6_radio_t radio, const char *bundle);
esp_err_t c6_snapshot_read(c6_radio_t radio, const char *bundle, uint32_t offset, void *buf, size_t len);

/* Streams the whole C6 flash (backup of the factory firmware). The C6 restarts afterwards. */
typedef esp_err_t (*c6_sink_t)(void *ctx, const void *data, size_t len);
esp_err_t c6_read_flash(c6_radio_t radio, c6_sink_t sink, void *ctx);
/* Detected flash size, valid after c6_read_flash() started. */
uint32_t c6_flash_size(c6_radio_t radio);

const c6_progress_t *c6_progress(c6_radio_t radio);

/* C6 chip data read from its ROM bootloader at the last flashing or backup, kept in NVS. */
typedef struct {
    bool known;
    uint8_t mac[6];
    uint16_t revision; /* major * 100 + minor */
    uint32_t flash_size;
    uint32_t flashes; /* successful bundle flashes since the data was first read */
} c6_chip_t;
const c6_chip_t *c6_chip_info(c6_radio_t radio);


/* Wiring test result (radio_probe.c). */
typedef struct {
    bool wired;         /* ROM sync: TX, RX, RESET and BOOT work */
    bool banner;        /* without sync: the ROM banner came after a reset (power, RESET, radio TX work) */
    bool download_mode; /* with BOOT held low the radio entered download mode (BOOT works) */
    char chip[16];
    uint32_t chip_id; /* C6FW_CHIP_* when a C6 or C5, else 0 */
    uint8_t mac[6];
    uint16_t revision; /* major * 100 + minor */
    uint32_t flash_size;
    char verdict[160];
} c6_probe_t;

/* Resets the radio a few times and tells which lines work; ESP_OK when all do. Takes a few seconds. */
esp_err_t c6_radio_probe(c6_radio_t radio, c6_probe_t *result);
/* Records the chip (C6FW_CHIP_*) a radio's bootloader reported. */
void c6_radio_set_chip(c6_radio_t radio, uint32_t chip);

#ifdef __cplusplus
}
#endif
