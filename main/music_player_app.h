// main/music_player_app.h -- lifecycle API for the standalone music player app.
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

// Create the player UI, but do not start audio work or touch the codec.
// Must be called while holding the BSP LVGL lock.
esp_err_t music_player_app_enter(void);

// Start the blocking audio worker and open the codec. Call without the LVGL lock.
esp_err_t music_player_app_start(void);

// Stop the audio worker with a bounded handshake. Call without the LVGL lock.
esp_err_t music_player_app_stop(void);

// Release the UI. Only call after stop succeeded and while holding LVGL lock.
void music_player_app_exit(void);

// Queue one short/long key event; this is the only entry used by the button callback.
void music_player_app_key(uint8_t button, uint8_t event);

// True while the worker owns a blocking PCM write.
bool music_player_app_audio_busy(void);
