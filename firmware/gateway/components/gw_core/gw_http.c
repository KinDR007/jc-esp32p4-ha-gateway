#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "gw.h"

static const char *TAG = "gw_http";

#define MAX_HANDLERS 80
#define MAX_JSON_BODY 4096

extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[] asm("_binary_index_html_gz_end");
extern const uint8_t p4gw_favicon_ico_start[] asm("_binary_p4gw_favicon_ico_start");
extern const uint8_t p4gw_favicon_ico_end[] asm("_binary_p4gw_favicon_ico_end");

static httpd_handle_t s_server;

typedef struct {
    esp_err_t (*handler)(httpd_req_t *);
    const char *uri;
    bool public;
} route_ctx_t;

static esp_err_t route_handler(httpd_req_t *req)
{
    route_ctx_t *ctx = req->user_ctx;
    if (!ctx->public && !gw_auth_check(req, ctx->uri)) {
        return ESP_OK;
    }
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return ctx->handler(req);
}

static esp_err_t register_route(const char *uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t *),
                                bool public)
{
    route_ctx_t *ctx = malloc(sizeof(*ctx));
    ESP_RETURN_ON_FALSE(ctx, ESP_ERR_NO_MEM, TAG, "no mem");
    ctx->handler = handler;
    ctx->uri = uri;
    ctx->public = public;
    httpd_uri_t route = {.uri = uri, .method = method, .handler = route_handler, .user_ctx = ctx};
    esp_err_t err = httpd_register_uri_handler(s_server, &route);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register %s: %s", uri, esp_err_to_name(err));
        free(ctx);
    }
    return err;
}

esp_err_t gw_http_register(const char *uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t *))
{
    return register_route(uri, method, handler, false);
}

esp_err_t gw_http_register_public(const char *uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t *))
{
    return register_route(uri, method, handler, true);
}

esp_err_t gw_http_send_json(httpd_req_t *req, cJSON *json)
{
    char *text = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!text) {
        return gw_http_send_error(req, 500, "out of memory");
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, text);
    cJSON_free(text);
    return err;
}

esp_err_t gw_http_send_error(httpd_req_t *req, int status, const char *message)
{
    static const struct {
        int code;
        const char *text;
    } statuses[] = {
        {400, "400 Bad Request"}, {401, "401 Unauthorized"}, {403, "403 Forbidden"}, {404, "404 Not Found"}, {409, "409 Conflict"},
        {413, "413 Payload Too Large"}, {500, "500 Internal Server Error"}, {503, "503 Service Unavailable"},
    };
    const char *status_text = "500 Internal Server Error";
    for (size_t i = 0; i < sizeof(statuses) / sizeof(statuses[0]); i++) {
        if (statuses[i].code == status) {
            status_text = statuses[i].text;
        }
    }
    httpd_resp_set_status(req, status_text);
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "error", message);
    return gw_http_send_json(req, json);
}

cJSON *gw_http_read_json(httpd_req_t *req)
{
    return gw_http_read_json_max(req, MAX_JSON_BODY);
}

cJSON *gw_http_read_json_max(httpd_req_t *req, size_t max)
{
    if (req->content_len == 0 || req->content_len > max) {
        gw_http_send_error(req, req->content_len ? 413 : 400, "expected a JSON body");
        return NULL;
    }
    char *body = heap_caps_malloc(req->content_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) {
        gw_http_send_error(req, 500, "out of memory");
        return NULL;
    }
    size_t received = 0;
    while (received < req->content_len) {
        int n = httpd_req_recv(req, body + received, req->content_len - received);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            free(body);
            gw_http_send_error(req, 400, "body read failed");
            return NULL;
        }
        received += n;
    }
    body[received] = '\0';
    cJSON *json = cJSON_Parse(body);
    free(body);
    if (!json) {
        gw_http_send_error(req, 400, "invalid JSON");
    }
    return json;
}

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, (const char *)index_html_gz_start, index_html_gz_end - index_html_gz_start);
}

/* Browsers ask for it before the page says anything, and without a password. */
static esp_err_t favicon_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "image/x-icon");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=604800");
    return httpd_resp_send(req, (const char *)p4gw_favicon_ico_start, p4gw_favicon_ico_end - p4gw_favicon_ico_start);
}

/* ---- push to WebSocket clients (the Home Assistant integration) ---- */

/* Handshake: the same sign-in as the API (token, session or Basic). */
static esp_err_t ws_auth(httpd_req_t *req)
{
    return gw_auth_check(req, "/api/ws") ? ESP_OK : ESP_FAIL;
}

/* Clients only listen: frames they send are read and dropped (pings are answered by the server). */
static esp_err_t ws_handler(httpd_req_t *req)
{
    uint8_t buf[128];
    httpd_ws_frame_t frame = {0};
    if (httpd_ws_recv_frame(req, &frame, 0) != ESP_OK || frame.len > sizeof(buf)) {
        return ESP_FAIL;
    }
    frame.payload = buf;
    return frame.len ? httpd_ws_recv_frame(req, &frame, frame.len) : ESP_OK;
}

/* Messages wait in a queue drained by one job on the server task: its own work queue holds only a few
 * jobs (a UDP mailbox), a burst of one job per message lost most of them. */
#define PUSH_QUEUE_LEN 48
static QueueHandle_t s_push_queue;
static volatile bool s_push_job;
static uint32_t s_push_dropped;

/* Runs on the server task: the socket list does not change under it. */
static void ws_send_all(void *arg)
{
    char *text;
    int fds[CONFIG_LWIP_MAX_SOCKETS];

    s_push_job = false; /* a message queued from now on queues another job */
    while (xQueueReceive(s_push_queue, &text, 0) == pdTRUE) {
        size_t count = sizeof(fds) / sizeof(fds[0]);
        if (httpd_get_client_list(s_server, &count, fds) == ESP_OK) {
            httpd_ws_frame_t frame = {.type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)text, .len = strlen(text)};
            for (size_t i = 0; i < count; i++) {
                if (httpd_ws_get_fd_info(s_server, fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
                    httpd_ws_send_frame_async(s_server, fds[i], &frame);
                }
            }
        }
        cJSON_free(text);
    }
}

int gw_http_ws_clients(void)
{
    int fds[CONFIG_LWIP_MAX_SOCKETS], n = 0;
    size_t count = sizeof(fds) / sizeof(fds[0]);
    if (s_server && httpd_get_client_list(s_server, &count, fds) == ESP_OK) {
        for (size_t i = 0; i < count; i++) {
            n += httpd_ws_get_fd_info(s_server, fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET;
        }
    }
    return n;
}

void gw_http_push(const char *topic, const char *payload)
{
    if (!s_push_queue) {
        return;
    }
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "topic", topic);
    cJSON_AddStringToObject(json, "payload", payload);
    char *text = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!text) {
        return;
    }
    if (xQueueSend(s_push_queue, &text, 0) != pdTRUE) {
        cJSON_free(text);
        if (s_push_dropped++ % 100 == 0) {
            ESP_LOGW(TAG, "push queue full, %" PRIu32 " messages dropped", s_push_dropped);
        }
        return;
    }
    if (!s_push_job) {
        s_push_job = true;
        if (httpd_queue_work(s_server, ws_send_all, NULL) != ESP_OK) {
            s_push_job = false; /* the next message tries again */
        }
    }
}

esp_err_t gw_http_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = MAX_HANDLERS;
    config.stack_size = 10240;
    /* Core 1 belongs to the sound recognition: an upload running there between its short yields
     * (httpd inherits a high priority from the flash lock) starves IDLE1 and trips the task watchdog. */
    config.core_id = 0;
    config.max_open_sockets = 8;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 15;
    config.send_wait_timeout = 15;
    config.uri_match_fn = httpd_uri_match_wildcard;
    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &config), TAG, "start");

    httpd_uri_t index = {.uri = "/", .method = HTTP_GET, .handler = index_handler};
    httpd_register_uri_handler(s_server, &index);
    httpd_uri_t favicon = {.uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_handler};
    httpd_register_uri_handler(s_server, &favicon);
    httpd_uri_t ws = {.uri = "/api/ws", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true,
                      .ws_pre_handshake_cb = ws_auth};
    httpd_register_uri_handler(s_server, &ws);
    s_push_queue = xQueueCreate(PUSH_QUEUE_LEN, sizeof(char *));
    ESP_LOGI(TAG, "web UI on port %d", config.server_port);
    return ESP_OK;
}

httpd_handle_t gw_http_server(void)
{
    return s_server;
}
