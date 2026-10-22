// main/main.c -- standalone three-key music player application.
//
// This is a derivative application, not the baseline hardware-test menu. The
// BSP still owns the display, ADC buttons, ES8311, battery gauge, and shared
// I2C bus; main/ owns the product UI, playback state, and application worker.
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "key_guard.h"
#include "music_player_app.h"

static const char *TAG = "music_app";

#define INPUT_QUEUE_DEPTH 8
#define INPUT_TASK_STACK 3072
#define KEY_GUARD_MAX_US (10LL * 1000 * 1000)

typedef struct {
    bsp_btn_t button;
    bsp_btn_ev_t event;
} input_event_t;

static QueueHandle_t s_input_queue;
static bool s_input_ready;
static key_guard_t s_key_guard;

// Runs on the button component's shared timer callback. Keep this bounded:
// queue the event and return, never touch LVGL or the audio codec here.
static void on_key(bsp_btn_t button, bsp_btn_ev_t event, void *user) {
    (void)user;
    const bool was_armed = s_key_guard.armed;
    if (key_guard_consume(&s_key_guard, event == BSP_BTN_RELEASE,
                          esp_timer_get_time())) {
        if (was_armed && !s_key_guard.armed) {
            ESP_LOGI(TAG, "wake key released; input enabled");
        }
        return;
    }
    if (!s_input_ready || !s_input_queue) return;
    const input_event_t input = { .button = button, .event = event };
    (void)xQueueSend(s_input_queue, &input, 0);
}

static void input_task(void *argument) {
    (void)argument;
    input_event_t input;
    for (;;) {
        if (xQueueReceive(s_input_queue, &input, portMAX_DELAY) != pdTRUE) continue;
        music_player_app_key((uint8_t)input.button, (uint8_t)input.event);
    }
}

static esp_err_t input_start(void) {
    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(input_event_t));
    if (!s_input_queue) return ESP_ERR_NO_MEM;
    if (xTaskCreate(input_task, "key_input", INPUT_TASK_STACK,
                    NULL, 5, NULL) != pdPASS) {
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t error = bsp_button_init(on_key, NULL);
    if (error != ESP_OK) {
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
        return error;
    }
    // The wake key may have been released before the button driver installed
    // its callback. Synchronize against the physical level to disarm promptly.
    if (s_key_guard.armed) {
        const int millivolts = bsp_button_read_mv();
        key_guard_sync_released(&s_key_guard,
                                millivolts >= BSP_BTN_MV_RELEASED_MIN);
    }
    s_input_ready = true;
    return ESP_OK;
}

void app_main(void) {
    ESP_LOGI(TAG, "Pocket Radio starting");

    const uint32_t wake_causes = esp_sleep_get_wakeup_causes();
    if (wake_causes & (1u << ESP_SLEEP_WAKEUP_GPIO)) {
        // A deep-sleep key wake restarts the chip while the key is still held.
        // Swallow that physical gesture until release so it cannot trigger play.
        key_guard_arm(&s_key_guard, esp_timer_get_time(), KEY_GUARD_MAX_US);
    }

    esp_err_t error = bsp_i2c_init();
    if (error != ESP_OK) ESP_LOGE(TAG, "shared I2C init failed: %s",
                                  esp_err_to_name(error));

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "display/LVGL init failed; player cannot start");
        return;
    }
    bsp_display_backlight(100);

    // Battery is optional; enter() renders "BAT --" until the gauge responds.
    error = bsp_battery_init();
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "battery gauge unavailable: %s", esp_err_to_name(error));
    }

    if (!bsp_lvgl_lock(1000)) {
        ESP_LOGE(TAG, "could not acquire LVGL lock for initial page");
        return;
    }
    error = music_player_app_enter();
    bsp_lvgl_unlock();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "player UI creation failed: %s", esp_err_to_name(error));
        return;
    }

    // Audio init and the worker are deliberately started outside the LVGL lock.
    error = music_player_app_start();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "audio player start failed: %s", esp_err_to_name(error));
        // The UI remains useful for showing track metadata, but no input can
        // produce sound. Report this as a degraded boot rather than rebooting.
    }

    error = input_start();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "button input init failed: %s", esp_err_to_name(error));
    }
    ESP_LOGI(TAG, "Pocket Radio ready: UP/DOWN select, OK play/pause, OK long stop");
}
