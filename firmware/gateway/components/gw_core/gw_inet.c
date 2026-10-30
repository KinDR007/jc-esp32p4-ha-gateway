#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "gw.h"

static const char *TAG = "gw_inet";

/* Internet reachability for the web UI header: a TCP connection to the host the update check uses
 * (DNS and a route out both work), every minute. */
#define CHECK_HOST "github.com"
#define CHECK_PORT "443"
#define CHECK_TIMEOUT_S 5
#define INTERVAL_S 60

static volatile int s_state = -1; /* -1 not checked yet, 0 unreachable, 1 reachable */
static volatile int64_t s_checked_us;

static bool reachable(void)
{
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM}, *res = NULL;
    if (getaddrinfo(CHECK_HOST, CHECK_PORT, &hints, &res) != 0 || !res) {
        return false;
    }
    bool ok = false;
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd >= 0) {
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        if (connect(fd, res->ai_addr, res->ai_addrlen) == 0) {
            ok = true;
        } else if (errno == EINPROGRESS) {
            fd_set wr;
            FD_ZERO(&wr);
            FD_SET(fd, &wr);
            struct timeval tv = {.tv_sec = CHECK_TIMEOUT_S};
            int err = 0;
            socklen_t len = sizeof(err);
            ok = select(fd + 1, NULL, &wr, NULL, &tv) == 1 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 &&
                 err == 0;
        }
        close(fd);
    }
    freeaddrinfo(res);
    return ok;
}

static void inet_task(void *arg)
{
    while (true) {
        if (gw_eth_wait_ip(portMAX_DELAY)) {
            int state = reachable();
            if (state != s_state) {
                ESP_LOGI(TAG, "internet %s", state ? "reachable" : "unreachable");
            }
            s_state = state;
            s_checked_us = esp_timer_get_time();
        }
        vTaskDelay(pdMS_TO_TICKS(INTERVAL_S * 1000));
    }
}

void gw_inet_start(void)
{
    xTaskCreate(inet_task, "gw_inet", 4096, NULL, 2, NULL);
}

int gw_inet_state(void)
{
    return s_state;
}

double gw_inet_age_s(void)
{
    return s_checked_us ? (esp_timer_get_time() - s_checked_us) / 1e6 : -1;
}
