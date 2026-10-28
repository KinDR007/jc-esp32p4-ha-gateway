#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "gw.h"
#include "mbedtls/base64.h"

static const char *TAG = "gw_http";

#define MAX_HANDLERS 64
#define MAX_JSON_BODY 4096

extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[] asm("_binary_index_html_gz_end");

static httpd_handle_t s_server;

typedef struct {
    esp_err_t (*handler)(httpd_req_t *);
} route_ctx_t;

static bool constant_time_equal(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    unsigned diff = la ^ lb;
    for (size_t i = 0; i < la; i++) {
        diff |= (unsigned char)a[i] ^ (unsigned char)(i < lb ? b[i] : 0);
    }
    return diff == 0;
}

/* HTTP Basic auth with user "admin" once a password is set in the UI. */
static bool authorized(httpd_req_t *req)
{
    char password[65];
    if (!gw_config_get_str(GW_KEY_PASSWORD, password, sizeof(password)) || password[0] == '\0') {
        return true;
    }
    char header[160];
    unsigned char decoded[128];
    size_t decoded_len = 0;
    bool ok = false;

    if (httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header)) == ESP_OK &&
        strncmp(header, "Basic ", 6) == 0 &&
        mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &decoded_len, (const unsigned char *)header + 6,
                              strlen(header + 6)) == 0) {
        decoded[decoded_len] = '\0';
        char *sep = strchr((char *)decoded, ':');
        if (sep) {
            *sep = '\0';
            ok = strcmp((char *)decoded, "admin") == 0 && constant_time_equal(sep + 1, password);
        }
    }
    if (!ok) {
        /* No WWW-Authenticate challenge: the UI shows its own login form. */
        gw_http_send_error(req, 401, "unauthorized");
    }
    return ok;
}

static esp_err_t route_handler(httpd_req_t *req)
{
    route_ctx_t *ctx = req->user_ctx;
    if (!authorized(req)) {
        return ESP_OK;
    }
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return ctx->handler(req);
}

esp_err_t gw_http_register(const char *uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t *))
{
    route_ctx_t *ctx = malloc(sizeof(*ctx));
    ESP_RETURN_ON_FALSE(ctx, ESP_ERR_NO_MEM, TAG, "no mem");
    ctx->handler = handler;
    httpd_uri_t route = {.uri = uri, .method = method, .handler = route_handler, .user_ctx = ctx};
    esp_err_t err = httpd_register_uri_handler(s_server, &route);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register %s: %s", uri, esp_err_to_name(err));
        free(ctx);
    }
    return err;
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
        {400, "400 Bad Request"}, {401, "401 Unauthorized"}, {404, "404 Not Found"}, {409, "409 Conflict"},
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
    if (req->content_len == 0 || req->content_len > MAX_JSON_BODY) {
        gw_http_send_error(req, req->content_len ? 413 : 400, "expected a JSON body");
        return NULL;
    }
    char *body = malloc(req->content_len + 1);
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

esp_err_t gw_http_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = MAX_HANDLERS;
    config.stack_size = 10240;
    config.max_open_sockets = 8;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 15;
    config.send_wait_timeout = 15;
    config.uri_match_fn = httpd_uri_match_wildcard;
    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &config), TAG, "start");

    httpd_uri_t index = {.uri = "/", .method = HTTP_GET, .handler = index_handler};
    httpd_register_uri_handler(s_server, &index);
    ESP_LOGI(TAG, "web UI on port %d", config.server_port);
    return ESP_OK;
}

httpd_handle_t gw_http_server(void)
{
    return s_server;
}
