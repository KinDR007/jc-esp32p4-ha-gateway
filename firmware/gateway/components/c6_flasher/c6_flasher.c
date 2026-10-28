#include "c6_flasher.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp32_port.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_loader.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "sdkconfig.h"

static const char *TAG = "c6_flasher";

#define C6FW_SUBTYPE 0x40
#define C6DATA_SUBTYPE 0x41
#define FLASH_BLOCK 4096
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
} snap_header_t;

static c6_progress_t s_progress;

static void progress(int percent, const char *stage)
{
    s_progress.percent = percent;
    strlcpy(s_progress.stage, stage, sizeof(s_progress.stage));
}

const c6_progress_t *c6_progress(void)
{
    return &s_progress;
}

void c6_init_pins(void)
{
    gpio_config_t reset = {
        .pin_bit_mask = 1ULL << CONFIG_GW_C6_RESET_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_set_level(CONFIG_GW_C6_RESET_GPIO, 1);
    gpio_config(&reset);
    gpio_set_level(CONFIG_GW_C6_RESET_GPIO, 1);
    /* BOOT has a 5k1 pull-up on the C6 side; leave it released. */
    gpio_reset_pin(CONFIG_GW_C6_BOOT_GPIO);
    gpio_set_direction(CONFIG_GW_C6_BOOT_GPIO, GPIO_MODE_INPUT);
}

void c6_hold_reset(bool hold)
{
    gpio_set_level(CONFIG_GW_C6_RESET_GPIO, hold ? 0 : 1);
}

void c6_reset(void)
{
    c6_hold_reset(true);
    vTaskDelay(pdMS_TO_TICKS(20));
    c6_hold_reset(false);
}

static const esp_partition_t *partition(uint8_t subtype)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, subtype, NULL);
}

esp_err_t c6fw_read_dir(c6fw_dir_t *dir)
{
    const esp_partition_t *part = partition(C6FW_SUBTYPE);
    ESP_RETURN_ON_FALSE(part, ESP_ERR_NOT_FOUND, TAG, "no c6fw partition");
    ESP_RETURN_ON_ERROR(esp_partition_read(part, 0, dir, sizeof(*dir)), TAG, "read dir");
    if (memcmp(dir->magic, C6FW_DIR_MAGIC, 4) != 0 || dir->format != C6FW_FORMAT_VERSION ||
        dir->count > C6FW_MAX_ENTRIES) {
        return ESP_ERR_INVALID_VERSION;
    }
    uint32_t crc = esp_rom_crc32_le(0, (const uint8_t *)dir, offsetof(c6fw_dir_t, crc32));
    if (crc != dir->crc32) {
        return ESP_ERR_INVALID_CRC;
    }
    for (uint32_t i = 0; i < dir->count; i++) {
        c6fw_entry_t *e = &dir->entries[i];
        e->name[sizeof(e->name) - 1] = '\0';
        e->version[sizeof(e->version) - 1] = '\0';
        if (e->offset + e->size > part->size) {
            return ESP_ERR_INVALID_SIZE;
        }
    }
    return ESP_OK;
}

esp_err_t c6fw_find(const char *name, c6fw_entry_t *entry)
{
    c6fw_dir_t dir;
    ESP_RETURN_ON_ERROR(c6fw_read_dir(&dir), TAG, "no valid c6fw image");
    for (uint32_t i = 0; i < dir.count; i++) {
        if (strcmp(dir.entries[i].name, name) == 0) {
            *entry = dir.entries[i];
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t c6fw_verify(const c6fw_entry_t *entry)
{
    const esp_partition_t *part = partition(C6FW_SUBTYPE);
    uint8_t *buf = malloc(FLASH_BLOCK);
    uint8_t digest[32];
    esp_err_t err = ESP_OK;
    mbedtls_sha256_context ctx;

    ESP_RETURN_ON_FALSE(buf, ESP_ERR_NO_MEM, TAG, "no mem");
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    for (uint32_t pos = 0; pos < entry->size && err == ESP_OK; pos += FLASH_BLOCK) {
        size_t n = entry->size - pos < FLASH_BLOCK ? entry->size - pos : FLASH_BLOCK;
        err = esp_partition_read(part, entry->offset + pos, buf, n);
        mbedtls_sha256_update(&ctx, buf, n);
    }
    mbedtls_sha256_finish(&ctx, digest);
    mbedtls_sha256_free(&ctx);
    free(buf);
    if (err == ESP_OK && memcmp(digest, entry->sha256, sizeof(digest)) != 0) {
        err = ESP_ERR_INVALID_CRC;
    }
    return err;
}

esp_err_t c6fw_verify_all(void)
{
    c6fw_dir_t dir;
    ESP_RETURN_ON_ERROR(c6fw_read_dir(&dir), TAG, "directory");
    for (uint32_t i = 0; i < dir.count; i++) {
        ESP_RETURN_ON_ERROR(c6fw_verify(&dir.entries[i]), TAG, "bundle %s", dir.entries[i].name);
    }
    return ESP_OK;
}

/* ---- bundle access ---- */

typedef struct {
    c6fw_entry_t entry;
    uint32_t count;
    c6fw_segment_t *segs;
} bundle_t;

static esp_err_t bundle_load(const c6fw_entry_t *entry, bundle_t *b)
{
    const esp_partition_t *part = partition(C6FW_SUBTYPE);
    char magic[4];

    b->entry = *entry;
    b->segs = NULL;
    ESP_RETURN_ON_ERROR(esp_partition_read(part, entry->offset, magic, 4), TAG, "read");
    ESP_RETURN_ON_ERROR(esp_partition_read(part, entry->offset + 4, &b->count, 4), TAG, "read");
    ESP_RETURN_ON_FALSE(memcmp(magic, C6FW_BUNDLE_MAGIC, 4) == 0 && b->count > 0 && b->count < 32,
                        ESP_ERR_INVALID_VERSION, TAG, "bad bundle header");
    b->segs = calloc(b->count, sizeof(c6fw_segment_t));
    ESP_RETURN_ON_FALSE(b->segs, ESP_ERR_NO_MEM, TAG, "no mem");
    return esp_partition_read(part, entry->offset + 8, b->segs, b->count * sizeof(c6fw_segment_t));
}

static void bundle_free(bundle_t *b)
{
    free(b->segs);
    b->segs = NULL;
}

static bool bundle_has_preserve(const bundle_t *b)
{
    for (uint32_t i = 0; i < b->count; i++) {
        if (b->segs[i].flags & C6FW_SEG_PRESERVE) {
            return true;
        }
    }
    return false;
}

/* ---- ROM loader session ---- */

typedef struct {
    esp_loader_t loader;
    esp32_port_t port;
} session_t;

static esp_err_t session_open(session_t *s)
{
    memset(s, 0, sizeof(*s));
    s->port.port.ops = &esp32_uart_ops;
    s->port.baud_rate = 115200;
    s->port.uart_port = CONFIG_GW_C6_UART_PORT;
    s->port.uart_rx_pin = CONFIG_GW_C6_UART_RX;
    s->port.uart_tx_pin = CONFIG_GW_C6_UART_TX;
    s->port.reset_pin = CONFIG_GW_C6_RESET_GPIO;
    s->port.boot_pin = CONFIG_GW_C6_BOOT_GPIO;
    s->port.rx_buffer_size = 2048;
    s->port.tx_buffer_size = 2048;

    if (esp_loader_init_serial(&s->loader, &s->port.port) != ESP_LOADER_SUCCESS) {
        return ESP_FAIL;
    }
    esp_loader_connect_args_t args = ESP_LOADER_CONNECT_DEFAULT();
    esp_loader_error_t err = esp_loader_connect(&s->loader, &args);
    if (err != ESP_LOADER_SUCCESS) {
        esp_loader_deinit(&s->loader);
        ESP_LOGE(TAG, "C6 bootloader not responding (%d): check JP1 jumpers 13-20, 15-22, 17-24", err);
        return ESP_ERR_TIMEOUT;
    }
    if (esp_loader_get_target(&s->loader) != ESP32C6_CHIP) {
        esp_loader_deinit(&s->loader);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (esp_loader_change_transmission_rate(&s->loader, FLASH_BAUD) != ESP_LOADER_SUCCESS) {
        ESP_LOGW(TAG, "staying at 115200 Bd");
    }
    return ESP_OK;
}

static void session_close(session_t *s)
{
    esp_loader_reset_target(&s->loader);
    esp_loader_deinit(&s->loader);
    c6_init_pins();
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
        s_progress.percent = pct_from + (int)((int64_t)(pct_to - pct_from) * (pos + n) / size);
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

static esp_err_t snapshot_save(session_t *s, const bundle_t *from)
{
    const esp_partition_t *part = partition(C6DATA_SUBTYPE);
    ESP_RETURN_ON_FALSE(part, ESP_ERR_NOT_FOUND, TAG, "no c6data partition");
    snap_header_t hdr = {0};
    memcpy(hdr.magic, SNAP_MAGIC, 4);
    strlcpy(hdr.bundle, from->entry.name, sizeof(hdr.bundle));
    uint32_t generation;
    int slot = snapshot_pick_slot(hdr.bundle, &generation);
    hdr.generation = generation;
    uint32_t base = slot * slot_size(part);

    uint32_t data_off = SNAP_DATA_OFFSET, total = 0;
    for (uint32_t i = 0; i < from->count && hdr.count < SNAP_MAX; i++) {
        if (from->segs[i].flags & C6FW_SEG_PRESERVE) {
            hdr.entries[hdr.count++] = (snap_entry_t){.addr = from->segs[i].addr, .size = from->segs[i].size,
                                                      .data_off = data_off};
            data_off += (from->segs[i].size + FLASH_BLOCK - 1) / FLASH_BLOCK * FLASH_BLOCK;
            total += from->segs[i].size;
        }
    }
    ESP_RETURN_ON_FALSE(data_off <= slot_size(part), ESP_ERR_INVALID_SIZE, TAG, "snapshot too large");

    uint8_t *buf = heap_caps_malloc(total, MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(buf, ESP_ERR_NO_MEM, TAG, "no mem");
    esp_err_t err = ESP_OK;
    uint8_t *p = buf;
    progress(5, "saving Zigbee network data");
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
    int slot = snapshot_find(to->entry.name, &hdr);
    const esp_partition_t *part = partition(C6DATA_SUBTYPE);

    for (uint32_t i = 0; i < to->count; i++) {
        const c6fw_segment_t *seg = &to->segs[i];
        if (!(seg->flags & C6FW_SEG_PRESERVE)) {
            continue;
        }
        const snap_entry_t *match = NULL;
        for (uint32_t j = 0; slot >= 0 && j < hdr.count; j++) {
            if (hdr.entries[j].addr == seg->addr && hdr.entries[j].size == seg->size) {
                match = &hdr.entries[j];
            }
        }
        if (match) {
            progress(90, "restoring Zigbee network data");
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

esp_err_t c6_snapshot_info(const char *bundle, uint32_t *total_size, uint32_t *generation)
{
    snap_header_t hdr;
    if (snapshot_find(bundle, &hdr) < 0) {
        return ESP_ERR_NOT_FOUND;
    }
    *total_size = 0;
    for (uint32_t i = 0; i < hdr.count; i++) {
        *total_size += hdr.entries[i].size;
    }
    *generation = hdr.generation;
    return ESP_OK;
}

esp_err_t c6_snapshot_read(const char *bundle, uint32_t offset, void *buf, size_t len)
{
    snap_header_t hdr;
    int slot = snapshot_find(bundle, &hdr);
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

esp_err_t c6_flash_bundle(const c6fw_entry_t *entry, const char *current_name)
{
    bundle_t to = {0}, from = {0};
    session_t *s = calloc(1, sizeof(session_t));
    esp_err_t err;

    ESP_RETURN_ON_FALSE(s, ESP_ERR_NO_MEM, TAG, "no mem");
    s_progress.busy = true;
    s_progress.last_error[0] = '\0';
    progress(0, "verifying image");

    err = c6fw_verify(entry);
    if (err != ESP_OK) {
        snprintf(s_progress.last_error, sizeof(s_progress.last_error), "bundle %s fails sha256 check", entry->name);
        goto done;
    }
    if ((err = bundle_load(entry, &to)) != ESP_OK) {
        goto done;
    }
    c6fw_entry_t current;
    if (current_name && current_name[0] && c6fw_find(current_name, &current) == ESP_OK) {
        bundle_load(&current, &from);
    }

    progress(2, "connecting to C6 bootloader");
    if ((err = session_open(s)) != ESP_OK) {
        snprintf(s_progress.last_error, sizeof(s_progress.last_error),
                 "C6 bootloader not responding, check JP1 jumpers");
        goto done;
    }

    if (from.segs && bundle_has_preserve(&from)) {
        err = snapshot_save(s, &from);
        if (err != ESP_OK) {
            snprintf(s_progress.last_error, sizeof(s_progress.last_error), "saving Zigbee data failed");
            goto close;
        }
    }

    uint32_t total = 0, done_bytes = 0;
    for (uint32_t i = 0; i < to.count; i++) {
        if (to.segs[i].flags & C6FW_SEG_WRITE) {
            total += to.segs[i].size;
        }
    }
    const esp_partition_t *part = partition(C6FW_SUBTYPE);
    for (uint32_t i = 0; i < to.count && err == ESP_OK; i++) {
        const c6fw_segment_t *seg = &to.segs[i];
        if (seg->flags & C6FW_SEG_ERASE) {
            progress(8, "erasing settings area");
            if (esp_loader_flash_erase_region(&s->loader, seg->addr, seg->size) != ESP_LOADER_SUCCESS) {
                err = ESP_FAIL;
            }
        } else if (seg->flags & C6FW_SEG_WRITE) {
            char stage[48];
            snprintf(stage, sizeof(stage), "writing 0x%05lx (%lu kB)", (unsigned long)seg->addr,
                     (unsigned long)seg->size / 1024);
            progress(s_progress.percent, stage);
            part_reader_t r = {.part = part, .base = entry->offset + seg->data_off};
            int from_pct = 10 + (int)(80LL * done_bytes / total);
            int to_pct = 10 + (int)(80LL * (done_bytes + seg->size) / total);
            for (int attempt = 0; attempt < 3; attempt++) {
                err = write_region(s, seg->addr, seg->size, fill_from_partition, &r, from_pct, to_pct);
                if (err == ESP_OK) {
                    break;
                }
                ESP_LOGW(TAG, "write 0x%lx failed (%s), retry", (unsigned long)seg->addr, esp_err_to_name(err));
            }
            done_bytes += seg->size;
        }
    }
    if (err == ESP_OK && bundle_has_preserve(&to)) {
        err = snapshot_restore(s, &to);
    }
    if (err != ESP_OK && !s_progress.last_error[0]) {
        snprintf(s_progress.last_error, sizeof(s_progress.last_error), "flashing %s failed: %s", entry->name,
                 esp_err_to_name(err));
    }
close:
    session_close(s);
done:
    bundle_free(&to);
    bundle_free(&from);
    free(s);
    progress(err == ESP_OK ? 100 : s_progress.percent, err == ESP_OK ? "done" : "failed");
    s_progress.busy = false;
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "C6 now runs %s (%s)", entry->name, entry->version);
    } else {
        ESP_LOGE(TAG, "%s", s_progress.last_error);
    }
    return err;
}

static uint32_t s_flash_size;

uint32_t c6_flash_size(void)
{
    return s_flash_size;
}

esp_err_t c6_read_flash(c6_sink_t sink, void *ctx)
{
    session_t *s = calloc(1, sizeof(session_t));
    uint8_t *buf = malloc(FLASH_BLOCK);
    esp_err_t err;

    if (!s || !buf) {
        free(s);
        free(buf);
        return ESP_ERR_NO_MEM;
    }
    s_progress.busy = true;
    s_progress.last_error[0] = '\0';
    progress(0, "reading C6 flash");
    err = session_open(s);
    if (err != ESP_OK) {
        snprintf(s_progress.last_error, sizeof(s_progress.last_error),
                 "C6 bootloader not responding, check JP1 jumpers");
    }
    if (err == ESP_OK) {
        if (esp_loader_flash_detect_size(&s->loader, &s_flash_size) != ESP_LOADER_SUCCESS) {
            s_flash_size = 4 * 1024 * 1024;
        }
        ESP_LOGI(TAG, "C6 flash %lu kB", (unsigned long)s_flash_size / 1024);
        for (uint32_t addr = 0; addr < s_flash_size && err == ESP_OK; addr += FLASH_BLOCK) {
            if (esp_loader_flash_read(&s->loader, buf, addr, FLASH_BLOCK) != ESP_LOADER_SUCCESS) {
                snprintf(s_progress.last_error, sizeof(s_progress.last_error), "C6 flash read failed at 0x%lx",
                         (unsigned long)addr);
                err = ESP_FAIL;
                break;
            }
            err = sink(ctx, buf, FLASH_BLOCK);
            s_progress.percent = (int)((uint64_t)(addr + FLASH_BLOCK) * 100 / s_flash_size);
        }
        session_close(s);
    }
    free(buf);
    free(s);
    progress(err == ESP_OK ? 100 : s_progress.percent, err == ESP_OK ? "backup done" : "backup failed");
    s_progress.busy = false;
    return err;
}
