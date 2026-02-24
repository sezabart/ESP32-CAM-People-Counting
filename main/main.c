#include "esp_camera.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define THRESHOLD 15      // Sensitivity: lower is more sensitive
#define ZONE_WIDTH 40     // Width of the detection zones in pixels
#define COOLDOWN_MS 500   // Prevent double counting

static const char *TAG = "PEOPLE_COUNTER";

typedef enum { IDLE, LEFT_TRIGGERED, RIGHT_TRIGGERED } state_t;
state_t current_state = IDLE;
int count_in = 0;
int count_out = 0;

// Helper to get average brightness of a vertical zone
uint32_t get_zone_avg(camera_fb_t *fb, int start_x) {
    uint32_t total = 0;
    for (int y = 0; y < fb->height; y++) {
        for (int x = start_x; x < start_x + ZONE_WIDTH; x++) {
            total += fb->buf[y * fb->width + x];
        }
    }
    return total / (fb->height * ZONE_WIDTH);
}

void app_main() {
    // 1. Camera Init (AI-Thinker Pins)
    camera_config_t config = {
        .pin_pwdn = 32, .pin_reset = -1, .pin_xclk = 0,
        .pin_sscb_sda = 26, .pin_sscb_scl = 27,
        .pin_d7 = 35, .pin_d6 = 34, .pin_d5 = 39, .pin_d4 = 36,
        .pin_d3 = 21, .pin_d2 = 19, .pin_d1 = 18, .pin_d0 = 5,
        .pin_vsync = 25, .pin_href = 23, .pin_pclk = 22,
        .xclk_freq_hz = 20000000, .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0, .pixel_format = PIXFORMAT_GRAYSCALE,
        .frame_size = FRAMESIZE_QVGA, .jpeg_quality = 12, .fb_count = 1
    };
    esp_camera_init(&config);

    uint32_t last_left = 0, last_right = 0;

    while (1) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) continue;

        uint32_t avg_left = get_zone_avg(fb, 10); // Zone on the left
        uint32_t avg_right = get_zone_avg(fb, fb->width - ZONE_WIDTH - 10); // Zone on the right

        // Detect change compared to previous frame
        bool left_active = abs((int)avg_left - (int)last_left) > THRESHOLD;
        bool right_active = abs((int)avg_right - (int)last_right) > THRESHOLD;

        // Simple State Machine
        if (current_state == IDLE) {
            if (left_active) current_state = LEFT_TRIGGERED;
            else if (right_active) current_state = RIGHT_TRIGGERED;
        } 
        else if (current_state == LEFT_TRIGGERED && right_active) {
            count_in++;
            ESP_LOGI(TAG, "Person entered! Total IN: %d", count_in);
            current_state = IDLE;
            vTaskDelay(pdMS_TO_TICKS(COOLDOWN_MS));
        } 
        else if (current_state == RIGHT_TRIGGERED && left_active) {
            count_out++;
            ESP_LOGI(TAG, "Person exited! Total OUT: %d", count_out);
            current_state = IDLE;
            vTaskDelay(pdMS_TO_TICKS(COOLDOWN_MS));
        }

        last_left = avg_left;
        last_right = avg_right;
        esp_camera_fb_return(fb);
        
        vTaskDelay(pdMS_TO_TICKS(50)); // ~20 FPS
    }
}