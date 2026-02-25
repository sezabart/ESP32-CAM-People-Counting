#include <stdio.h>
#include <string.h>
#include <math.h>
#include "esp_camera.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ESP32_COUNTER";

// --- Settings ---
#define THRESHOLD 15
#define ALPHA 0.05f
#define ZONE_WIDTH 40
#define WIFI_SSID "ESP32-CAM-Counter"
#define STATE_TIMEOUT_MS 2000  // Reset if movement takes too long

// --- State ---
typedef struct {
    int count_left;
    int count_right;
    int average_people;
    float bg_l;
    float bg_r;
} counter_data_t;

volatile counter_data_t state = {0, 0, 0, 128.0f, 128.0f};

// --- Web Handlers ---
esp_err_t get_stats_handler(httpd_req_t *req) {
    char json[128];
    snprintf(json, sizeof(json), "{\"left\":%d, \"right\":%d, \"avg\":%d}", 
             state.count_left, state.count_right, state.average_people);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, strlen(json));
}

esp_err_t post_reset_handler(httpd_req_t *req) {
    state.count_left = 0; state.count_right = 0; state.average_people = 0;
    return httpd_resp_send(req, "Reset OK", 8);
}

esp_err_t http_404_error_handler(httpd_req_t *req, httpd_err_code_t err) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
}

esp_err_t get_root_handler(httpd_req_t *req) {
    const char* html = "<!DOCTYPE html><html><head>"
        "<title>Success</title><meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<style>body{font-family:sans-serif; text-align:center; background:#1a1a1a; color:white;}"
        ".card{background:#2d2d2d; padding:20px; border-radius:15px; display:inline-block; margin-top:30px; min-width:280px; box-shadow: 0 10px 20px rgba(0,0,0,0.5);}"
        ".val{font-size:3.5em; font-weight:bold; color:#00d1b2; margin:10px 0;}"
        "button{background:#ff3860; border:none; color:white; padding:10px 20px; border-radius:5px; cursor:pointer; font-weight:bold; margin-top:10px;}"
        "button:active{background:#c01c3f;}</style></head><body>"
        "<div class='card'><h2>Visitors</h2>"
        "<div id='avg' class='val'>0</div>"
        "<p>Left: <span id='left'>0</span> | Right: <span id='right'>0</span></p>"
        "<button onclick='reset()'>RESET COUNTER</button></div>"
        "<script>function update(){fetch('/stats').then(r=>r.json()).then(d=>{"
        "document.getElementById('avg').innerText=d.avg;"
        "document.getElementById('left').innerText=d.left;"
        "document.getElementById('right').innerText=d.right;});}"
        "function reset(){fetch('/reset',{method:'POST'}).then(()=>update());}"
        "setInterval(update, 1000); update();</script></body></html>";
    return httpd_resp_send(req, html, strlen(html));
}

// --- Logic Task ---
void motion_task(void *pvParameters) {
    int move_state = 0; 
    int64_t last_trip_time = 0;

    while (1) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) { vTaskDelay(1); continue; }

        uint32_t sum_l = 0, sum_r = 0;
        int samples = (fb->height / 2) * ZONE_WIDTH;
        for (int y = 0; y < fb->height; y += 2) {
            for (int x = 0; x < ZONE_WIDTH; x++) {
                sum_l += fb->buf[y * fb->width + x];
                sum_r += fb->buf[y * fb->width + (fb->width - ZONE_WIDTH + x)];
            }
        }
        float cur_l = (float)sum_l / samples;
        float cur_r = (float)sum_r / samples;

        bool l_trip = fabsf(cur_l - state.bg_l) > THRESHOLD;
        bool r_trip = fabsf(cur_r - state.bg_r) > THRESHOLD;
        int64_t now = esp_timer_get_time() / 1000;

        // Reset state if it has been hanging too long
        if (move_state != 0 && (now - last_trip_time) > STATE_TIMEOUT_MS) {
            move_state = 0;
            ESP_LOGD(TAG, "State timeout - resetting to IDLE");
        }

        if (move_state == 0) {
            if (l_trip) { move_state = 1; last_trip_time = now; } 
            else if (r_trip) { move_state = 2; last_trip_time = now; }
        } else if (move_state == 1 && r_trip) { // Moved Left -> Right
            state.count_right++;
            state.average_people = (int)ceilf((state.count_left + state.count_right) / 2.0f);
            move_state = 0; 
            vTaskDelay(pdMS_TO_TICKS(800)); // Debounce
        } else if (move_state == 2 && l_trip) { // Moved Right -> Left
            state.count_left++;
            state.average_people = (int)ceilf((state.count_left + state.count_right) / 2.0f);
            move_state = 0; 
            vTaskDelay(pdMS_TO_TICKS(800)); // Debounce
        }

        if (!l_trip) state.bg_l = (cur_l * ALPHA) + (state.bg_l * (1.0f - ALPHA));
        if (!r_trip) state.bg_r = (cur_r * ALPHA) + (state.bg_r * (1.0f - ALPHA));

        esp_camera_fb_return(fb);
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}

// --- DNS Captive Portal Task ---
void dns_server_task(void *pvParameters) {
    uint8_t pkt[128];
    struct sockaddr_in dest_addr = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = INADDR_ANY };
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    bind(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    while (1) {
        struct sockaddr_in source_addr; socklen_t len = sizeof(source_addr);
        int size = recvfrom(sock, pkt, sizeof(pkt), 0, (struct sockaddr *)&source_addr, &len);
        if (size > 12) {
            pkt[2] |= 0x80; pkt[3] |= 0x80; pkt[7] = 1;
            int idx = size;
            pkt[idx++] = 0xc0; pkt[idx++] = 0x0c; pkt[idx++] = 0x00; pkt[idx++] = 0x01; // Type A
            pkt[idx++] = 0x00; pkt[idx++] = 0x01; pkt[idx++] = 0x00; pkt[idx++] = 0x00; // Class/TTL
            pkt[idx++] = 0x00; pkt[idx++] = 0x3c; pkt[idx++] = 0x00; pkt[idx++] = 0x04; // Len
            pkt[idx++] = 192; pkt[idx++] = 168; pkt[idx++] = 4; pkt[idx++] = 1;
            sendto(sock, pkt, idx, 0, (struct sockaddr *)&source_addr, sizeof(source_addr));
        }
        vTaskDelay(1);
    }
}

void app_main() {
    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_ap();
    
    wifi_config_t wifi_cfg = { .ap = { .ssid = WIFI_SSID, .max_connection = 4, .authmode = WIFI_AUTH_OPEN } };
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &wifi_cfg);
    esp_wifi_start();

    camera_config_t cam_cfg = {
        .pin_pwdn = 32, .pin_reset = -1, .pin_xclk = 0, .pin_sscb_sda = 26, .pin_sscb_scl = 27,
        .pin_d7 = 35, .pin_d6 = 34, .pin_d5 = 39, .pin_d4 = 36, .pin_d3 = 21, .pin_d2 = 19, 
        .pin_d1 = 18, .pin_d0 = 5, .pin_vsync = 25, .pin_href = 23, .pin_pclk = 22,
        .xclk_freq_hz = 20000000, .pixel_format = PIXFORMAT_GRAYSCALE, .frame_size = FRAMESIZE_QVGA, .fb_count = 1
    };
    esp_camera_init(&cam_cfg);

    httpd_handle_t server = NULL;
    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    if (httpd_start(&server, &http_cfg) == ESP_OK) {
        httpd_uri_t r = { "/", HTTP_GET, get_root_handler, NULL };
        httpd_uri_t s = { "/stats", HTTP_GET, get_stats_handler, NULL };
        httpd_uri_t rs = { "/reset", HTTP_POST, post_reset_handler, NULL };
        httpd_register_uri_handler(server, &r);
        httpd_register_uri_handler(server, &s);
        httpd_register_uri_handler(server, &rs);
    }

    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, http_404_error_handler);

    xTaskCreate(dns_server_task, "dns", 2048, NULL, 5, NULL);
    xTaskCreatePinnedToCore(motion_task, "motion", 4096, NULL, 5, NULL, 0);
}