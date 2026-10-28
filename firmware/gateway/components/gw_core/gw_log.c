#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "gw.h"

/* Log lines are teed into a ring buffer in PSRAM so the web UI can show them. */

static char *s_ring;
static size_t s_size;
static uint32_t s_head; /* total bytes ever written */
static SemaphoreHandle_t s_lock;
static vprintf_like_t s_orig;
static char s_line[384];

static void ring_write(const char *data, size_t len)
{
    if (len > s_size) {
        data += len - s_size;
        len = s_size;
    }
    size_t pos = s_head % s_size;
    size_t first = len < s_size - pos ? len : s_size - pos;
    memcpy(s_ring + pos, data, first);
    memcpy(s_ring, data + first, len - first);
    s_head += len;
}

static int log_vprintf(const char *fmt, va_list args)
{
    if (xPortInIsrContext() || xTaskGetSchedulerState() != taskSCHEDULER_RUNNING ||
        xSemaphoreTake(s_lock, pdMS_TO_TICKS(5)) != pdTRUE) {
        return s_orig(fmt, args);
    }
    va_list copy;
    va_copy(copy, args);
    int len = vsnprintf(s_line, sizeof(s_line), fmt, copy);
    va_end(copy);
    if (len > 0) {
        ring_write(s_line, (size_t)len < sizeof(s_line) ? (size_t)len : sizeof(s_line) - 1);
    }
    xSemaphoreGive(s_lock);
    return s_orig(fmt, args);
}

esp_err_t gw_log_init(size_t size)
{
    s_ring = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!s_ring) {
        return ESP_ERR_NO_MEM;
    }
    s_size = size;
    s_lock = xSemaphoreCreateMutex();
    s_orig = esp_log_set_vprintf(log_vprintf);
    return ESP_OK;
}

size_t gw_log_read(uint32_t *seq, char *out, size_t len)
{
    if (!s_ring || xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return 0;
    }
    uint32_t head = s_head;
    uint32_t oldest = head > s_size ? head - s_size : 0;
    uint32_t from = *seq;
    if (from < oldest || from > head) {
        from = oldest;
    }
    size_t count = head - from;
    if (count > len) {
        count = len;
    }
    for (size_t i = 0; i < count; i++) {
        out[i] = s_ring[(from + i) % s_size];
    }
    xSemaphoreGive(s_lock);
    *seq = from + count;
    return count;
}
