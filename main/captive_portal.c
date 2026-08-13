/**
 * @file  captive_portal.c
 * @brief Captive Portal — implementation
 */

#include "captive_portal.h"
#include "wifi_manager.h"

#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "esp_log.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

static const char *TAG = "PORTAL";

// =============================================================================
// HTML Pages
// =============================================================================

static const char *PAGE_CONFIG =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Resilient IoT Edge</title>"
    "<style>"
    "body{font-family:sans-serif;max-width:380px;margin:50px auto;padding:20px}"
    "h2{margin-bottom:4px}p{color:#555;font-size:14px}"
    "label{display:block;margin:12px 0 4px;font-weight:600;font-size:14px}"
    "input{width:100%;padding:10px;border:1px solid #ccc;border-radius:4px;"
    "box-sizing:border-box;font-size:16px}"
    "button{width:100%;padding:12px;margin-top:20px;background:#0070f3;"
    "color:#fff;border:none;border-radius:4px;font-size:16px;cursor:pointer}"
    "</style></head><body>"
    "<h2>Resilient IoT Edge</h2>"
    "<p>Enter your Wi-Fi credentials to connect the device.</p>"
    "<form method='post' action='/connect'>"
    "<label>Network (SSID)</label>"
    "<input type='text' name='ssid' placeholder='Wi-Fi network name' required>"
    "<label>Password</label>"
    "<input type='password' name='password' placeholder='Wi-Fi password'>"
    "<button type='submit'>Connect Device</button>"
    "</form></body></html>";

static const char *PAGE_CONNECTING =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Connecting</title>"
    "<style>body{font-family:sans-serif;max-width:380px;margin:50px auto;"
    "padding:20px;text-align:center}h2{color:#28a745}</style></head>"
    "<body><h2>Connecting...</h2>"
    "<p>The device is connecting to your Wi-Fi network.</p>"
    "<p>You may close this page.</p>"
    "</body></html>";

static const char *PAGE_ERROR =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Error</title>"
    "<style>body{font-family:sans-serif;max-width:380px;margin:50px auto;"
    "padding:20px;text-align:center}h2{color:#dc3545}"
    "a{color:#0070f3}</style></head>"
    "<body><h2>SSID Required</h2>"
    "<p>Please enter the network name.</p>"
    "<p><a href='/'>Try again</a></p>"
    "</body></html>";

// =============================================================================
// URL Decoder and Form Parser
// =============================================================================

static void prv_url_decode(char *dst, const char *src, size_t max_len)
{
    size_t i = 0;
    while (*src && i < max_len - 1U) {
        if (*src == '%' && src[1] && src[2]) {
            int hex = 0;
            sscanf(src + 1, "%2x", &hex);
            dst[i++] = (char)hex;
            src += 3;
        } else if (*src == '+') {
            dst[i++] = ' ';
            src++;
        } else {
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
}

static void prv_parse_form(const char *body,
                            char *ssid, size_t ssid_max,
                            char *pass, size_t pass_max)
{
    ssid[0] = '\0';
    pass[0] = '\0';

    const char *s = strstr(body, "ssid=");
    if (s) {
        s += 5;
        const char *end = strchr(s, '&');
        size_t len = end ? (size_t)(end - s) : strlen(s);
        char encoded[WIFI_SSID_MAX_LEN * 3] = {0};
        if (len > sizeof(encoded) - 1) len = sizeof(encoded) - 1;
        strncpy(encoded, s, len);
        prv_url_decode(ssid, encoded, ssid_max);
    }

    const char *p = strstr(body, "password=");
    if (p) {
        p += 9;
        const char *end = strchr(p, '&');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        char encoded[WIFI_PASS_MAX_LEN * 3] = {0};
        if (len > sizeof(encoded) - 1) len = sizeof(encoded) - 1;
        strncpy(encoded, p, len);
        prv_url_decode(pass, encoded, pass_max);
    }
}

// =============================================================================
// HTTP Handlers
// =============================================================================

static esp_err_t prv_get_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, PAGE_CONFIG, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t prv_post_connect(httpd_req_t *req)
{
    char body[256] = {0};
    int len = req->content_len;

    if (len >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");
        return ESP_FAIL;
    }

    int received = 0;
    while (received < len) {
        int ret = httpd_req_recv(req, body + received, len - received);
        if (ret <= 0) {
            httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "Timeout");
            return ESP_FAIL;
        }
        received += ret;
    }
    body[received] = '\0';

    char ssid[WIFI_SSID_MAX_LEN] = {0};
    char pass[WIFI_PASS_MAX_LEN] = {0};
    prv_parse_form(body, ssid, sizeof(ssid), pass, sizeof(pass));

    if (strlen(ssid) == 0) {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, PAGE_ERROR, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // Save to NVS and signal FSM
    wifi_manager_save_credentials(ssid, pass);
    xEventGroupSetBits(g_wifi_event_group, WIFI_EVT_CREDENTIALS_SET);
    ESP_LOGI(TAG, "Credentials received: SSID='%s' — signaling FSM", ssid);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, PAGE_CONNECTING, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Catch-all: redirect iOS/Android captive portal detection to config page
static esp_err_t prv_catch_all(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// =============================================================================
// DNS Responder Task
// =============================================================================

#define DNS_PORT     53
#define DNS_BUF_SIZE 512
#define AP_IP_U32    0xC0A80401U  // 192.168.4.1

static int            s_dns_sock        = -1;
static TaskHandle_t   s_dns_task_handle = NULL;

static int prv_build_dns_response(const uint8_t *query, int qlen,
                                   uint8_t *resp,  int resp_max)
{
    if (qlen < 12 || qlen + 16 > resp_max) return -1;

    memcpy(resp, query, qlen);

    // Mark as response, authoritative, no error
    resp[2] = 0x81;
    resp[3] = 0x80;
    // Answer count = 1
    resp[6] = 0x00;
    resp[7] = 0x01;

    int pos = qlen;
    resp[pos++] = 0xC0; resp[pos++] = 0x0C; // Name pointer → offset 12
    resp[pos++] = 0x00; resp[pos++] = 0x01; // Type A
    resp[pos++] = 0x00; resp[pos++] = 0x01; // Class IN
    resp[pos++] = 0x00; resp[pos++] = 0x00; // TTL
    resp[pos++] = 0x00; resp[pos++] = 0x0A; // TTL = 10s
    resp[pos++] = 0x00; resp[pos++] = 0x04; // RDLENGTH = 4
    resp[pos++] = 192;                       // 192.168.4.1
    resp[pos++] = 168;
    resp[pos++] = 4;
    resp[pos++] = 1;

    return pos;
}

static void prv_dns_task(void *pvParameters)
{
    s_dns_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_dns_sock < 0) {
        ESP_LOGE(TAG, "DNS socket failed");
        vTaskDelete(NULL);
        return;
    }

    int reuse = 1;
    setsockopt(s_dns_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in server = {
        .sin_family      = AF_INET,
        .sin_port        = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(s_dns_sock, (struct sockaddr *)&server, sizeof(server)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed");
        close(s_dns_sock);
        s_dns_sock = -1;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS responder active on port 53");

    uint8_t rx[DNS_BUF_SIZE], tx[DNS_BUF_SIZE];
    struct sockaddr_in client;
    socklen_t clen = sizeof(client);

    while (1) {
        int len = recvfrom(s_dns_sock, rx, sizeof(rx), 0,
                           (struct sockaddr *)&client, &clen);
        if (len <= 0) break; // Socket closed — task exits cleanly

        int resp_len = prv_build_dns_response(rx, len, tx, sizeof(tx));
        if (resp_len > 0) {
            sendto(s_dns_sock, tx, resp_len, 0,
                   (struct sockaddr *)&client, clen);
        }
    }

    close(s_dns_sock);
    s_dns_sock = -1;
    vTaskDelete(NULL);
}

// =============================================================================
// Public API
// =============================================================================

static httpd_handle_t s_server = NULL;

esp_err_t captive_portal_start(void)
{
    // ── HTTP Server ───────────────────────────────────────────────────────────
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.uri_match_fn     = httpd_uri_match_wildcard;

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server failed to start");
        return ESP_FAIL;
    }

    httpd_uri_t uri_root = {
        .uri     = "/",
        .method  = HTTP_GET,
        .handler = prv_get_root,
    };
    httpd_uri_t uri_connect = {
        .uri     = "/connect",
        .method  = HTTP_POST,
        .handler = prv_post_connect,
    };
    httpd_uri_t uri_catch = {
        .uri     = "/*",
        .method  = HTTP_GET,
        .handler = prv_catch_all,
    };

    httpd_register_uri_handler(s_server, &uri_root);
    httpd_register_uri_handler(s_server, &uri_connect);
    httpd_register_uri_handler(s_server, &uri_catch);

    ESP_LOGI(TAG, "HTTP server started at 192.168.4.1");

    // ── DNS Responder ─────────────────────────────────────────────────────────
    xTaskCreate(prv_dns_task, "dns_task", 4096, NULL, 5, &s_dns_task_handle);

    return ESP_OK;
}

esp_err_t captive_portal_stop(void)
{
    // Stop DNS: closing the socket causes recvfrom() to return -1 → task exits
    if (s_dns_sock >= 0) {
        close(s_dns_sock);
        s_dns_sock = -1;
    }
    vTaskDelay(pdMS_TO_TICKS(100)); // Give task time to exit

    // Stop HTTP server
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }

    ESP_LOGI(TAG, "Captive portal stopped");
    return ESP_OK;
}