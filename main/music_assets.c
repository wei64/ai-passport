// main/music_assets.c -- metadata for PCM files embedded by CMake.
//
// target_add_binary_data() in main/CMakeLists.txt produces the byte and length
// symbols. Keep the PCM files in assets/music/pcm as the reviewable source; the
// generated assembler files live only under build/.
#include "music_assets.h"

#include <stdbool.h>
#include <stdint.h>

// RENAME_TO in main/CMakeLists.txt stabilizes these symbols even if the asset
// directory moves. ESP-IDF emits a byte array plus a 32-bit length object.
extern const uint8_t _binary_music_morning_postcard_start[];
extern const uint32_t music_morning_postcard_length;
extern const uint8_t _binary_music_blue_hour_start[];
extern const uint32_t music_blue_hour_length;
extern const uint8_t _binary_music_little_engine_start[];
extern const uint32_t music_little_engine_length;

static music_track_t s_tracks[MUSIC_PLAYER_TRACK_COUNT];
static bool s_tracks_ready;

const music_track_t *music_assets_tracks(void) {
    if (!s_tracks_ready) {
        s_tracks[0] = (music_track_t){
            .title = "Morning Postcard",
            .subtitle = "Bright / 112 BPM",
            .pcm = _binary_music_morning_postcard_start,
            .bytes = music_morning_postcard_length,
        };
        s_tracks[1] = (music_track_t){
            .title = "Blue Hour",
            .subtitle = "Calm / 88 BPM",
            .pcm = _binary_music_blue_hour_start,
            .bytes = music_blue_hour_length,
        };
        s_tracks[2] = (music_track_t){
            .title = "Little Engine",
            .subtitle = "Playful / 136 BPM",
            .pcm = _binary_music_little_engine_start,
            .bytes = music_little_engine_length,
        };
        s_tracks_ready = true;
    }
    return s_tracks;
}

size_t music_assets_count(void) {
    return MUSIC_PLAYER_TRACK_COUNT;
}
