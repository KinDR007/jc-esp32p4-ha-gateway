/*
 * Radio environment scanner for the P4 gateway (System > Network surroundings, full scan).
 *
 * The P4 flashes this image, reads the result lines from UART0 (460800 Bd) and then flashes the
 * role firmware back. One JSON object per line:
 *   @NS {...}  start            @NW {...}  Wi-Fi access point
 *   @NC {...}  802.15.4 channel: energy (noise) and frames heard
 *   @NB {...}  802.15.4 beacon: Zigbee / Thread / other network that answered the beacon request
 *   @NP {...}  802.15.4 PAN seen in traffic on a channel (also networks that send no beacons)
 *   @ND {...}  done
 * Only integer conversions in printf, no floats or 64-bit types.
 */
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_ieee802154.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#define CH_FIRST 11
#define CH_LAST 26
#define ED_SAMPLES 16
#define ED_SYMBOLS 128   /* 2 ms per energy measurement */
#define LISTEN_MS 400    /* after the beacon request: beacons and ordinary traffic */
#define MAX_PANS 24      /* per channel */
#define MAX_BEACONS 48

typedef struct {
    uint8_t data[128];
    int8_t rssi;
    uint8_t lqi;
} rx_frame_t;

static QueueHandle_t s_rx;
static SemaphoreHandle_t s_ed_done, s_tx_done;
static volatile int8_t s_ed_power;

/* ---------------------------------------------------------------- output */

static void json_str(char *out, size_t size, const uint8_t *s, size_t len)
{
    size_t o = 0;
    for (size_t i = 0; i < len && s[i] && o + 7 < size; i++) {
        uint8_t c = s[i];
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = c;
        } else if (c < 0x20 || c == 0x7f) {
            o += snprintf(out + o, size - o, "\\u%04x", c);
        } else {
            out[o++] = c; /* UTF-8 bytes pass through */
        }
    }
    out[o] = '\0';
}

static void hex(char *out, const uint8_t *p, size_t n, bool reverse)
{
    for (size_t i = 0; i < n; i++) {
        sprintf(out + 2 * i, "%02x", p[reverse ? n - 1 - i : i]);
    }
}

/* ---------------------------------------------------------------- Wi-Fi */

static const char *auth_name(wifi_auth_mode_t a)
{
    switch (a) {
    case WIFI_AUTH_OPEN: return "open";
    case WIFI_AUTH_WEP: return "wep";
    case WIFI_AUTH_WPA_PSK: return "wpa";
    case WIFI_AUTH_WPA2_PSK: return "wpa2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "wpa/wpa2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "wpa2-ent";
    case WIFI_AUTH_WPA3_PSK: return "wpa3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "wpa2/wpa3";
    case WIFI_AUTH_OWE: return "owe";
    default: return "other";
    }
}

static void scan_wifi(void)
{
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    wifi_scan_config_t scan = {
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = {.min = 80, .max = 200},
    };
    /* two passes: an access point missed in one is usually seen in the other */
    for (int pass = 0; pass < 2; pass++) {
        if (esp_wifi_scan_start(&scan, true) != ESP_OK) {
            continue;
        }
        uint16_t n = 0;
        esp_wifi_scan_get_ap_num(&n);
        wifi_ap_record_t *aps = calloc(n ? n : 1, sizeof(*aps));
        if (!aps || esp_wifi_scan_get_ap_records(&n, aps) != ESP_OK) {
            free(aps);
            continue;
        }
        for (int i = 0; i < n; i++) {
            const wifi_ap_record_t *a = &aps[i];
            char ssid[100], bssid[13];
            json_str(ssid, sizeof(ssid), a->ssid, sizeof(a->ssid));
            hex(bssid, a->bssid, 6, false);
            printf("@NW {\"ssid\":\"%s\",\"bssid\":\"%s\",\"ch\":%u,\"ch2\":\"%s\",\"rssi\":%d,\"auth\":\"%s\","
                   "\"phy\":\"%s%s%s%s\"}\n",
                   ssid, bssid, a->primary,
                   a->second == WIFI_SECOND_CHAN_ABOVE ? "above" : a->second == WIFI_SECOND_CHAN_BELOW ? "below" : "none",
                   a->rssi, auth_name(a->authmode), a->phy_11b ? "b" : "", a->phy_11g ? "g" : "", a->phy_11n ? "n" : "",
                   a->phy_11ax ? "ax" : "");
        }
        free(aps);
    }
    fflush(stdout);
    esp_wifi_stop();
    esp_wifi_deinit();
}

/* ---------------------------------------------------------------- 802.15.4 driver callbacks (ISR) */

void IRAM_ATTR esp_ieee802154_receive_done(uint8_t *frame, esp_ieee802154_frame_info_t *info)
{
    rx_frame_t f;
    size_t len = frame[0] < sizeof(f.data) ? frame[0] + 1 : sizeof(f.data);
    memcpy(f.data, frame, len);
    f.rssi = info->rssi;
    f.lqi = info->lqi;
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(s_rx, &f, &woken);
    esp_ieee802154_receive_handle_done(frame);
    portYIELD_FROM_ISR(woken);
}

void IRAM_ATTR esp_ieee802154_energy_detect_done(int8_t power)
{
    BaseType_t woken = pdFALSE;
    s_ed_power = power;
    xSemaphoreGiveFromISR(s_ed_done, &woken);
    portYIELD_FROM_ISR(woken);
}

void IRAM_ATTR esp_ieee802154_transmit_done(const uint8_t *frame, const uint8_t *ack, esp_ieee802154_frame_info_t *info)
{
    BaseType_t woken = pdFALSE;
    if (ack) {
        esp_ieee802154_receive_handle_done(ack);
    }
    xSemaphoreGiveFromISR(s_tx_done, &woken);
    portYIELD_FROM_ISR(woken);
}

void IRAM_ATTR esp_ieee802154_transmit_failed(const uint8_t *frame, esp_ieee802154_tx_error_t error)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_tx_done, &woken);
    portYIELD_FROM_ISR(woken);
}

/* ---------------------------------------------------------------- 802.15.4 scan */

typedef struct {
    uint16_t pan;
    uint16_t frames;
    int8_t rssi;
} pan_t;

typedef struct {
    uint8_t ch;
    uint16_t pan;
    char src[17];
} beacon_key_t;

static beacon_key_t s_beacons[MAX_BEACONS];
static int s_beacon_count;

static bool beacon_seen(uint8_t ch, uint16_t pan, const char *src)
{
    for (int i = 0; i < s_beacon_count; i++) {
        if (s_beacons[i].ch == ch && s_beacons[i].pan == pan && strcmp(s_beacons[i].src, src) == 0) {
            return true;
        }
    }
    if (s_beacon_count < MAX_BEACONS) {
        s_beacons[s_beacon_count] = (beacon_key_t){.ch = ch, .pan = pan};
        strlcpy(s_beacons[s_beacon_count].src, src, sizeof(s_beacons[0].src));
        s_beacon_count++;
    }
    return false;
}

/* MAC address field of a given addressing mode: 2 = short, 3 = extended. Returns its length. */
static int addr_len(int mode)
{
    return mode == 2 ? 2 : mode == 3 ? 8 : 0;
}

static void note_pan(pan_t *pans, int *n, uint16_t pan, int8_t rssi)
{
    for (int i = 0; i < *n; i++) {
        if (pans[i].pan == pan) {
            pans[i].frames++;
            pans[i].rssi = rssi > pans[i].rssi ? rssi : pans[i].rssi;
            return;
        }
    }
    if (*n < MAX_PANS) {
        pans[(*n)++] = (pan_t){.pan = pan, .frames = 1, .rssi = rssi};
    }
}

static void handle_beacon(uint8_t ch, const rx_frame_t *f, const uint8_t *p, const uint8_t *end, uint16_t pan, int src_mode)
{
    char src[17] = "";
    int alen = addr_len(src_mode);
    if (p + alen > end) {
        return;
    }
    hex(src, p, alen, true);
    p += alen;
    if (p + 4 > end) {
        return;
    }
    uint16_t sf = p[0] | p[1] << 8;
    p += 2;
    int gts = p[0] & 7;
    p += 1 + (gts ? 1 + 3 * gts : 0);
    if (p >= end) {
        return;
    }
    int pend_short = p[0] & 7, pend_ext = (p[0] >> 4) & 7;
    p += 1 + 2 * pend_short + 8 * pend_ext;
    if (p > end || beacon_seen(ch, pan, src)) {
        return;
    }
    bool coord = sf & (1 << 14), permit = sf & (1 << 15);
    size_t plen = end - p;
    if (plen >= 15 && p[0] == 0x00) {
        /* Zigbee: protocol id 0, stack profile, router/end device capacity, depth, extended PAN id */
        char epid[17];
        hex(epid, p + 3, 8, true);
        printf("@NB {\"type\":\"zigbee\",\"ch\":%u,\"pan\":\"0x%04x\",\"src\":\"%s\",\"rssi\":%d,\"lqi\":%u,"
               "\"coord\":%s,\"permit\":%s,\"stack\":%u,\"router_cap\":%s,\"ed_cap\":%s,\"depth\":%u,\"epid\":\"%s\","
               "\"update\":%u}\n",
               ch, pan, src, f->rssi, f->lqi, coord ? "true" : "false", permit ? "true" : "false", p[1] & 0x0f,
               (p[2] & 0x04) ? "true" : "false", (p[2] & 0x80) ? "true" : "false", (p[2] >> 3) & 0x0f, epid, p[14]);
    } else if (plen >= 26 && p[0] == 0x03) {
        /* Thread 1.1 beacon payload: protocol id 3, flags, network name, extended PAN id */
        char name[40], epid[17];
        json_str(name, sizeof(name), p + 2, 16);
        hex(epid, p + 18, 8, false);
        printf("@NB {\"type\":\"thread\",\"ch\":%u,\"pan\":\"0x%04x\",\"src\":\"%s\",\"rssi\":%d,\"lqi\":%u,"
               "\"name\":\"%s\",\"epid\":\"%s\",\"joinable\":%s}\n",
               ch, pan, src, f->rssi, f->lqi, name, epid, (p[1] & 0x01) ? "true" : "false");
    } else {
        /* Thread 1.2+ answers without payload; other stacks have their own */
        printf("@NB {\"type\":\"%s\",\"ch\":%u,\"pan\":\"0x%04x\",\"src\":\"%s\",\"rssi\":%d,\"lqi\":%u,\"coord\":%s,"
               "\"permit\":%s,\"payload\":%u}\n",
               plen ? "other" : "no_payload", ch, pan, src, f->rssi, f->lqi, coord ? "true" : "false",
               permit ? "true" : "false", (unsigned)plen);
    }
}

/* Parses the MAC header; beacons are reported, every frame counts for its PAN. */
static void handle_frame(uint8_t ch, const rx_frame_t *f, pan_t *pans, int *npans)
{
    const uint8_t *d = f->data;
    int len = d[0];
    if (len < 5 || len > 127) {
        return;
    }
    const uint8_t *p = d + 1, *end = d + 1 + len - 2; /* without FCS (replaced by RSSI/LQI) */
    uint16_t fcf = p[0] | p[1] << 8;
    int type = fcf & 7, pan_comp = (fcf >> 6) & 1, dst_mode = (fcf >> 10) & 3, version = (fcf >> 12) & 3,
        src_mode = (fcf >> 14) & 3;
    p += 3; /* FCF, sequence number */
    if (version > 1) {
        return; /* 802.15.4-2015 frames: addressing rules differ, not parsed */
    }
    int have_dst_pan = dst_mode != 0;
    uint16_t pan = 0xffff;
    if (have_dst_pan) {
        if (p + 2 > end) {
            return;
        }
        pan = p[0] | p[1] << 8;
        p += 2 + addr_len(dst_mode);
    }
    if (src_mode && !(pan_comp && have_dst_pan)) {
        if (p + 2 > end) {
            return;
        }
        pan = p[0] | p[1] << 8;
        p += 2;
    }
    if (pan != 0xffff) {
        note_pan(pans, npans, pan, f->rssi);
    }
    if (type == 0 && src_mode) {
        handle_beacon(ch, f, p, end, pan, src_mode);
    }
}

static void scan_channel(uint8_t ch, uint8_t seq)
{
    pan_t pans[MAX_PANS];
    int npans = 0, frames = 0;
    rx_frame_t f;

    esp_ieee802154_set_channel(ch);
    xQueueReset(s_rx);
    esp_ieee802154_receive();

    /* noise: strongest and average energy over ED_SAMPLES short measurements */
    int ed_max = -128, ed_sum = 0, ed_n = 0;
    for (int i = 0; i < ED_SAMPLES; i++) {
        if (esp_ieee802154_energy_detect(ED_SYMBOLS) == ESP_OK &&
            xSemaphoreTake(s_ed_done, pdMS_TO_TICKS(50)) == pdTRUE) {
            ed_max = s_ed_power > ed_max ? s_ed_power : ed_max;
            ed_sum += s_ed_power;
            ed_n++;
        }
    }
    esp_ieee802154_receive();

    /* beacon request: MAC command 0x07 to broadcast PAN/address, no source address */
    static uint8_t req[] = {10, 0x03, 0x08, 0, 0xff, 0xff, 0xff, 0xff, 0x07, 0, 0};
    req[3] = seq;
    if (esp_ieee802154_transmit(req, true) == ESP_OK) {
        xSemaphoreTake(s_tx_done, pdMS_TO_TICKS(50));
    }
    esp_ieee802154_receive();

    int64_t until = esp_timer_get_time() + LISTEN_MS * 1000;
    while (esp_timer_get_time() < until) {
        if (xQueueReceive(s_rx, &f, pdMS_TO_TICKS(10)) == pdTRUE) {
            frames++;
            handle_frame(ch, &f, pans, &npans);
        }
    }
    printf("@NC {\"ch\":%u,\"ed_max\":%d,\"ed_avg\":%d,\"frames\":%d}\n", ch, ed_n ? ed_max : 0,
           ed_n ? ed_sum / ed_n : 0, frames);
    for (int i = 0; i < npans; i++) {
        printf("@NP {\"ch\":%u,\"pan\":\"0x%04x\",\"frames\":%u,\"rssi\":%d}\n", ch, pans[i].pan, pans[i].frames,
               pans[i].rssi);
    }
    fflush(stdout);
}

static void scan_802154(void)
{
    s_rx = xQueueCreate(32, sizeof(rx_frame_t));
    s_ed_done = xSemaphoreCreateBinary();
    s_tx_done = xSemaphoreCreateBinary();
    ESP_ERROR_CHECK(esp_ieee802154_enable());
    esp_ieee802154_set_promiscuous(true);
    esp_ieee802154_set_rx_when_idle(true);
    esp_ieee802154_set_panid(0xffff);
    for (uint8_t ch = CH_FIRST; ch <= CH_LAST; ch++) {
        scan_channel(ch, ch);
    }
    esp_ieee802154_disable();
}

void app_main(void)
{
    int64_t start = esp_timer_get_time();
    nvs_flash_init();
    /* the P4 starts reading after the reset; give it a moment */
    vTaskDelay(pdMS_TO_TICKS(500));
    printf("\n@NS {\"ver\":\"%s\"}\n", esp_app_get_description()->version);
    fflush(stdout);
    scan_wifi();
    scan_802154();
    printf("@ND {\"ms\":%lu}\n", (unsigned long)((esp_timer_get_time() - start) / 1000));
    fflush(stdout);
    while (true) {
        vTaskDelay(portMAX_DELAY);
    }
}
