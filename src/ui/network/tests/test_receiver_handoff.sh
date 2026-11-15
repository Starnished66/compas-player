#!/bin/bash
set -euo pipefail
repo="$(cd "$(dirname "$0")/../../../.." && pwd)"
runtime="$(mktemp -d "${TMPDIR:-/tmp}/dac_screens.XXXXXX")"
trap 'rm -rf "$runtime"' EXIT
[[ -f "$repo/build_host/app/main.o" ]] || { echo 'run make host first' >&2; exit 1; }
mkdir -p "$runtime/.compas" "$runtime/music/.plugins" "$runtime/scripts/plugins"
printf 'setup_complete=1\n' > "$runtime/.compas/settings.txt"
ln -s "${COMPAS_HOST_ASSETS:?set COMPAS_HOST_ASSETS to existing firmware assets}" "$runtime/assets"
objcopy --redefine-sym main=compas_original_main "$repo/build_host/app/main.o" "$runtime/main.o"
mapfile -t objs < <(find "$repo/build_host" -name '*.o' ! -path '*/app/main.o' ! -path '*/CMakeFiles/*' -print | sort)
gcc -std=c11 -D_DEFAULT_SOURCE -DHOST_BUILD=1 -I"$repo" -I"$repo/lvgl/src" -I"$repo/cJSON" \
    -c "$repo/src/ui/network/tests/test_receiver_handoff.c" -o "$runtime/test.o"
g++ \
    "$runtime/test.o" "$runtime/main.o" "${objs[@]}" \
    "$repo/build_host/libwavpack/libwavpack.a" "$repo/build_host/libsndfile/libsndfile.a" \
    -Wl,--wrap=wifi_feature_guard,--wrap=settings_save_async,--wrap=gui_shell_usb_dac_owns_output \
    -Wl,--wrap=gui_network_usb_dac_pending,--wrap=disable_usb_dac_mode,--wrap=gui_shell_bt_dac_busy \
    -Wl,--wrap=start_bt_apply_output_settings,--wrap=gui_network_extend_bt_pairing_window \
    -Wl,--wrap=airplay_control_start,--wrap=airplay_control_stop,--wrap=airplay_bridge_is_stopped \
    -Wl,--wrap=airplay_bridge_has_active_stream,--wrap=gui_shell_refresh_quick_drawer_expansion_toggles,--wrap=show_error_toast \
    -Wl,--wrap=seed_track_metadata,--wrap=prepare_track_metadata,--wrap=clear_selection_metadata_seed \
    -Wl,--wrap=track_metadata_count_play,--wrap=request_track_audio_metadata,--wrap=arm_next_track_for_audio \
    -Wl,--wrap=set_play_button_state,--wrap=gui_player_get_screen,--wrap=audio_get_resume_position_seconds \
    -Wl,--wrap=audio_play_file_at_with_replaygain_mode \
    -Wl,--wrap=add_pill_toggle_row,--wrap=airplay_control_is_active,--wrap=audio_is_playing,--wrap=audio_is_paused \
    -Wl,--wrap=audio_toggle_pause,--wrap=settings_save,--wrap=gui_player_queue_checkpoint_urgent \
    $(sdl2-config --libs) -lpthread -lm -ldl -o "$runtime/test"
cd "$runtime"
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./test
