#include "music_player.h"

#include <stdio.h>

void music_player_init(music_player_t *player) {
    if (!player) return;
    player->selected = 0;
    player->active = 0;
    player->offset_bytes = 0;
    player->state = MUSIC_PLAYBACK_STOPPED;
}

bool music_player_select(music_player_t *player, size_t track_count, int delta) {
    if (!player || track_count == 0) return false;

    size_t selected = player->selected;
    if (delta < 0) {
        const size_t distance = (size_t)(-delta) % track_count;
        selected = (selected + track_count - distance) % track_count;
    } else {
        selected = (selected + (size_t)delta) % track_count;
    }
    if (selected == player->selected) return false;

    player->selected = selected;
    // Selecting a track always moves the active cursor. Keeping PLAYING means
    // the worker starts the new track at the next chunk boundary; PAUSED stays
    // paused at zero. A completed player must not claim the new track is done.
    player->active = selected;
    player->offset_bytes = 0;
    if (player->state == MUSIC_PLAYBACK_FINISHED) {
        player->state = MUSIC_PLAYBACK_STOPPED;
    }
    return true;
}

music_action_t music_player_handle_input(music_player_t *player,
                                         size_t track_count,
                                         music_input_t input) {
    if (!player || track_count == 0) return MUSIC_ACTION_NONE;

    switch (input) {
    case MUSIC_INPUT_UP:
        return music_player_select(player, track_count, -1)
             ? MUSIC_ACTION_SELECT_PREVIOUS : MUSIC_ACTION_NONE;
    case MUSIC_INPUT_DOWN:
        return music_player_select(player, track_count, 1)
             ? MUSIC_ACTION_SELECT_NEXT : MUSIC_ACTION_NONE;
    case MUSIC_INPUT_OK:
        if (player->state == MUSIC_PLAYBACK_PLAYING) {
            player->state = MUSIC_PLAYBACK_PAUSED;
            return MUSIC_ACTION_PAUSE;
        }
        if (player->state == MUSIC_PLAYBACK_PAUSED) {
            player->state = MUSIC_PLAYBACK_PLAYING;
            return MUSIC_ACTION_RESUME;
        }
        player->active = player->selected;
        player->offset_bytes = 0;
        player->state = MUSIC_PLAYBACK_PLAYING;
        return MUSIC_ACTION_START;
    case MUSIC_INPUT_STOP:
        if (player->state == MUSIC_PLAYBACK_STOPPED) return MUSIC_ACTION_NONE;
        player->state = MUSIC_PLAYBACK_STOPPED;
        player->offset_bytes = 0;
        return MUSIC_ACTION_STOP;
    default:
        return MUSIC_ACTION_NONE;
    }
}

void music_player_advance(music_player_t *player, size_t bytes) {
    if (!player || player->state != MUSIC_PLAYBACK_PLAYING) return;
    player->offset_bytes += bytes;
}

void music_player_finish(music_player_t *player) {
    if (!player) return;
    player->state = MUSIC_PLAYBACK_FINISHED;
}

unsigned music_player_progress_permille(const music_player_t *player,
                                        const music_track_t *track) {
    if (!player || !track || track->bytes == 0) return 0;
    if (player->offset_bytes >= track->bytes) return 1000;
    return (unsigned)(((uint64_t)player->offset_bytes * 1000U) / track->bytes);
}

bool music_format_duration(size_t bytes, char *out, size_t out_size) {
    if (!out || out_size == 0) return false;
    const uint64_t samples = bytes / MUSIC_PLAYER_BYTES_PER_SAMPLE;
    const uint64_t seconds = samples / MUSIC_PLAYER_SAMPLE_RATE;
    const int written = snprintf(out, out_size, "%llu:%02llu",
                                 (unsigned long long)(seconds / 60),
                                 (unsigned long long)(seconds % 60));
    return written >= 0 && (size_t)written < out_size;
}
