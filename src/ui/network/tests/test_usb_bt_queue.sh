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
    -c "$repo/src/ui/network/tests/test_usb_bt_queue.c" -o "$runtime/test.o"
g++ \
    "$runtime/test.o" "$runtime/main.o" "${objs[@]}" \
    "$repo/build_host/libwavpack/libwavpack.a" "$repo/build_host/libsndfile/libsndfile.a" \
    -Wl,--wrap=settings_save_async \
    -Wl,--wrap=show_info_toast \
    -Wl,--wrap=show_error_toast \
    -Wl,--wrap=gui_shell_refresh_quick_drawer_expansion_toggles \
    -Wl,--wrap=gui_library_suspend_boot_prompt \
    -Wl,--wrap=gui_library_scan_active \
    -Wl,--wrap=firmware_update_busy \
    -Wl,--wrap=usb_mode_control_host_connected \
    -Wl,--wrap=usb_mode_control_storage_host_configured \
    -Wl,--wrap=usb_mode_control_detect_current \
    -Wl,--wrap=usb_mode_control_apply_with_status \
    -Wl,--wrap=gui_shell_bt_dac_busy \
    -Wl,--wrap=start_bt_apply_output_settings \
    -Wl,--wrap=gui_network_extend_bt_pairing_window \
    $(sdl2-config --libs) -lpthread -lm -ldl -o "$runtime/test"
cd "$runtime"
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./test
