#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../../.." && pwd)
out="$repo/build/host-tests/test_player_dac_transport"
mkdir -p "$(dirname "$out")"
"${CC:-cc}" -std=gnu11 -DHOST_BUILD -O0 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
    -I"$repo" -I"$repo/lvgl" "$repo/src/ui/player/transport/tests/test_player_dac_transport.c" \
    "$repo/src/ui/player/transport/player_playback.c" "$repo/src/ui/player/queue/player_queue_remote.c" \
    -Wl,--gc-sections -Wl,--wrap=play_track_at -Wl,--wrap=gui_player_handle_auto_advance -o "$out"
"$out"

bash "$repo/src/ui/player/transport/tests/test_bt_dac_session.sh"
bash "$repo/src/network/bluetooth/control/tests/test_bt_dac_route.sh"
