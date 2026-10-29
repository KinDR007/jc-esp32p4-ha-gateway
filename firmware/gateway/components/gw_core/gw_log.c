#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "gw.h"

/* Log lines are teed into a ring buffer in PSRAM so the web UI can show them. Every line starts with
 * the time it was logged: unix milliseconds (13 digits) once the clock is set, else "+<ms since boot>",
 * then a space. OpenThread and continuation lines have no usable time of their own. */

/* A second, smaller copy lives in internal RAM that startup leaves alone, so the log before a
 * software, watchdog or panic restart can be read after it (internal RAM, unlike PSRAM, has no
 * write-back cache that a panic could lose). */
#define KEPT_SIZE (16 * 1024)
#define KEPT_MAGIC 0x50344C47u
typedef struct {
    uint32_t magic;
    uint32_t head; /* total bytes ever written */
    uint32_t check;
    char data[KEPT_SIZE];
} kept_log_t;
static __NOINIT_ATTR kept_log_t s_kept;
static char *s_prev;
static size_t s_prev_len;

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

static void kept_write(const char *data, size_t len)
{
    if (len > KEPT_SIZE) {
        data += len - KEPT_SIZE;
        len = KEPT_SIZE;
    }
    size_t pos = s_kept.head % KEPT_SIZE;
    size_t first = len < KEPT_SIZE - pos ? len : KEPT_SIZE - pos;
    memcpy(s_kept.data + pos, data, first);
    memcpy(s_kept.data, data + first, len - first);
    s_kept.head += len;
    s_kept.check = s_kept.head ^ KEPT_MAGIC;
}

/* Takes over what the previous run left in s_kept, then starts it over for this run. */
static void kept_init(void)
{
    esp_reset_reason_t reason = esp_reset_reason();
    if (reason != ESP_RST_POWERON && reason != ESP_RST_BROWNOUT && s_kept.magic == KEPT_MAGIC &&
        s_kept.check == (s_kept.head ^ KEPT_MAGIC) && s_kept.head > 0) {
        size_t len = s_kept.head < KEPT_SIZE ? s_kept.head : KEPT_SIZE;
        s_prev = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
        if (s_prev) {
            size_t start = s_kept.head - len;
            for (size_t i = 0; i < len; i++) {
                s_prev[i] = s_kept.data[(start + i) % KEPT_SIZE];
            }
            s_prev_len = len;
        }
    }
    s_kept.head = 0;
    s_kept.check = KEPT_MAGIC;
    s_kept.magic = KEPT_MAGIC;
}

static bool s_line_start = true;

static void stamp_line(void)
{
    char ts[24];
    struct timeval tv;
    gettimeofday(&tv, NULL);
    int n = tv.tv_sec > 1700000000 ? snprintf(ts, sizeof(ts), "%lld%03ld ", (long long)tv.tv_sec, (long)(tv.tv_usec / 1000))
                                   : snprintf(ts, sizeof(ts), "+%lld ", (long long)(esp_timer_get_time() / 1000));
    ring_write(ts, n);
    kept_write(ts, n);
}

/* A log call may hold several lines or end mid-line: stamp at every line start. */
static void tee(const char *p, size_t n)
{
    while (n) {
        if (s_line_start) {
            stamp_line();
            s_line_start = false;
        }
        const char *nl = memchr(p, '\n', n);
        size_t seg = nl ? (size_t)(nl - p) + 1 : n;
        ring_write(p, seg);
        kept_write(p, seg);
        p += seg;
        n -= seg;
        s_line_start = nl != NULL;
    }
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
        bool cut = (size_t)len >= sizeof(s_line);
        tee(s_line, cut ? sizeof(s_line) - 1 : (size_t)len);
        if (cut) {
            tee("\n", 1); /* the end of a cut line (and its newline) is lost: keep the next line apart */
        }
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
    kept_init();
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

size_t gw_log_previous(const char **text)
{
    *text = s_prev;
    return s_prev_len;
}
