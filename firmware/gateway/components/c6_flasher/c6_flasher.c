#include "c6_flasher.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "driver/gpio.h"
#include "esp32_port.h"
#include "esp_check.h"
#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_loader.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "miniz.h"
#include "sdkconfig.h"

static const char *TAG = "c6_flasher";

/* MD5 of esp-serial-flasher (private header; struct MD5Context comes with esp_loader.h). */
void MD5Init(struct MD5Context *context);
void MD5Update(struct MD5Context *context, unsigned char const *buf, unsigned len);
void MD5Final(unsigned char digest[16], struct MD5Context *context);

#define C6FW_SUBTYPE 0x40
#define C6DATA_SUBTYPE 0x41
#define FLASH_BLOCK 4096
/* Compressed blocks as esptool sends them to a ROM loader (no stub): larger ones corrupt. */
#define DEFLATE_BLOCK 1024
#define FLASH_BAUD 460800
#define SNAP_MAGIC "C6SN"
#define SNAP_MAX 4
#define SNAP_DATA_OFFSET 0x1000

typedef struct __attribute__((packed)) {
    uint32_t addr;
    uint32_t size;
    uint32_t data_off;
    uint32_t crc32;
} snap_entry_t;

typedef struct __attribute__((packed)) {
    char magic[4];
    char bundle[16];
    uint32_t count;
    uint32_t generation;
    snap_entry_t entries[SNAP_MAX];
    uint32_t crc32;
    uint32_t saved_unix; /* not in the crc: headers written before it have 0xffffffff, 0 = clock unset */
} snap_header_t;

/* One radio: the C6 of the board (A) or the external radio on JP1 (B, docs/dual-radio.md). */
typedef struct {
    c6_radio_pins_t pins;
    const char *name;     /* "C6" / "radio B" in messages */
    const char *nvs_ns;   /* chip data and the installed slots layout */
    const char *snap_tag; /* prefix of its Zigbee snapshots in c6data ("" keeps the names of radio A) */
    c6_progress_t progress;
    c6_chip_t chip;
    bool chip_loaded;
    uint32_t flash_size;
} radio_t;

static radio_t s_radios[C6_RADIOS] = {
    [C6_RADIO_A] = {.pins = {.uart_port = CONFIG_GW_C6_UART_PORT, .tx = CONFIG_GW_C6_UART_TX, .rx = CONFIG_GW_C6_UART_RX,
                             .reset = CONFIG_GW_C6_RESET_GPIO, .boot = CONFIG_GW_C6_BOOT_GPIO},
                    .name = "C6", .nvs_ns = "c6chip", .snap_tag = ""},
    [C6_RADIO_B] = {.pins = {.uart_port = CONFIG_GW_RADIO_B_UART_PORT, .tx = CONFIG_GW_RADIO_B_UART_TX,
                             .rx = CONFIG_GW_RADIO_B_UART_RX, .reset = CONFIG_GW_RADIO_B_RESET_GPIO,
                             .boot = CONFIG_GW_RADIO_B_BOOT_GPIO, .reset_open_drain = true},
                    .name = "radio B", .nvs_ns = "radiob", .snap_tag = "b/"},
};

static radio_t *radio(c6_radio_t r)
{
    return &s_radios[r == C6_RADIO_B ? C6_RADIO_B : C6_RADIO_A];
}

static void progress(radio_t *rd, int percent, const char *stage)
{
    rd->progress.percent = percent;
    strlcpy(rd->progress.stage, stage, sizeof(rd->progress.stage));
}

const c6_progress_t *c6_progress(c6_radio_t r)
{
    return &radio(r)->progress;
}

const c6_radio_pins_t *c6_radio_pins(c6_radio_t r)
{
    return &radio(r)->pins;
}

void c6_hold_reset(c6_radio_t r, bool hold)
{
    gpio_set_level(radio(r)->pins.reset, hold ? 0 : 1);
}

void c6_reset(c6_radio_t r)
{
    c6_hold_reset(r, true);
    vTaskDelay(pdMS_TO_TICKS(20));
    c6_hold_reset(r, false);
}

static const esp_partition_t *partition(uint8_t subtype)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, subtype, NULL);
}

/* ---- radio firmware packages ----
 * The gateway keeps no radio firmware in its flash (docs/dual-radio.md). A package (c6fw.bin for
 * the C6, c5fw.bin for the C5, scripts/pack_c6fw.py) is downloaded from the release or uploaded
 * into a PSRAM buffer that a software restart keeps, flashed into the radio at the next start and
 * dropped. For each radio the NVS keeps its "local" image: the directory of the package it was
 * flashed from with the small bundles that only select a slot (the large ones C6FW_REMOTE), so a
 * role switch works without the package and without internet. */
#define CHIP_NVS_NS "c6chip" /* radio A: the namespace of the single-radio gateway */
#define STAGE_MAX (4096 * 1024)
#define STAGE_MAGIC 0x4b505336 /* "6SPK" */
#define LOCAL_MAX_BUNDLE (32 * 1024)
#define LOCAL_KEY "local"

typedef struct {
    /* the noinit section starts the PSRAM, and the timing tuning at every start writes its test
     * pattern at PSRAM 0x80 (MSPI_TIMING_PSRAM_TEST_DATA_ADDR, 128 bytes): keep clear of it */
    uint8_t tuning_area[4096];
    uint32_t magic;
    uint32_t len;
    uint32_t crc;
    uint32_t chip; /* C6FW_CHIP_* of the firmwares in the package */
    uint8_t data[STAGE_MAX];
} stage_t;

static EXT_RAM_NOINIT_ATTR stage_t s_stage;
static const uint8_t *s_pkg; /* validated package in PSRAM, NULL when none */
static uint32_t s_pkg_len;
static uint32_t s_pkg_chip;

/* local image of each radio (NVS), loaded on first use; NULL when the radio has none */
static uint8_t *s_local[C6_RADIOS];
static uint32_t s_local_len[C6_RADIOS];
static bool s_local_loaded[C6_RADIOS];

static bool dir_valid(const c6fw_dir_t *dir)
{
    return memcmp(dir->magic, C6FW_DIR_MAGIC, 4) == 0 && dir->format >= C6FW_FORMAT_MIN &&
           dir->format <= C6FW_FORMAT_VERSION && dir->count > 0 && dir->count <= C6FW_MAX_ENTRIES &&
           esp_rom_crc32_le(0, (const uint8_t *)dir, offsetof(c6fw_dir_t, crc32)) == dir->crc32;
}

/* Directory and small bundles of a package (large ones C6FW_REMOTE); *len its size. */
static uint8_t *local_image(const uint8_t *src_dir, esp_err_t (*read)(void *ctx, uint32_t off, void *buf, size_t n),
                            void *ctx, uint32_t *len)
{
    uint32_t cursor = sizeof(c6fw_dir_t);
    const c6fw_dir_t *sd = (const c6fw_dir_t *)src_dir;
    for (uint32_t i = 0; i < sd->count; i++) {
        if (sd->entries[i].offset != C6FW_REMOTE && sd->entries[i].size <= LOCAL_MAX_BUNDLE) {
            cursor += (sd->entries[i].size + 15) & ~15u;
        }
    }
    uint8_t *img = heap_caps_calloc(1, cursor, MALLOC_CAP_SPIRAM);
    if (!img) {
        return NULL;
    }
    c6fw_dir_t *dir = (c6fw_dir_t *)img;
    memcpy(dir, sd, sizeof(*dir));
    cursor = sizeof(c6fw_dir_t);
    for (uint32_t i = 0; i < dir->count; i++) {
        c6fw_entry_t *e = &dir->entries[i];
        if (e->offset != C6FW_REMOTE && e->size <= LOCAL_MAX_BUNDLE && read(ctx, e->offset, img + cursor, e->size) == ESP_OK) {
            e->offset = cursor;
            cursor += (e->size + 15) & ~15u;
        } else {
            e->offset = C6FW_REMOTE;
        }
    }
    dir->crc32 = esp_rom_crc32_le(0, img, offsetof(c6fw_dir_t, crc32));
    *len = cursor;
    return img;
}

static esp_err_t read_partition(void *ctx, uint32_t off, void *buf, size_t n)
{
    return esp_partition_read((const esp_partition_t *)ctx, off, buf, n);
}

static esp_err_t read_memory(void *ctx, uint32_t off, void *buf, size_t n)
{
    memcpy(buf, (const uint8_t *)ctx + off, n);
    return ESP_OK;
}

static void local_store(radio_t *rd, uint8_t *img, uint32_t len)
{
    int r = rd - s_radios;
    nvs_handle_t h;
    if (nvs_open(rd->nvs_ns, NVS_READWRITE, &h) == ESP_OK) {
        if (img) {
            nvs_set_blob(h, LOCAL_KEY, img, len);
        } else {
            nvs_erase_key(h, LOCAL_KEY);
        }
        nvs_commit(h);
        nvs_close(h);
    }
    free(s_local[r]);
    s_local[r] = img;
    s_local_len[r] = img ? len : 0;
    s_local_loaded[r] = true;
}

/* Radio A of a gateway from before: its c6fw partition held the directory and the small bundles
 * (or the whole package); copied into the NVS once, the partition is not read afterwards. */
static void local_migrate(radio_t *rd)
{
    const esp_partition_t *part = partition(C6FW_SUBTYPE);
    c6fw_dir_t dir;
    if (!part || esp_partition_read(part, 0, &dir, sizeof(dir)) != ESP_OK || !dir_valid(&dir)) {
        return;
    }
    for (uint32_t i = 0; i < dir.count; i++) {
        if (dir.entries[i].offset != C6FW_REMOTE && dir.entries[i].offset + dir.entries[i].size > part->size) {
            return;
        }
    }
    uint32_t len;
    uint8_t *img = local_image((const uint8_t *)&dir, read_partition, (void *)part, &len);
    if (img) {
        ESP_LOGI(TAG, "%s: local image taken over from the c6fw partition (%lu B)", rd->name, (unsigned long)len);
        local_store(rd, img, len);
    }
}

static const uint8_t *local_get(c6_radio_t r)
{
    radio_t *rd = radio(r);
    if (!s_local_loaded[r]) {
        s_local_loaded[r] = true;
        nvs_handle_t h;
        size_t len = 0;
        if (nvs_open(rd->nvs_ns, NVS_READONLY, &h) == ESP_OK) {
            if (nvs_get_blob(h, LOCAL_KEY, NULL, &len) == ESP_OK && len >= sizeof(c6fw_dir_t)) {
                s_local[r] = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
                if (s_local[r] && nvs_get_blob(h, LOCAL_KEY, s_local[r], &len) == ESP_OK &&
                    dir_valid((const c6fw_dir_t *)s_local[r])) {
                    s_local_len[r] = len;
                } else {
                    free(s_local[r]);
                    s_local[r] = NULL;
                }
            }
            nvs_close(h);
        }
        if (!s_local[r] && r == C6_RADIO_A) {
            local_migrate(rd);
        }
    }
    return s_local[r];
}

/* C6FW_CHIP_* of a radio: radio A is the C6 of the board, radio B what its bootloader reported. */
static uint32_t radio_chip(c6_radio_t r)
{
    if (r == C6_RADIO_A) {
        return C6FW_CHIP_C6;
    }
    nvs_handle_t h;
    uint32_t chip = 0;
    if (nvs_open(radio(r)->nvs_ns, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, "chip", &chip);
        nvs_close(h);
    }
    return chip;
}

esp_err_t c6fw_read_dir(c6_radio_t r, c6fw_dir_t *dir)
{
    const uint8_t *src = s_pkg && s_pkg_chip == radio_chip(r) ? s_pkg : local_get(r);
    if (!src) {
        return ESP_ERR_NOT_FOUND;
    }
    memcpy(dir, src, sizeof(*dir));
    for (uint32_t i = 0; i < dir->count; i++) {
        dir->entries[i].name[sizeof(dir->entries[i].name) - 1] = '\0';
        dir->entries[i].version[sizeof(dir->entries[i].version) - 1] = '\0';
    }
    return ESP_OK;
}

esp_err_t c6fw_find(c6_radio_t r, const char *name, c6fw_entry_t *entry)
{
    c6fw_dir_t dir;
    ESP_RETURN_ON_ERROR(c6fw_read_dir(r, &dir), TAG, "no package or local image for the radio");
    for (uint32_t i = 0; i < dir.count; i++) {
        if (strcmp(dir.entries[i].name, name) == 0) {
            *entry = dir.entries[i];
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

/* offset of entry e (same name and sha256, data present) in an image */
static bool image_find(const uint8_t *img, uint32_t len, const c6fw_entry_t *e, uint32_t *offset)
{
    if (!img) {
        return false;
    }
    const c6fw_dir_t *dir = (const c6fw_dir_t *)img;
    for (uint32_t i = 0; i < dir->count && i < C6FW_MAX_ENTRIES; i++) {
        const c6fw_entry_t *p = &dir->entries[i];
        if (strncmp(p->name, e->name, sizeof(p->name)) == 0 && memcmp(p->sha256, e->sha256, 32) == 0 &&
            p->offset != C6FW_REMOTE && p->offset + p->size <= len) {
            *offset = p->offset;
            return true;
        }
    }
    return false;
}

/* The bytes of a bundle: from the package in PSRAM, else from a radio's local image. */
static const uint8_t *bundle_data(const c6fw_entry_t *e)
{
    uint32_t off;
    if (image_find(s_pkg, s_pkg_len, e, &off)) {
        return s_pkg + off;
    }
    for (int r = 0; r < C6_RADIOS; r++) {
        if (image_find(local_get(r), s_local_len[r], e, &off)) {
            return s_local[r] + off;
        }
    }
    return NULL;
}

static esp_err_t fw_read(const c6fw_entry_t *e, uint32_t off, void *buf, size_t n)
{
    const uint8_t *data = bundle_data(e);
    if (!data || off + n > e->size) {
        return ESP_ERR_NOT_FOUND;
    }
    memcpy(buf, data + off, n);
    return ESP_OK;
}

bool c6fw_available(const c6fw_entry_t *e)
{
    return bundle_data(e) != NULL;
}

esp_err_t c6fw_verify(const c6fw_entry_t *entry)
{
    const uint8_t *data = bundle_data(entry);
    uint8_t digest[32];
    if (!data) {
        return ESP_ERR_NOT_FOUND;
    }
    mbedtls_sha256(data, entry->size, digest, 0);
    return memcmp(digest, entry->sha256, sizeof(digest)) == 0 ? ESP_OK : ESP_ERR_INVALID_CRC;
}

/* Checks a package in memory: directory, format and the sha256 of every bundle. */
static esp_err_t pkg_check(const uint8_t *pkg, uint32_t len)
{
    const c6fw_dir_t *dir = (const c6fw_dir_t *)pkg;
    if (len < sizeof(*dir) || !dir_valid(dir) || dir->format != C6FW_FORMAT_VERSION) {
        return ESP_ERR_INVALID_VERSION;
    }
    for (uint32_t i = 0; i < dir->count; i++) {
        const c6fw_entry_t *e = &dir->entries[i];
        uint8_t sha[32];
        if (e->offset == C6FW_REMOTE || e->offset + e->size > len) {
            return ESP_ERR_INVALID_SIZE;
        }
        mbedtls_sha256(pkg + e->offset, e->size, sha, 0);
        if (memcmp(sha, e->sha256, 32) != 0) {
            return ESP_ERR_INVALID_CRC;
        }
    }
    return ESP_OK;
}

static uint32_t pkg_chip(void);

bool c6fw_package_sha(c6_radio_t r, uint8_t sha[32])
{
    nvs_handle_t h;
    size_t len = 32;
    bool ok = false;
    if (nvs_open(radio(r)->nvs_ns, NVS_READONLY, &h) == ESP_OK) {
        ok = nvs_get_blob(h, "pkg", sha, &len) == ESP_OK && len == 32;
        nvs_close(h);
    }
    return ok;
}

uint8_t *c6fw_stage_buffer(uint32_t *capacity)
{
    s_stage.magic = 0;
    s_pkg = NULL;
    *capacity = STAGE_MAX;
    return s_stage.data;
}

static void stage_sync(uint32_t len)
{
    /* PSRAM sits behind a write-back cache: write it out, a restart would drop what is still cached */
    esp_cache_msync(&s_stage.magic, offsetof(stage_t, data) - offsetof(stage_t, magic) + len,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

esp_err_t c6fw_stage_commit(uint32_t len, uint32_t *chip)
{
    if (len == 0 || len > STAGE_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    ESP_RETURN_ON_ERROR(pkg_check(s_stage.data, len), TAG, "package");
    s_pkg = s_stage.data;
    s_pkg_len = len;
    s_pkg_chip = pkg_chip();
    if (!s_pkg_chip) {
        s_pkg = NULL;
        ESP_LOGE(TAG, "package without a bootloader image: chip unknown");
        return ESP_ERR_NOT_SUPPORTED;
    }
    s_stage.len = len;
    s_stage.chip = s_pkg_chip;
    s_stage.crc = esp_rom_crc32_le(0, s_stage.data, len);
    s_stage.magic = STAGE_MAGIC;
    stage_sync(len);
    if (chip) {
        *chip = s_pkg_chip;
    }
    ESP_LOGI(TAG, "package for chip %lu in PSRAM (%lu B)", (unsigned long)s_pkg_chip, (unsigned long)len);
    return ESP_OK;
}

bool c6fw_stage_load(void)
{
    if (s_stage.magic != STAGE_MAGIC || s_stage.len == 0 || s_stage.len > STAGE_MAX ||
        esp_rom_crc32_le(0, s_stage.data, s_stage.len) != s_stage.crc) {
        s_stage.magic = 0;
        return false;
    }
    s_pkg = s_stage.data;
    s_pkg_len = s_stage.len;
    s_pkg_chip = s_stage.chip;
    ESP_LOGI(TAG, "package for chip %lu kept in PSRAM across the restart (%lu B)", (unsigned long)s_pkg_chip,
             (unsigned long)s_stage.len);
    return true;
}

uint32_t c6fw_stage_chip(void)
{
    return s_pkg ? s_pkg_chip : 0;
}

void c6fw_stage_drop(void)
{
    s_pkg = NULL;
    s_stage.magic = 0;
    stage_sync(0);
}

uint32_t c6fw_radio_chip(c6_radio_t r)
{
    return radio_chip(r);
}

void c6_radio_set_chip(c6_radio_t r, uint32_t chip)
{
    if (r == C6_RADIO_A || radio_chip(r) == chip) {
        return;
    }
    nvs_handle_t h;
    if (nvs_open(radio(r)->nvs_ns, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, "chip", chip);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ---- bundle access ---- */

typedef struct {
    c6fw_entry_t entry;
    uint32_t count;
    c6fw_segment_t *segs;
} bundle_t;

static esp_err_t bundle_load(const c6fw_entry_t *entry, bundle_t *b)
{
    char magic[4];

    b->entry = *entry;
    b->segs = NULL;
    ESP_RETURN_ON_ERROR(fw_read(entry, 0, magic, 4), TAG, "read");
    ESP_RETURN_ON_ERROR(fw_read(entry, 4, &b->count, 4), TAG, "read");
    ESP_RETURN_ON_FALSE(memcmp(magic, C6FW_BUNDLE_MAGIC, 4) == 0 && b->count > 0 && b->count < 32,
                        ESP_ERR_INVALID_VERSION, TAG, "bad bundle header");
    b->segs = calloc(b->count, sizeof(c6fw_segment_t));
    ESP_RETURN_ON_FALSE(b->segs, ESP_ERR_NO_MEM, TAG, "no mem");
    return fw_read(entry, 8, b->segs, b->count * sizeof(c6fw_segment_t));
}

static void bundle_free(bundle_t *b)
{
    free(b->segs);
    b->segs = NULL;
}

static const c6fw_segment_t *bundle_find_flag(const bundle_t *b, uint32_t flag)
{
    for (uint32_t i = 0; i < b->count; i++) {
        if (b->segs[i].flags & flag) {
            return &b->segs[i];
        }
    }
    return NULL;
}

static bool bundle_has_preserve(const bundle_t *b)
{
    return bundle_find_flag(b, C6FW_SEG_PRESERVE) != NULL;
}

/* ---- ROM loader session ---- */

typedef struct {
    esp_loader_t loader;
    esp32_port_t port;
    radio_t *radio;
} session_t;

/* ---------------------------------------------------------------- chip info */

static void chip_info_save(radio_t *rd)
{
    nvs_handle_t h;
    if (nvs_open(rd->nvs_ns, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "info", &rd->chip, sizeof(rd->chip));
        nvs_commit(h);
        nvs_close(h);
    }
}

const c6_chip_t *c6_chip_info(c6_radio_t r)
{
    radio_t *rd = radio(r);
    if (!rd->chip_loaded) {
        nvs_handle_t h;
        size_t len = sizeof(rd->chip);
        if (nvs_open(rd->nvs_ns, NVS_READONLY, &h) == ESP_OK) {
            if (nvs_get_blob(h, "info", &rd->chip, &len) != ESP_OK || len != sizeof(rd->chip)) {
                memset(&rd->chip, 0, sizeof(rd->chip));
            }
            nvs_close(h);
        }
        rd->chip_loaded = true;
    }
    return &rd->chip;
}

/* MAC, revision and flash size while the ROM bootloader is connected; failures only leave old data. */
static void chip_info_read(session_t *s)
{
    radio_t *rd = s->radio;
    c6_chip_info(rd - s_radios);
    uint8_t mac[6];
    uint16_t rev;
    uint32_t size;
    if (esp_loader_read_mac(&s->loader, mac) == ESP_LOADER_SUCCESS) {
        memcpy(rd->chip.mac, mac, sizeof(mac));
        rd->chip.known = true;
    }
    if (esp_loader_get_chip_revision(&s->loader, &rev) == ESP_LOADER_SUCCESS) {
        rd->chip.revision = rev;
    }
    if (esp_loader_flash_detect_size(&s->loader, &size) == ESP_LOADER_SUCCESS) {
        rd->chip.flash_size = size;
    }
    chip_info_save(rd);
}

static esp_err_t session_open(session_t *s, radio_t *rd)
{
    memset(s, 0, sizeof(*s));
    s->radio = rd;
    s->port.port.ops = &esp32_uart_ops;
    s->port.baud_rate = 115200;
    s->port.uart_port = rd->pins.uart_port;
    s->port.uart_rx_pin = rd->pins.rx;
    s->port.uart_tx_pin = rd->pins.tx;
    s->port.reset_pin = rd->pins.reset;
    s->port.boot_pin = rd->pins.boot;
    s->port.rx_buffer_size = 2048;
    s->port.tx_buffer_size = 2048;

    if (esp_loader_init_serial(&s->loader, &s->port.port) != ESP_LOADER_SUCCESS) {
        return ESP_FAIL;
    }
    esp_loader_connect_args_t args = ESP_LOADER_CONNECT_DEFAULT();
    esp_loader_error_t err = esp_loader_connect(&s->loader, &args);
    if (err != ESP_LOADER_SUCCESS) {
        esp_loader_deinit(&s->loader);
        ESP_LOGE(TAG, "%s bootloader not responding (%d): check the JP1 wiring", rd->name, err);
        return ESP_ERR_TIMEOUT;
    }
    /* radio A is the C6 of the board; radio B a C6 or C5 */
    target_chip_t chip = esp_loader_get_target(&s->loader);
    if (chip != ESP32C6_CHIP && (rd == &s_radios[C6_RADIO_A] || chip != ESP32C5_CHIP)) {
        esp_loader_deinit(&s->loader);
        return ESP_ERR_NOT_SUPPORTED;
    }
    c6_radio_set_chip(rd - s_radios, chip == ESP32C5_CHIP ? C6FW_CHIP_C5 : C6FW_CHIP_C6);
    if (esp_loader_change_transmission_rate(&s->loader, FLASH_BAUD) != ESP_LOADER_SUCCESS) {
        ESP_LOGW(TAG, "staying at 115200 Bd");
    }
    chip_info_read(s);
    return ESP_OK;
}

static void session_close(session_t *s)
{
    esp_loader_reset_target(&s->loader);
    esp_loader_deinit(&s->loader);
    c6_radio_init_pins(&s->radio->pins);
}

static esp_err_t write_region(session_t *s, uint32_t addr, uint32_t size, uint32_t (*fill)(void *, uint32_t, uint8_t *, uint32_t),
                              void *ctx, int pct_from, int pct_to)
{
    uint8_t *buf = heap_caps_malloc(FLASH_BLOCK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(buf, ESP_ERR_NO_MEM, TAG, "no mem");
    esp_loader_flash_cfg_t cfg = {.offset = addr, .image_size = size, .block_size = FLASH_BLOCK};
    esp_err_t ret = ESP_OK;

    if (esp_loader_flash_start(&s->loader, &cfg) != ESP_LOADER_SUCCESS) {
        ret = ESP_FAIL;
        goto out;
    }
    for (uint32_t pos = 0; pos < size; pos += FLASH_BLOCK) {
        uint32_t n = size - pos < FLASH_BLOCK ? size - pos : FLASH_BLOCK;
        if (fill(ctx, pos, buf, n) != n || esp_loader_flash_write(&s->loader, &cfg, buf, n) != ESP_LOADER_SUCCESS) {
            ret = ESP_FAIL;
            goto out;
        }
        s->radio->progress.percent = pct_from + (int)((int64_t)(pct_to - pct_from) * (pos + n) / size);
    }
    if (esp_loader_flash_finish(&s->loader, &cfg) != ESP_LOADER_SUCCESS) {
        ret = ESP_ERR_INVALID_CRC;
    }
out:
    free(buf);
    return ret;
}

typedef struct {
    const esp_partition_t *part;
    uint32_t base;
} part_reader_t;

typedef struct {
    const uint8_t *data;
} mem_reader_t;

/* data of a segment of a c6fw bundle (partition or PSRAM package) */
typedef struct {
    const c6fw_entry_t *entry;
    uint32_t base;
} fw_reader_t;

static uint32_t fill_from_fw(void *ctx, uint32_t pos, uint8_t *buf, uint32_t n)
{
    fw_reader_t *r = ctx;
    return fw_read(r->entry, r->base + pos, buf, n) == ESP_OK ? n : 0;
}

static uint32_t fill_from_memory(void *ctx, uint32_t pos, uint8_t *buf, uint32_t n)
{
    memcpy(buf, ((mem_reader_t *)ctx)->data + pos, n);
    return n;
}

/*
 * Bundles are stored zlib-compressed; the P4 inflates a segment into PSRAM, checks its MD5 and
 * writes it uncompressed (the C6 ROM loader's own inflate produced wrong data here).
 */
static esp_err_t inflate_segment(const c6fw_segment_t *seg, const fw_reader_t *r, uint8_t **out)
{
    uint8_t *packed = heap_caps_malloc(seg->stored_size, MALLOC_CAP_SPIRAM);
    uint8_t *plain = heap_caps_malloc(seg->size, MALLOC_CAP_SPIRAM);
    esp_err_t err = packed && plain ? ESP_OK : ESP_ERR_NO_MEM;

    if (err == ESP_OK) {
        err = fw_read(r->entry, r->base, packed, seg->stored_size);
    }
    if (err == ESP_OK) {
        /* The decompressor state is ~11 KB: on the heap, not on the caller's stack. */
        tinfl_decompressor *inflator = heap_caps_malloc(sizeof(tinfl_decompressor), MALLOC_CAP_SPIRAM);
        size_t in_len = seg->stored_size, out_len = seg->size;
        err = ESP_ERR_NO_MEM;
        if (inflator) {
            tinfl_init(inflator);
            tinfl_status st = tinfl_decompress(inflator, packed, &in_len, plain, plain, &out_len,
                                               TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
            err = st == TINFL_STATUS_DONE && out_len == seg->size ? ESP_OK : ESP_ERR_INVALID_SIZE;
            free(inflator);
        }
    }
    if (err == ESP_OK) {
        uint8_t digest[16];
        struct MD5Context md5;
        MD5Init(&md5);
        MD5Update(&md5, plain, seg->size);
        MD5Final(digest, &md5);
        err = memcmp(digest, seg->md5, sizeof(digest)) == 0 ? ESP_OK : ESP_ERR_INVALID_CRC;
    }
    free(packed);
    if (err != ESP_OK) {
        free(plain);
        plain = NULL;
        ESP_LOGE(TAG, "inflate 0x%lx: %s", (unsigned long)seg->addr, esp_err_to_name(err));
    }
    *out = plain;
    return err;
}

/* C6FW_CHIP_* of the staged package: the chip id in the header of its bootloader image
 * (esp_image_header_t.chip_id at byte 12), the bootloader being a WRITE segment below 0x8000. */
static uint32_t pkg_chip(void)
{
    const c6fw_dir_t *dir = (const c6fw_dir_t *)s_pkg;
    uint32_t chip = 0;
    for (uint32_t i = 0; i < dir->count && !chip; i++) {
        bundle_t b = {0};
        if (bundle_load(&dir->entries[i], &b) != ESP_OK) {
            bundle_free(&b);
            continue;
        }
        for (uint32_t k = 0; k < b.count && !chip; k++) {
            const c6fw_segment_t *seg = &b.segs[k];
            if (!(seg->flags & C6FW_SEG_WRITE) || seg->addr >= 0x8000 || seg->size < 16) {
                continue;
            }
            uint8_t head[16] = {0}, *plain = NULL;
            fw_reader_t r = {.entry = &b.entry, .base = seg->data_off};
            if (seg->flags & C6FW_SEG_DEFLATE) {
                if (inflate_segment(seg, &r, &plain) == ESP_OK) {
                    memcpy(head, plain, sizeof(head));
                }
                free(plain);
            } else {
                fw_read(&b.entry, seg->data_off, head, sizeof(head));
            }
            if (head[0] == 0xE9) {
                chip = head[12] | head[13] << 8;
            }
        }
        bundle_free(&b);
    }
    return chip;
}

static uint32_t fill_from_partition(void *ctx, uint32_t pos, uint8_t *buf, uint32_t n)
{
    part_reader_t *r = ctx;
    return esp_partition_read(r->part, r->base + pos, buf, n) == ESP_OK ? n : 0;
}

/* ---- preserved regions ----
 * c6data holds one snapshot slot per bundle with PRESERVE segments (coordinator and router
 * each keep their own Zigbee network). A slot is: header sector, then region data.
 */

#define SNAP_SLOTS 2

static uint32_t slot_size(const esp_partition_t *part)
{
    return part->size / SNAP_SLOTS;
}

static bool snapshot_header(int slot, snap_header_t *hdr)
{
    const esp_partition_t *part = partition(C6DATA_SUBTYPE);
    if (!part || esp_partition_read(part, slot * slot_size(part), hdr, sizeof(*hdr)) != ESP_OK ||
        memcmp(hdr->magic, SNAP_MAGIC, 4) != 0 || hdr->count > SNAP_MAX) {
        return false;
    }
    hdr->bundle[sizeof(hdr->bundle) - 1] = '\0';
    return esp_rom_crc32_le(0, (const uint8_t *)hdr, offsetof(snap_header_t, crc32)) == hdr->crc32;
}

/* Name of a snapshot in c6data: the bundle, prefixed for radio B. */
static void snap_name(const radio_t *rd, const char *bundle, char out[16])
{
    snprintf(out, 16, "%s%s", rd->snap_tag, bundle);
}

static int snapshot_find(const char *bundle, snap_header_t *hdr)
{
    for (int slot = 0; slot < SNAP_SLOTS; slot++) {
        if (snapshot_header(slot, hdr) && strcmp(hdr->bundle, bundle) == 0) {
            return slot;
        }
    }
    return -1;
}

/* Same bundle's slot, else an empty one, else the oldest. */
static int snapshot_pick_slot(const char *bundle, uint32_t *next_generation)
{
    snap_header_t hdr;
    int same = -1, empty = -1, oldest = 0;
    uint32_t oldest_gen = UINT32_MAX, newest_gen = 0;

    for (int slot = 0; slot < SNAP_SLOTS; slot++) {
        if (!snapshot_header(slot, &hdr)) {
            empty = empty < 0 ? slot : empty;
            continue;
        }
        if (strcmp(hdr.bundle, bundle) == 0) {
            same = slot;
        }
        if (hdr.generation < oldest_gen) {
            oldest_gen = hdr.generation;
            oldest = slot;
        }
        newest_gen = hdr.generation > newest_gen ? hdr.generation : newest_gen;
    }
    *next_generation = newest_gen + 1;
    return same >= 0 ? same : empty >= 0 ? empty : oldest;
}

/* Where the C6 really keeps a preserved region: the zb_* data partition of that size in the
 * partition table on the C6 (it moved between layouts), else the bundle's own address. */
static uint32_t c6_region_addr(session_t *s, const c6fw_segment_t *seg)
{
    uint8_t *table = malloc(0xc00);
    uint32_t addr = seg->addr;
    if (table && esp_loader_flash_read(&s->loader, table, 0x8000, 0xc00) == ESP_LOADER_SUCCESS) {
        for (const uint8_t *e = table; e < table + 0xc00 && e[0] == 0xaa && e[1] == 0x50; e += 32) {
            uint32_t off, size;
            memcpy(&off, e + 4, 4);
            memcpy(&size, e + 8, 4);
            if (e[2] == 1 && size == seg->size && strncmp((const char *)e + 12, "zb_", 3) == 0) {
                addr = off;
                break;
            }
        }
    }
    free(table);
    return addr;
}

static esp_err_t snapshot_save(session_t *s, const bundle_t *from)
{
    const esp_partition_t *part = partition(C6DATA_SUBTYPE);
    ESP_RETURN_ON_FALSE(part, ESP_ERR_NOT_FOUND, TAG, "no c6data partition");
    snap_header_t hdr = {0};
    memcpy(hdr.magic, SNAP_MAGIC, 4);
    snap_name(s->radio, from->entry.name, hdr.bundle);
    uint32_t generation;
    int slot = snapshot_pick_slot(hdr.bundle, &generation);
    hdr.generation = generation;
    uint32_t base = slot * slot_size(part);

    uint32_t data_off = SNAP_DATA_OFFSET, total = 0;
    for (uint32_t i = 0; i < from->count && hdr.count < SNAP_MAX; i++) {
        if (from->segs[i].flags & C6FW_SEG_PRESERVE) {
            hdr.entries[hdr.count++] = (snap_entry_t){.addr = c6_region_addr(s, &from->segs[i]),
                                                      .size = from->segs[i].size, .data_off = data_off};
            data_off += (from->segs[i].size + FLASH_BLOCK - 1) / FLASH_BLOCK * FLASH_BLOCK;
            total += from->segs[i].size;
        }
    }
    ESP_RETURN_ON_FALSE(data_off <= slot_size(part), ESP_ERR_INVALID_SIZE, TAG, "snapshot too large");

    uint8_t *buf = heap_caps_malloc(total, MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(buf, ESP_ERR_NO_MEM, TAG, "no mem");
    esp_err_t err = ESP_OK;
    uint8_t *p = buf;
    progress(s->radio, 5, "saving Zigbee network data");
    for (uint32_t i = 0; i < hdr.count && err == ESP_OK; i++) {
        if (esp_loader_flash_read(&s->loader, p, hdr.entries[i].addr, hdr.entries[i].size) != ESP_LOADER_SUCCESS) {
            err = ESP_FAIL;
        }
        hdr.entries[i].crc32 = esp_rom_crc32_le(0, p, hdr.entries[i].size);
        p += hdr.entries[i].size;
    }
    if (err == ESP_OK) {
        /* Data first, header last: a power cut leaves no snapshot rather than a corrupt one. */
        err = esp_partition_erase_range(part, base, slot_size(part));
        p = buf;
        for (uint32_t i = 0; i < hdr.count && err == ESP_OK; i++) {
            err = esp_partition_write(part, base + hdr.entries[i].data_off, p, hdr.entries[i].size);
            p += hdr.entries[i].size;
        }
        hdr.crc32 = esp_rom_crc32_le(0, (const uint8_t *)&hdr, offsetof(snap_header_t, crc32));
        time_t now = time(NULL);
        hdr.saved_unix = now > 1700000000 ? (uint32_t)now : 0;
        if (err == ESP_OK) {
            err = esp_partition_write(part, base, &hdr, sizeof(hdr));
        }
    }
    free(buf);
    ESP_LOGI(TAG, "snapshot of %s in slot %d: %lu B, %s", from->entry.name, slot, (unsigned long)total,
             esp_err_to_name(err));
    return err;
}

static esp_err_t snapshot_restore(session_t *s, const bundle_t *to)
{
    snap_header_t hdr;
    char name[16];
    snap_name(s->radio, to->entry.name, name);
    int slot = snapshot_find(name, &hdr);
    const esp_partition_t *part = partition(C6DATA_SUBTYPE);

    for (uint32_t i = 0; i < to->count; i++) {
        const c6fw_segment_t *seg = &to->segs[i];
        if (!(seg->flags & C6FW_SEG_PRESERVE)) {
            continue;
        }
        /* same place, else the same size: snapshots taken in the per-firmware layouts lie elsewhere */
        const snap_entry_t *match = NULL;
        for (uint32_t j = 0; slot >= 0 && j < hdr.count; j++) {
            if (hdr.entries[j].size == seg->size && (!match || hdr.entries[j].addr == seg->addr)) {
                match = &hdr.entries[j];
            }
        }
        if (match) {
            progress(s->radio, 90, "restoring Zigbee network data");
            part_reader_t r = {.part = part, .base = slot * slot_size(part) + match->data_off};
            ESP_RETURN_ON_ERROR(write_region(s, seg->addr, seg->size, fill_from_partition, &r, 90, 95), TAG,
                                "restore 0x%lx", (unsigned long)seg->addr);
            ESP_LOGI(TAG, "restored 0x%lx (%lu B)", (unsigned long)seg->addr, (unsigned long)seg->size);
        } else {
            /* Leftovers of another firmware must not be parsed as network data. */
            uint32_t size = (seg->size + FLASH_BLOCK - 1) / FLASH_BLOCK * FLASH_BLOCK;
            if (esp_loader_flash_erase_region(&s->loader, seg->addr, size) != ESP_LOADER_SUCCESS) {
                return ESP_FAIL;
            }
            ESP_LOGW(TAG, "no snapshot for 0x%lx, region erased (new network)", (unsigned long)seg->addr);
        }
    }
    return ESP_OK;
}

esp_err_t c6_snapshot_take(c6_radio_t r, const char *bundle)
{
    radio_t *rd = radio(r);
    c6fw_entry_t entry;
    bundle_t b = {0};
    session_t *s = calloc(1, sizeof(session_t));
    ESP_RETURN_ON_FALSE(s, ESP_ERR_NO_MEM, TAG, "no mem");
    esp_err_t err = c6fw_find(r, bundle, &entry);
    if (err == ESP_OK) {
        err = bundle_load(&entry, &b);
    }
    if (err == ESP_OK && !bundle_has_preserve(&b)) {
        err = ESP_ERR_NOT_SUPPORTED;
    }
    if (err == ESP_OK) {
        rd->progress.busy = true;
        rd->progress.started_us = esp_timer_get_time();
        rd->progress.last_error[0] = '\0';
        progress(rd, 2, "connecting to C6 bootloader");
        err = session_open(s, rd);
        if (err == ESP_OK) {
            err = snapshot_save(s, &b);
            session_close(s);
        }
        if (err != ESP_OK) {
            snprintf(rd->progress.last_error, sizeof(rd->progress.last_error), "Zigbee backup failed: %s", esp_err_to_name(err));
        }
        progress(rd, err == ESP_OK ? 100 : rd->progress.percent, err == ESP_OK ? "done" : "failed");
        rd->progress.busy = false;
    }
    bundle_free(&b);
    free(s);
    return err;
}

esp_err_t c6_snapshot_info(c6_radio_t r, const char *bundle, uint32_t *total_size, uint32_t *generation, uint32_t *saved_unix)
{
    snap_header_t hdr;
    char name[16];
    snap_name(radio(r), bundle, name);
    if (snapshot_find(name, &hdr) < 0) {
        return ESP_ERR_NOT_FOUND;
    }
    *total_size = 0;
    for (uint32_t i = 0; i < hdr.count; i++) {
        *total_size += hdr.entries[i].size;
    }
    *generation = hdr.generation;
    if (saved_unix) {
        *saved_unix = hdr.saved_unix == UINT32_MAX ? 0 : hdr.saved_unix;
    }
    return ESP_OK;
}

esp_err_t c6_snapshot_read(c6_radio_t r, const char *bundle, uint32_t offset, void *buf, size_t len)
{
    snap_header_t hdr;
    char name[16];
    snap_name(radio(r), bundle, name);
    int slot = snapshot_find(name, &hdr);
    ESP_RETURN_ON_FALSE(slot >= 0, ESP_ERR_NOT_FOUND, TAG, "no snapshot");
    const esp_partition_t *part = partition(C6DATA_SUBTYPE);
    uint32_t base = slot * slot_size(part);
    uint8_t *out = buf;
    /* The snapshot is exposed as the concatenation of its regions. */
    for (uint32_t i = 0; i < hdr.count && len; i++) {
        if (offset >= hdr.entries[i].size) {
            offset -= hdr.entries[i].size;
            continue;
        }
        size_t n = hdr.entries[i].size - offset < len ? hdr.entries[i].size - offset : len;
        ESP_RETURN_ON_ERROR(esp_partition_read(part, base + hdr.entries[i].data_off + offset, out, n), TAG, "read");
        out += n;
        len -= n;
        offset = 0;
    }
    return len == 0 ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

/* ---- flashing ---- */

/* Writes and erases the segments of a bundle; done/total count written bytes for the progress. */
static esp_err_t write_bundle(session_t *s, const bundle_t *b, uint32_t *done, uint32_t total)
{
    esp_err_t err = ESP_OK;
    for (uint32_t i = 0; i < b->count && err == ESP_OK; i++) {
        const c6fw_segment_t *seg = &b->segs[i];
        if (seg->flags & C6FW_SEG_ERASE) {
            progress(s->radio, 8, "erasing settings area");
            if (esp_loader_flash_erase_region(&s->loader, seg->addr, seg->size) != ESP_LOADER_SUCCESS) {
                err = ESP_FAIL;
            }
        } else if (seg->flags & C6FW_SEG_WRITE) {
            char stage[48];
            snprintf(stage, sizeof(stage), "writing 0x%05lx (%lu kB)", (unsigned long)seg->addr,
                     (unsigned long)seg->size / 1024);
            progress(s->radio, s->radio->progress.percent, stage);
            fw_reader_t r = {.entry = &b->entry, .base = seg->data_off};
            int from_pct = 10 + (int)(80LL * *done / total);
            int to_pct = 10 + (int)(80LL * (*done + seg->size) / total);
            uint8_t *plain = NULL;
            mem_reader_t mem = {0};
            if (seg->flags & C6FW_SEG_DEFLATE) {
                err = inflate_segment(seg, &r, &plain);
                mem.data = plain;
            }
            for (int attempt = 0; attempt < 3 && err == ESP_OK; attempt++) {
                err = plain ? write_region(s, seg->addr, seg->size, fill_from_memory, &mem, from_pct, to_pct)
                            : write_region(s, seg->addr, seg->size, fill_from_fw, &r, from_pct, to_pct);
                if (err == ESP_OK) {
                    break;
                }
                ESP_LOGW(TAG, "write 0x%lx failed (%s), retry", (unsigned long)seg->addr, esp_err_to_name(err));
                err = attempt < 2 ? ESP_OK : err;
            }
            free(plain);
            *done += seg->size;
        }
    }
    return err;
}

static uint32_t bundle_bytes(const bundle_t *b)
{
    uint32_t total = 0;
    for (uint32_t i = 0; b->segs && i < b->count; i++) {
        if (b->segs[i].flags & C6FW_SEG_WRITE) {
            total += b->segs[i].size;
        }
    }
    return total;
}

/* sha256 of the slots bundle installed on the C6, kept with the chip data */
static bool slots_installed(const radio_t *rd, const c6fw_entry_t *slots)
{
    nvs_handle_t h;
    uint8_t sha[32];
    size_t len = sizeof(sha);
    bool same = false;
    if (nvs_open(rd->nvs_ns, NVS_READONLY, &h) == ESP_OK) {
        same = nvs_get_blob(h, "slots", sha, &len) == ESP_OK && len == sizeof(sha) && memcmp(sha, slots->sha256, len) == 0;
        nvs_close(h);
    }
    return same;
}

static void slots_record(const radio_t *rd, const c6fw_entry_t *slots)
{
    nvs_handle_t h;
    if (nvs_open(rd->nvs_ns, NVS_READWRITE, &h) == ESP_OK) {
        if (slots) {
            nvs_set_blob(h, "slots", slots->sha256, sizeof(slots->sha256));
        } else {
            nvs_erase_key(h, "slots");
        }
        nvs_commit(h);
        nvs_close(h);
    }
}

void c6_slots_invalidate(c6_radio_t r)
{
    slots_record(radio(r), NULL);
}


bool c6_flash_ready(c6_radio_t r, const c6fw_entry_t *entry)
{
    bundle_t b = {0};
    if (!c6fw_available(entry) || bundle_load(entry, &b) != ESP_OK) {
        bundle_free(&b);
        return false;
    }
    const c6fw_segment_t *marker = bundle_find_flag(&b, C6FW_SEG_SLOTS);
    c6fw_entry_t slots;
    bool ready = !marker || (c6fw_find(r, C6FW_SLOTS_BUNDLE, &slots) == ESP_OK && (slots_installed(radio(r), &slots) || c6fw_available(&slots)));
    bundle_free(&b);
    return ready;
}

esp_err_t c6_flash_bundle(c6_radio_t r, const c6fw_entry_t *entry, const char *current_name)
{
    radio_t *rd = radio(r);
    bundle_t to = {0}, from = {0}, base = {0};
    session_t *s = calloc(1, sizeof(session_t));
    esp_err_t err;

    ESP_RETURN_ON_FALSE(s, ESP_ERR_NO_MEM, TAG, "no mem");
    rd->progress.busy = true;
    rd->progress.started_us = esp_timer_get_time();
    rd->progress.last_error[0] = '\0';
    progress(rd, 0, "verifying image");

    err = c6fw_verify(entry);
    if (err != ESP_OK) {
        snprintf(rd->progress.last_error, sizeof(rd->progress.last_error), "bundle %s fails sha256 check", entry->name);
        goto done;
    }
    if ((err = bundle_load(entry, &to)) != ESP_OK) {
        goto done;
    }
    c6fw_entry_t current;
    if (current_name && current_name[0] && c6fw_find(r, current_name, &current) == ESP_OK) {
        bundle_load(&current, &from);
    }

    progress(rd, 2, "connecting to the bootloader");
    if ((err = session_open(s, rd)) != ESP_OK) {
        snprintf(rd->progress.last_error, sizeof(rd->progress.last_error),
                 "%s bootloader not responding, check the JP1 wiring", rd->name);
        goto done;
    }

    if (from.segs && bundle_has_preserve(&from)) {
        err = snapshot_save(s, &from);
        if (err != ESP_OK) {
            snprintf(rd->progress.last_error, sizeof(rd->progress.last_error), "saving Zigbee data failed");
            goto close;
        }
    }

    /* A slot bundle needs the layout of the slots bundle it was packed with: install it first. */
    const c6fw_segment_t *marker = bundle_find_flag(&to, C6FW_SEG_SLOTS);
    c6fw_entry_t slots_entry;
    if (marker && (c6fw_find(r, C6FW_SLOTS_BUNDLE, &slots_entry) != ESP_OK ||
                   memcmp(slots_entry.sha256, marker->md5, sizeof(marker->md5)) != 0)) {
        snprintf(rd->progress.last_error, sizeof(rd->progress.last_error), "bundle %s does not match the slots bundle", entry->name);
        err = ESP_ERR_INVALID_STATE;
        goto close;
    }
    if (marker && !slots_installed(rd, &slots_entry)) {
        if ((err = c6fw_verify(&slots_entry)) != ESP_OK || (err = bundle_load(&slots_entry, &base)) != ESP_OK) {
            snprintf(rd->progress.last_error, sizeof(rd->progress.last_error), "slots bundle unusable");
            goto close;
        }
        ESP_LOGW(TAG, "installing the slots layout %s", slots_entry.version);
    }
    uint32_t done_bytes = 0, total = bundle_bytes(&base) + bundle_bytes(&to);
    if (base.segs) {
        slots_record(rd, NULL); /* a partly written layout is not the installed one */
        err = write_bundle(s, &base, &done_bytes, total);
        if (err == ESP_OK) {
            slots_record(rd, &slots_entry);
        }
    }
    if (err == ESP_OK) {
        err = write_bundle(s, &to, &done_bytes, total);
    }
    if (err == ESP_OK && !marker) {
        slots_record(rd, NULL); /* a whole-flash bundle replaced the layout */
    }
    if (err == ESP_OK && bundle_has_preserve(&to)) {
        err = snapshot_restore(s, &to);
    }
    if (err != ESP_OK && !rd->progress.last_error[0]) {
        snprintf(rd->progress.last_error, sizeof(rd->progress.last_error), "flashing %s failed: %s", entry->name,
                 esp_err_to_name(err));
    }
close:
    session_close(s);
done:
    bundle_free(&to);
    bundle_free(&from);
    bundle_free(&base);
    free(s);
    uint32_t off;
    if (err == ESP_OK && image_find(s_pkg, s_pkg_len, entry, &off)) {
        /* flashed from the package: the radio's local image and package id follow it, the package
         * is not needed any more (role switches use the local image) */
        uint32_t len;
        uint8_t *img = local_image(s_pkg, read_memory, (void *)s_pkg, &len);
        if (img) {
            local_store(rd, img, len);
        }
        uint8_t sha[32];
        mbedtls_sha256(s_pkg, s_pkg_len, sha, 0);
        nvs_handle_t h;
        if (nvs_open(rd->nvs_ns, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_blob(h, "pkg", sha, sizeof(sha));
            nvs_commit(h);
            nvs_close(h);
        }
        c6fw_stage_drop();
        ESP_LOGI(TAG, "%s: package flashed, dropped from PSRAM", rd->name);
    }
    if (err == ESP_OK) {
        /* before busy clears: callers (and tests) treat busy == false as "flashing finished" */
        c6_chip_info(r);
        rd->chip.flashes++;
        chip_info_save(rd);
    }
    progress(rd, err == ESP_OK ? 100 : rd->progress.percent, err == ESP_OK ? "done" : "failed");
    rd->progress.busy = false;
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s now runs %s (%s)", rd->name, entry->name, entry->version);
    } else {
        ESP_LOGE(TAG, "%s", rd->progress.last_error);
    }
    return err;
}

uint32_t c6_flash_size(c6_radio_t r)
{
    return radio(r)->flash_size;
}

esp_err_t c6_read_flash(c6_radio_t r, c6_sink_t sink, void *ctx)
{
    radio_t *rd = radio(r);
    session_t *s = calloc(1, sizeof(session_t));
    uint8_t *buf = malloc(FLASH_BLOCK);
    esp_err_t err;

    if (!s || !buf) {
        free(s);
        free(buf);
        return ESP_ERR_NO_MEM;
    }
    rd->progress.busy = true;
    rd->progress.started_us = esp_timer_get_time();
    rd->progress.last_error[0] = '\0';
    progress(rd, 0, "reading the flash");
    err = session_open(s, rd);
    if (err != ESP_OK) {
        snprintf(rd->progress.last_error, sizeof(rd->progress.last_error),
                 "%s bootloader not responding, check the JP1 wiring", rd->name);
    }
    if (err == ESP_OK) {
        if (esp_loader_flash_detect_size(&s->loader, &rd->flash_size) != ESP_LOADER_SUCCESS) {
            rd->flash_size = 4 * 1024 * 1024;
        }
        ESP_LOGI(TAG, "%s flash %lu kB", rd->name, (unsigned long)rd->flash_size / 1024);
        for (uint32_t addr = 0; addr < rd->flash_size && err == ESP_OK; addr += FLASH_BLOCK) {
            if (esp_loader_flash_read(&s->loader, buf, addr, FLASH_BLOCK) != ESP_LOADER_SUCCESS) {
                snprintf(rd->progress.last_error, sizeof(rd->progress.last_error), "flash read failed at 0x%lx",
                         (unsigned long)addr);
                err = ESP_FAIL;
                break;
            }
            err = sink(ctx, buf, FLASH_BLOCK);
            rd->progress.percent = (int)((uint64_t)(addr + FLASH_BLOCK) * 100 / rd->flash_size);
        }
        session_close(s);
    }
    free(buf);
    free(s);
    progress(rd, err == ESP_OK ? 100 : rd->progress.percent, err == ESP_OK ? "backup done" : "backup failed");
    rd->progress.busy = false;
    return err;
}
