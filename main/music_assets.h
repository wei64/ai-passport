// main/music_assets.h -- built-in, read-only PCM tracks.
//
// The binary files are produced by tools/generate_music_assets.py and embedded
// through ESP-IDF target_add_binary_data(), so they never enter internal RAM as
// a mutable static array.
#pragma once

#include "music_player.h"

// Immutable metadata and Flash-backed PCM for each built-in track.
const music_track_t *music_assets_tracks(void);
size_t music_assets_count(void);
