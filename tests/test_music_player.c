// Host tests for playback state transitions, progress, and duration formatting.
#include <assert.h>
#include <string.h>

#include "music_player.h"

static void test_initial_and_navigation_state(void) {
    music_player_t player;
    music_player_init(&player);
    assert(player.selected == 0);
    assert(player.active == 0);
    assert(player.offset_bytes == 0);
    assert(player.state == MUSIC_PLAYBACK_STOPPED);

    assert(!music_player_select(&player, 0, 1));
    assert(!music_player_select(NULL, 3, 1));

    assert(music_player_select(&player, 3, 1));
    assert(player.selected == 1);
    assert(player.active == 1);
    assert(music_player_select(&player, 3, -1));
    assert(player.selected == 0);
    assert(music_player_select(&player, 3, -1));
    assert(player.selected == 2);
    assert(!music_player_select(&player, 3, 3));
    assert(player.selected == 2);
    assert(music_player_select(&player, 3, 4));
    assert(player.selected == 0);
}

static void test_play_pause_resume_stop(void) {
    music_player_t player;
    music_player_init(&player);

    assert(music_player_handle_input(&player, 3, MUSIC_INPUT_OK) ==
           MUSIC_ACTION_START);
    assert(player.state == MUSIC_PLAYBACK_PLAYING);
    music_player_advance(&player, 2048);
    assert(player.offset_bytes == 2048);

    assert(music_player_handle_input(&player, 3, MUSIC_INPUT_OK) ==
           MUSIC_ACTION_PAUSE);
    assert(player.state == MUSIC_PLAYBACK_PAUSED);
    music_player_advance(&player, 2048);
    assert(player.offset_bytes == 2048);

    assert(music_player_handle_input(&player, 3, MUSIC_INPUT_UP) ==
           MUSIC_ACTION_SELECT_PREVIOUS);
    assert(player.state == MUSIC_PLAYBACK_PAUSED);
    assert(player.selected == 2);
    assert(player.active == 2);
    assert(player.offset_bytes == 0);

    assert(music_player_handle_input(&player, 3, MUSIC_INPUT_OK) ==
           MUSIC_ACTION_RESUME);
    assert(player.state == MUSIC_PLAYBACK_PLAYING);

    assert(music_player_handle_input(&player, 3, MUSIC_INPUT_STOP) ==
           MUSIC_ACTION_STOP);
    assert(player.state == MUSIC_PLAYBACK_STOPPED);
    assert(player.offset_bytes == 0);
    assert(music_player_handle_input(&player, 3, MUSIC_INPUT_STOP) ==
           MUSIC_ACTION_NONE);

    assert(music_player_handle_input(NULL, 3, MUSIC_INPUT_OK) ==
           MUSIC_ACTION_NONE);
}

static void test_selection_while_playing_restarts_cursor(void) {
    music_player_t player;
    music_player_init(&player);
    (void)music_player_handle_input(&player, 3, MUSIC_INPUT_OK);
    music_player_advance(&player, 4096);

    assert(music_player_handle_input(&player, 3, MUSIC_INPUT_DOWN) ==
           MUSIC_ACTION_SELECT_NEXT);
    assert(player.state == MUSIC_PLAYBACK_PLAYING);
    assert(player.selected == 1);
    assert(player.active == 1);
    assert(player.offset_bytes == 0);
}

static void test_finish_replay_and_selection_clear_finished(void) {
    music_player_t player;
    music_player_init(&player);
    (void)music_player_handle_input(&player, 3, MUSIC_INPUT_OK);
    music_player_advance(&player, 8192);
    music_player_finish(&player);
    assert(player.state == MUSIC_PLAYBACK_FINISHED);
    assert(player.offset_bytes == 8192);

    assert(music_player_handle_input(&player, 3, MUSIC_INPUT_OK) ==
           MUSIC_ACTION_START);
    assert(player.offset_bytes == 0);
    assert(player.state == MUSIC_PLAYBACK_PLAYING);

    music_player_finish(&player);
    assert(music_player_handle_input(&player, 3, MUSIC_INPUT_DOWN) ==
           MUSIC_ACTION_SELECT_NEXT);
    assert(player.state == MUSIC_PLAYBACK_STOPPED);
}

static void test_progress_and_duration(void) {
    const music_track_t track = { .bytes = 32000 };   // 1 second
    music_player_t player;
    music_player_init(&player);

    assert(music_player_progress_permille(NULL, &track) == 0);
    assert(music_player_progress_permille(&player, NULL) == 0);
    assert(music_player_progress_permille(&player, &track) == 0);

    player.offset_bytes = 16000;
    assert(music_player_progress_permille(&player, &track) == 500);
    player.offset_bytes = 32000;
    assert(music_player_progress_permille(&player, &track) == 1000);
    player.offset_bytes = 64000;
    assert(music_player_progress_permille(&player, &track) == 1000);

    char text[16];
    assert(music_format_duration(0, text, sizeof(text)));
    assert(strcmp(text, "0:00") == 0);
    assert(music_format_duration(32000, text, sizeof(text)));
    assert(strcmp(text, "0:01") == 0);
    assert(music_format_duration(32000 * 65, text, sizeof(text)));
    assert(strcmp(text, "1:05") == 0);
    assert(!music_format_duration(32000, text, 4));
    assert(!music_format_duration(32000, NULL, sizeof(text)));
}

int main(void) {
    test_initial_and_navigation_state();
    test_play_pause_resume_stop();
    test_selection_while_playing_restarts_cursor();
    test_finish_replay_and_selection_clear_finished();
    test_progress_and_duration();
    return 0;
}
