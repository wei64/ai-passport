# The standalone player replaces the baseline demo menu. Keep only the BSP and
# application modules needed for this firmware; unused demo sources are not built.
idf_component_register(
    SRCS "main.c"
         "music_player.c"
         "music_assets.c"
         "music_player_app.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_timer
)

# Embed the checked-in PCM assets as read-only Flash data. CMake creates the
# assembler symbols from each binary, so no multi-megabyte C hex array or
# runtime decoder is needed.
target_add_binary_data(${COMPONENT_LIB}
    "${CMAKE_CURRENT_LIST_DIR}/../assets/music/pcm/morning_postcard.pcm"
    BINARY RENAME_TO music_morning_postcard)
target_add_binary_data(${COMPONENT_LIB}
    "${CMAKE_CURRENT_LIST_DIR}/../assets/music/pcm/blue_hour.pcm"
    BINARY RENAME_TO music_blue_hour)
target_add_binary_data(${COMPONENT_LIB}
    "${CMAKE_CURRENT_LIST_DIR}/../assets/music/pcm/little_engine.pcm"
    BINARY RENAME_TO music_little_engine)
