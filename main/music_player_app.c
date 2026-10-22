// main/music_player_app.c -- three-key player UI and bounded streaming worker.
//
// UI pages and application tasks belong in main/. The ES8311 and shared I2C
// drivers stay behind components/bsp; this file only selects a track, serves
// bounded PCM chunks to bsp_audio_write(), and presents playback state.
#include "music_player_app.h"

#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "music_assets.h"
#include "music_player.h"

#include <stdint.h>
#include <string.h>

// The visual language is a compact field recorder rather than the baseline
// test menu: an ink backdrop, warm paper track rows, and one orange transport.
#define COLOR_BG       0x101820
#define COLOR_PAPER    0xF4EBDD
#define COLOR_INK      0x17212B
#define COLOR_MUTED    0x83919B
#define COLOR_ACCENT   0xF26B3A
#define COLOR_ACCENT_2 0xFFC857
#define COLOR_OK       0x70C1A0
#define COLOR_ERROR    0xD9534F
#define COLOR_LINE     0x384753

#define PLAYER_WORKER_STACK      4096
#define PLAYER_STOP_TIMEOUT_MS   3000
#define PLAYER_INPUT_WAIT_MS     20
#define PLAYER_TICK_MS           200

// Keep each blocking I2S write short enough that pause and track changes are
// observed promptly. 2048 bytes at 16 kHz/16 bit is about 64 ms.
#define PCM_CHUNK_BYTES          2048
#define PCM_CHUNK_SAMPLES        (PCM_CHUNK_BYTES / MUSIC_PLAYER_BYTES_PER_SAMPLE)
#define PLAYER_VOLUME_DEFAULT    72

typedef struct {
    music_input_t input;
} player_event_t;

// s_tracks/s_track_count are immutable catalog data. s_player is protected by
// s_player_mutex and is never read or written directly from the LVGL timer.
static const music_track_t *s_tracks;
static size_t s_track_count;
static music_player_t s_player;
static int16_t s_pcm_chunk[PCM_CHUNK_SAMPLES];

static TaskHandle_t s_worker;
static SemaphoreHandle_t s_worker_stopped;
static SemaphoreHandle_t s_player_mutex;
static volatile bool s_worker_stop;
static volatile bool s_audio_busy;

// UI objects are owned by the LVGL context. Worker callbacks take the recursive
// BSP LVGL lock before touching any of them.
static lv_obj_t *s_screen;
static lv_obj_t *s_track_cards[MUSIC_PLAYER_TRACK_COUNT];
static lv_obj_t *s_track_titles[MUSIC_PLAYER_TRACK_COUNT];
static lv_obj_t *s_track_subtitles[MUSIC_PLAYER_TRACK_COUNT];
static lv_obj_t *s_track_numbers[MUSIC_PLAYER_TRACK_COUNT];
static lv_obj_t *s_track_marks[MUSIC_PLAYER_TRACK_COUNT];
static lv_obj_t *s_status_label;
static lv_obj_t *s_progress_label;
static lv_obj_t *s_time_label;
static lv_obj_t *s_battery_label;
static lv_obj_t *s_progress_bar;
static lv_obj_t *s_transport_button;
static lv_obj_t *s_transport_label;
static lv_timer_t *s_ui_timer;

static const char *state_text(music_playback_state_t state) {
    switch (state) {
    case MUSIC_PLAYBACK_PLAYING:  return "PLAYING";
    case MUSIC_PLAYBACK_PAUSED:   return "PAUSED";
    case MUSIC_PLAYBACK_FINISHED: return "COMPLETE";
    case MUSIC_PLAYBACK_STOPPED:
    default:                      return "READY";
    }
}

static const char *transport_text(music_playback_state_t state) {
    switch (state) {
    case MUSIC_PLAYBACK_PLAYING:  return "PAUSE";
    case MUSIC_PLAYBACK_PAUSED:   return "RESUME";
    case MUSIC_PLAYBACK_FINISHED: return "REPLAY";
    case MUSIC_PLAYBACK_STOPPED:
    default:                      return "START TRACK";
    }
}

static uint32_t state_color(music_playback_state_t state) {
    switch (state) {
    case MUSIC_PLAYBACK_PLAYING:  return COLOR_OK;
    case MUSIC_PLAYBACK_PAUSED:   return COLOR_ACCENT_2;
    case MUSIC_PLAYBACK_FINISHED: return COLOR_ACCENT;
    default:                      return COLOR_MUTED;
    }
}

static bool player_lock(void) {
    return s_player_mutex &&
           xSemaphoreTake(s_player_mutex, pdMS_TO_TICKS(500)) == pdTRUE;
}

static void player_unlock(void) {
    if (s_player_mutex) xSemaphoreGive(s_player_mutex);
}

static void style_panel(lv_obj_t *obj, uint32_t background, int radius,
                        lv_coord_t border_width, uint32_t border) {
    lv_obj_set_style_bg_color(obj, lv_color_hex(background), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_border_width(obj, border_width, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(border), 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text,
                            const lv_font_t *font, uint32_t color) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

static void update_track_card(size_t index, const music_player_t *player) {
    if (!player || index >= s_track_count || !s_track_cards[index]) return;

    const bool selected = index == player->selected;
    const bool active = index == player->active &&
                        player->state != MUSIC_PLAYBACK_STOPPED;

    lv_obj_set_style_bg_color(
        s_track_cards[index],
        lv_color_hex(selected ? COLOR_INK : COLOR_PAPER), 0);
    lv_obj_set_style_border_width(s_track_cards[index], selected ? 3 : 0, 0);
    lv_obj_set_style_border_color(
        s_track_cards[index], lv_color_hex(COLOR_ACCENT_2), 0);

    const uint32_t primary = selected ? COLOR_PAPER : COLOR_INK;
    const uint32_t secondary = selected ? COLOR_ACCENT_2 : COLOR_MUTED;
    lv_obj_set_style_text_color(s_track_titles[index], lv_color_hex(primary), 0);
    lv_obj_set_style_text_color(s_track_subtitles[index], lv_color_hex(secondary), 0);
    lv_obj_set_style_text_color(s_track_numbers[index],
                                lv_color_hex(selected ? COLOR_ACCENT_2 : COLOR_ACCENT), 0);
    lv_label_set_text(s_track_marks[index], active ? "NOW" : "");
    lv_obj_set_style_text_color(s_track_marks[index], lv_color_hex(COLOR_OK), 0);
}

// Must be called from an LVGL context with the BSP lock held.
static void render_snapshot(const music_player_t *player) {
    if (!s_screen || !player || s_track_count == 0) return;
    const size_t index = player->active < s_track_count
                       ? player->active : player->selected;
    const music_track_t *track = &s_tracks[index];

    lv_label_set_text(s_status_label, state_text(player->state));
    lv_obj_set_style_text_color(s_status_label,
                                lv_color_hex(state_color(player->state)), 0);
    lv_label_set_text(s_transport_label, transport_text(player->state));
    lv_obj_set_style_bg_color(
        s_transport_button,
        lv_color_hex(player->state == MUSIC_PLAYBACK_PLAYING
                         ? COLOR_INK : COLOR_ACCENT), 0);

    lv_bar_set_value(s_progress_bar,
                     (int32_t)music_player_progress_permille(player, track),
                     LV_ANIM_OFF);

    char elapsed[16];
    char total[16];
    music_format_duration(player->offset_bytes, elapsed, sizeof(elapsed));
    music_format_duration(track->bytes, total, sizeof(total));
    lv_label_set_text_fmt(s_progress_label, "%s\n%s", elapsed, total);
    lv_label_set_text_fmt(s_time_label, "%s  ·  %s", track->title,
                          state_text(player->state));
    for (size_t i = 0; i < s_track_count; ++i) {
        update_track_card(i, player);
    }
}

static void refresh_battery(void) {
    const int soc = bsp_battery_soc();
    if (soc < 0) {
        lv_label_set_text(s_battery_label, "BAT --");
        lv_obj_set_style_text_color(
            s_battery_label, lv_color_hex(COLOR_MUTED), 0);
        return;
    }
    lv_label_set_text_fmt(s_battery_label, "BAT %d%%", soc);
    lv_obj_set_style_text_color(
        s_battery_label,
        lv_color_hex(soc < 20 ? COLOR_ERROR : COLOR_PAPER), 0);
}

// Runs in the LVGL task, which already owns the recursive LVGL lock. The
// application mutex only protects the small playback state snapshot.
static void ui_tick(lv_timer_t *timer) {
    (void)timer;
    if (!s_screen || !player_lock()) return;
    music_player_t snapshot = s_player;
    player_unlock();
    refresh_battery();
    render_snapshot(&snapshot);
}

static void render_shared_state(void) {
    if (!player_lock()) return;
    music_player_t snapshot = s_player;
    player_unlock();

    if (!bsp_lvgl_lock(500)) return;
    render_snapshot(&snapshot);
    bsp_lvgl_unlock();
}

// Button events are delivered by main's single input task. Applying state
// changes here keeps the button callback bounded while a long PCM write is in
// progress; the audio worker observes the new state at its next 64 ms boundary.
static void apply_input(player_event_t event) {
    if (!player_lock()) return;
    const size_t old_selected = s_player.selected;
    (void)music_player_handle_input(&s_player, s_track_count, event.input);
    // A rapid second SELECT can arrive before the worker observes the first one.
    // Resetting here ensures the worker starts exactly at the latest track and
    // not at a stale offset from the previous chunk.
    if (old_selected != s_player.selected) s_player.offset_bytes = 0;
    player_unlock();
    render_shared_state();
}

// Plays from the current shared offset until completion, pause, stop, or a
// track change. The mutex is never held during the blocking audio write.
static void worker_stream(void) {
    s_audio_busy = true;
    for (;;) {
        if (!player_lock()) break;
        if (s_worker_stop || s_player.state != MUSIC_PLAYBACK_PLAYING ||
            s_player.active >= s_track_count) {
            player_unlock();
            break;
        }
        const music_track_t *track = &s_tracks[s_player.active];
        const size_t offset = s_player.offset_bytes;
        const bool finished = offset >= track->bytes;
        player_unlock();

        if (finished) {
            if (player_lock()) {
                if (s_player.state == MUSIC_PLAYBACK_PLAYING &&
                    s_player.active < s_track_count &&
                    s_player.offset_bytes >= s_tracks[s_player.active].bytes) {
                    music_player_finish(&s_player);
                }
                player_unlock();
            }
            break;
        }

        const size_t remaining = track->bytes - offset;
        const size_t bytes = remaining < PCM_CHUNK_BYTES
                           ? remaining : PCM_CHUNK_BYTES;
        memcpy(s_pcm_chunk, track->pcm + offset, bytes);
        if (bsp_audio_write(s_pcm_chunk, bytes) != ESP_OK) {
            if (player_lock()) {
                s_player.state = MUSIC_PLAYBACK_STOPPED;
                player_unlock();
            }
            break;
        }

        if (!player_lock()) break;
        // A selection or stop may have arrived during the blocking write. Never
        // advance a track that the latest input already left.
        if (s_player.active == (size_t)(track - s_tracks) &&
            s_player.offset_bytes == offset &&
            s_player.state == MUSIC_PLAYBACK_PLAYING) {
            music_player_advance(&s_player, bytes);
        }
        player_unlock();
        // The LVGL timer owns repeated progress painting. Rendering every audio
        // chunk would redraw the whole page at ~15 Hz for no visual benefit.
    }
    s_audio_busy = false;
}

static void player_worker(void *argument) {
    (void)argument;
    for (;;) {
        // main applies input changes without blocking; this worker wakes at a
        // bounded interval to observe them. vTaskDelay also yields to IDLE.
        if (!player_lock()) {
            vTaskDelay(pdMS_TO_TICKS(PLAYER_INPUT_WAIT_MS));
            continue;
        }
        const bool should_play = !s_worker_stop &&
            s_player.state == MUSIC_PLAYBACK_PLAYING;
        player_unlock();
        if (s_worker_stop) break;
        if (should_play) {
            worker_stream();
        } else {
            vTaskDelay(pdMS_TO_TICKS(PLAYER_INPUT_WAIT_MS));
        }
    }
    s_audio_busy = false;
    // The stop path waits on this semaphore. Returning from the task is simpler
    // and safer than deleting it from another task while it may still unwind.
    if (s_worker_stopped) xSemaphoreGive(s_worker_stopped);
}

esp_err_t music_player_app_enter(void) {
    s_tracks = music_assets_tracks();
    s_track_count = music_assets_count();
    if (!s_tracks || s_track_count == 0 ||
        s_track_count > MUSIC_PLAYER_TRACK_COUNT) {
        return ESP_ERR_INVALID_STATE;
    }
    music_player_init(&s_player);
    s_player.active = s_player.selected;

    s_screen = lv_obj_create(NULL);
    lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    style_panel(s_screen, COLOR_BG, 0, 0, COLOR_BG);
    lv_obj_set_size(s_screen, 240, 320);

    lv_obj_t *eyebrow = make_label(s_screen, "AI PASSPORT / FIELD NOTES",
                                   &lv_font_montserrat_14, COLOR_ACCENT_2);
    lv_obj_set_pos(eyebrow, 18, 15);

    lv_obj_t *title = make_label(s_screen, "POCKET RADIO",
                                 &lv_font_montserrat_20, COLOR_PAPER);
    lv_obj_set_pos(title, 18, 35);

    s_battery_label = make_label(s_screen, "BAT --",
                                 &lv_font_montserrat_14, COLOR_MUTED);
    lv_obj_set_pos(s_battery_label, 154, 18);
    lv_obj_set_width(s_battery_label, 70);
    lv_obj_set_style_text_align(s_battery_label, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_t *rule = lv_obj_create(s_screen);
    style_panel(rule, COLOR_LINE, 0, 0, COLOR_LINE);
    lv_obj_set_pos(rule, 18, 67);
    lv_obj_set_size(rule, 204, 1);

    // Each row is an individual list item. The selected row is dark, the active
    // track gets a small NOW marker, and all content stays within 204 px.
    for (size_t i = 0; i < s_track_count; ++i) {
        const int y = 77 + (int)i * 55;
        s_track_cards[i] = lv_obj_create(s_screen);
        style_panel(s_track_cards[i], COLOR_PAPER, 6, 0, COLOR_PAPER);
        lv_obj_set_pos(s_track_cards[i], 18, y);
        lv_obj_set_size(s_track_cards[i], 204, 49);

        s_track_numbers[i] = make_label(s_track_cards[i], "01",
                                        &lv_font_montserrat_14, COLOR_ACCENT);
        lv_obj_set_pos(s_track_numbers[i], 8, 4);

        s_track_titles[i] = make_label(s_track_cards[i],
                                       s_tracks[i].title,
                                       &lv_font_montserrat_14, COLOR_INK);
        lv_obj_set_pos(s_track_titles[i], 36, 5);
        lv_obj_set_width(s_track_titles[i], 157);
        lv_label_set_long_mode(s_track_titles[i], LV_LABEL_LONG_CLIP);

        s_track_subtitles[i] = make_label(s_track_cards[i],
                                          s_tracks[i].subtitle,
                                          &lv_font_montserrat_14, COLOR_MUTED);
        lv_obj_set_pos(s_track_subtitles[i], 36, 27);

        s_track_marks[i] = make_label(s_track_cards[i], "",
                                      &lv_font_montserrat_14, COLOR_OK);
        lv_obj_set_pos(s_track_marks[i], 172, 27);
    }

    s_status_label = make_label(s_screen, "READY",
                                &lv_font_montserrat_14, COLOR_MUTED);
    lv_obj_set_pos(s_status_label, 18, 248);
    lv_obj_set_width(s_status_label, 58);

    s_time_label = make_label(s_screen, "",
                              &lv_font_montserrat_14, COLOR_PAPER);
    lv_obj_set_pos(s_time_label, 72, 248);
    lv_obj_set_width(s_time_label, 150);
    lv_obj_set_style_text_align(s_time_label, LV_TEXT_ALIGN_RIGHT, 0);

    s_progress_bar = lv_bar_create(s_screen);
    lv_obj_set_pos(s_progress_bar, 18, 270);
    lv_obj_set_size(s_progress_bar, 204, 7);
    lv_bar_set_range(s_progress_bar, 0, 1000);
    lv_obj_set_style_bg_color(s_progress_bar, lv_color_hex(COLOR_LINE), 0);
    lv_obj_set_style_bg_opa(s_progress_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_progress_bar, 0, 0);
    lv_obj_set_style_radius(s_progress_bar, 2, 0);
    lv_obj_set_style_bg_color(s_progress_bar, lv_color_hex(COLOR_ACCENT),
                              LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_progress_bar, 2, LV_PART_INDICATOR);

    s_transport_button = lv_obj_create(s_screen);
    style_panel(s_transport_button, COLOR_ACCENT, 6, 0, COLOR_ACCENT);
    lv_obj_set_pos(s_transport_button, 18, 288);
    lv_obj_set_size(s_transport_button, 146, 25);
    s_transport_label = make_label(s_transport_button, "START TRACK",
                                   &lv_font_montserrat_14, COLOR_PAPER);
    lv_obj_center(s_transport_label);

    s_progress_label = make_label(s_screen, "0:00\n0:00",
                                  &lv_font_montserrat_14, COLOR_MUTED);
    lv_obj_set_pos(s_progress_label, 170, 289);
    lv_obj_set_width(s_progress_label, 52);
    lv_obj_set_style_text_align(s_progress_label, LV_TEXT_ALIGN_RIGHT, 0);

    s_ui_timer = lv_timer_create(ui_tick, PLAYER_TICK_MS, NULL);
    refresh_battery();
    render_snapshot(&s_player);
    lv_screen_load(s_screen);
    return ESP_OK;
}

esp_err_t music_player_app_start(void) {
    if (s_worker) return ESP_OK;

    s_player_mutex = xSemaphoreCreateMutex();
    s_worker_stopped = xSemaphoreCreateBinary();
    if (!s_player_mutex || !s_worker_stopped) {
        if (s_player_mutex) vSemaphoreDelete(s_player_mutex);
        if (s_worker_stopped) vSemaphoreDelete(s_worker_stopped);
        s_player_mutex = NULL;
        s_worker_stopped = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_worker_stop = false;
    s_audio_busy = false;
    const bool audio_ready =
        bsp_audio_init() == ESP_OK &&
        bsp_audio_set_format(MUSIC_PLAYER_SAMPLE_RATE,
                             MUSIC_PLAYER_BITS,
                             MUSIC_PLAYER_CHANNELS) == ESP_OK;
    if (audio_ready) bsp_audio_set_volume(PLAYER_VOLUME_DEFAULT);

    if (xTaskCreate(player_worker, "music_player", PLAYER_WORKER_STACK,
                    NULL, 4, &s_worker) != pdPASS) {
        vSemaphoreDelete(s_worker_stopped);
        vSemaphoreDelete(s_player_mutex);
        s_worker_stopped = NULL;
        s_player_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t music_player_app_stop(void) {
    if (!s_worker) return ESP_OK;

    s_worker_stop = true;
    if (!s_worker_stopped ||
        xSemaphoreTake(s_worker_stopped,
                       pdMS_TO_TICKS(PLAYER_STOP_TIMEOUT_MS)) != pdTRUE) {
        // Keep every handle alive so the caller can retry. Deleting a worker
        // still inside bsp_audio_write() could leave I2S in an unknown state.
        return ESP_ERR_TIMEOUT;
    }

    // The worker acknowledges from inside its own task and then returns.
    s_worker = NULL;
    vSemaphoreDelete(s_worker_stopped);
    vSemaphoreDelete(s_player_mutex);
    s_worker_stopped = NULL;
    s_player_mutex = NULL;
    return ESP_OK;
}

void music_player_app_exit(void) {
    if (s_ui_timer) {
        lv_timer_delete(s_ui_timer);
        s_ui_timer = NULL;
    }
    if (s_screen) {
        lv_obj_delete(s_screen);
        s_screen = NULL;
    }
    for (size_t i = 0; i < MUSIC_PLAYER_TRACK_COUNT; ++i) {
        s_track_cards[i] = NULL;
        s_track_titles[i] = NULL;
        s_track_subtitles[i] = NULL;
        s_track_numbers[i] = NULL;
        s_track_marks[i] = NULL;
    }
    s_status_label = NULL;
    s_progress_label = NULL;
    s_time_label = NULL;
    s_battery_label = NULL;
    s_progress_bar = NULL;
    s_transport_button = NULL;
    s_transport_label = NULL;
}

void music_player_app_key(uint8_t button, uint8_t event) {
    if (s_worker_stop || !s_worker) return;
    if (event != BSP_BTN_CLICK && event != BSP_BTN_LONG) return;

    music_input_t input;
    if (event == BSP_BTN_LONG && button == BSP_BTN_OK) {
        input = MUSIC_INPUT_STOP;
    } else if (event != BSP_BTN_CLICK) {
        return;
    } else if (button == BSP_BTN_UP) {
        input = MUSIC_INPUT_UP;
    } else if (button == BSP_BTN_DOWN) {
        input = MUSIC_INPUT_DOWN;
    } else if (button == BSP_BTN_OK) {
        input = MUSIC_INPUT_OK;
    } else {
        return;
    }
    const player_event_t item = { .input = input };
    apply_input(item);
}

bool music_player_app_audio_busy(void) {
    return s_audio_busy;
}
