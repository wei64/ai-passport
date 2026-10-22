// main/music_player.h -- pure playback state and PCM metadata.
//
// This module deliberately has no ESP-IDF or LVGL dependencies. The hardware
// worker and the UI both use it so list navigation, play/pause transitions, and
// progress calculations can be tested on a host.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MUSIC_PLAYER_TRACK_COUNT 3
#define MUSIC_PLAYER_SAMPLE_RATE 16000
#define MUSIC_PLAYER_CHANNELS 1
#define MUSIC_PLAYER_BITS 16
#define MUSIC_PLAYER_BYTES_PER_SAMPLE \
    ((MUSIC_PLAYER_CHANNELS * MUSIC_PLAYER_BITS) / 8)

// Embedding at build time (rather than decoding MP3/WAV at runtime) avoids a
// codec allocation and lets playback read a stable read-only Flash region.
typedef struct {
    const char *title;
    const char *subtitle;
    const uint8_t *pcm;
    size_t bytes;
} music_track_t;

typedef enum {
    MUSIC_PLAYBACK_STOPPED = 0,
    MUSIC_PLAYBACK_PLAYING,
    MUSIC_PLAYBACK_PAUSED,
    MUSIC_PLAYBACK_FINISHED,
} music_playback_state_t;

typedef enum {
    MUSIC_INPUT_UP = 0,
    MUSIC_INPUT_DOWN,
    MUSIC_INPUT_OK,
    MUSIC_INPUT_STOP,
} music_input_t;

typedef enum {
    MUSIC_ACTION_NONE = 0,
    MUSIC_ACTION_SELECT_PREVIOUS,
    MUSIC_ACTION_SELECT_NEXT,
    MUSIC_ACTION_START,
    MUSIC_ACTION_PAUSE,
    MUSIC_ACTION_RESUME,
    MUSIC_ACTION_STOP,
} music_action_t;

typedef struct {
    size_t selected;
    size_t active;
    size_t offset_bytes;
    music_playback_state_t state;
} music_player_t;

// Reset all runtime state to the first track without touching media bytes.
void music_player_init(music_player_t *player);

// Move selection by delta. A changed selection also becomes the active track
// and resets its offset. Returns false for invalid input or a no-op wrap.
bool music_player_select(music_player_t *player, size_t track_count, int delta);

// Translate one physical-key input into a state transition. The selected track
// starts from offset zero. Pause/resume only act on the active track.
music_action_t music_player_handle_input(music_player_t *player,
                                         size_t track_count,
                                         music_input_t input);

// Successful hardware writes advance the active track. finish marks it COMPLETE
// so it remains visible in the UI and OK can replay it.
void music_player_advance(music_player_t *player, size_t bytes);
void music_player_finish(music_player_t *player);

// Progress in 0..100; stopped-at-start and invalid input report zero.
unsigned music_player_progress_permille(const music_player_t *player,
                                        const music_track_t *track);

// Format bytes as M:SS without floating point or locale-dependent formatting.
// Returns false when the output buffer is too small.
bool music_format_duration(size_t bytes, char *out, size_t out_size);
