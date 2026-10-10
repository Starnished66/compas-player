#!/bin/sh
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
manager=$repo/firmware/overlay/usr/bin/compas-zram
tmp=$(mktemp -d "${TMPDIR:-/tmp}/compas-zram-test.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

fail() { echo "FAIL: $*" >&2; exit 1; }
assert_eq() { [ "$1" = "$2" ] || fail "$3 (expected '$2', got '$1')"; }

setup_case() {
    case_root=$tmp/$1
    mkdir -p "$case_root/sys/block/zram0" "$case_root/bin"
    : > "$case_root/sys/block/zram0/disksize"
    : > "$case_root/sys/block/zram0/reset"
    printf 'lzo [lz4]\n' > "$case_root/sys/block/zram0/comp_algorithm"
    printf '0\n' > "$case_root/sys/block/zram0/disksize"
    printf '60\n' > "$case_root/swappiness"
    : > "$case_root/dev-zram0"
    printf 'Filename Type Size Used Priority\n' > "$case_root/swaps"
    cat > "$case_root/bin/mkswap" <<'EOF'
#!/bin/sh
echo mkswap >> "$EVENTS"
if [ "${FAIL_MKSWAP:-0}" = 1 ]; then
    [ "${FAIL_MKSWAP_ACTIVE:-0}" = 1 ] && echo "$DEVICE partition 16380 1 100" >> "$SWAPS"
    exit 1
fi
exit 0
EOF
    cat > "$case_root/bin/swapon" <<'EOF'
#!/bin/sh
echo "swapon $*" >> "$EVENTS"
if [ "${FAIL_SWAPON:-0}" = 1 ]; then
    [ "${FAIL_SWAPON_ACTIVE:-0}" = 1 ] && echo "$DEVICE partition 16380 1 100" >> "$SWAPS"
    exit 1
fi
echo "$DEVICE partition 16380 0 -1" >> "$SWAPS"
exit 0
EOF
    cat > "$case_root/bin/cat" <<'EOF'
#!/bin/sh
case $1 in
    */comp_algorithm)
        if [ -n "${MOCK_SELECTED_COMP_ALGORITHM:-}" ]; then
            printf '%s\n' "$MOCK_SELECTED_COMP_ALGORITHM"
        else
            /bin/cat "$1"
        fi
        ;;
    *) exec /bin/cat "$@" ;;
esac
EOF
    chmod +x "$case_root/bin/mkswap" "$case_root/bin/swapon" "$case_root/bin/cat"
}

run_manager() {
    EVENTS=$case_root/events SWAPS=$case_root/swaps DEVICE=$case_root/dev/zram0 \
        MOCK_SELECTED_COMP_ALGORITHM=${MOCK_SELECTED_COMP_ALGORITHM:-[lz4]} \
        COMPAS_ZRAM_SYS_BLOCK=$case_root/sys/block \
        COMPAS_ZRAM_PROC_SWAPS=$case_root/swaps \
        COMPAS_ZRAM_SWAPPINESS=$case_root/swappiness \
        COMPAS_ZRAM_DEVICE=$case_root/dev/zram0 \
        PATH=$case_root/bin:/usr/bin:/bin \
        "$manager" "$1"
}

setup_case success
mkdir -p "$case_root/dev"
touch "$case_root/dev/zram0"
run_manager start || fail 'normal start failed'
assert_eq "$(cat "$case_root/sys/block/zram0/comp_algorithm")" lz4 'LZ4 selected'
assert_eq "$(cat "$case_root/sys/block/zram0/disksize")" 25165824 'logical size'
assert_eq "$(cat "$case_root/swappiness")" 100 'swappiness after activation'
assert_eq "$(cat "$case_root/events")" "mkswap
swapon $case_root/dev/zram0" 'initialization order and swapon arguments'
run_manager start || fail 'idempotent start failed'
[ "$(wc -l < "$case_root/events")" -eq 2 ] || fail 'active device was initialized twice'
printf '40\n' > "$case_root/swappiness"
run_manager start || fail 'active start could not restore swappiness'
assert_eq "$(cat "$case_root/swappiness")" 100 'idempotent active start swappiness'

setup_case no_lz4
mkdir -p "$case_root/dev"; touch "$case_root/dev/zram0"
printf 'lzo zstd\n' > "$case_root/sys/block/zram0/comp_algorithm"
if run_manager start; then fail 'start succeeded without LZ4'; fi
[ ! -s "$case_root/events" ] || fail 'commands ran without LZ4'
assert_eq "$(cat "$case_root/sys/block/zram0/disksize")" 0 'unsupported device left uninitialized'
assert_eq "$(cat "$case_root/swappiness")" 60 'unsupported device left swappiness unchanged'

setup_case unconfirmed_lz4
mkdir -p "$case_root/dev"; touch "$case_root/dev/zram0"
printf 'lzo lz4\n' > "$case_root/sys/block/zram0/comp_algorithm"
if MOCK_SELECTED_COMP_ALGORITHM='[lzo] lz4' run_manager start; then fail 'unselected LZ4 was accepted'; fi
[ ! -s "$case_root/events" ] || fail 'commands ran without confirmed LZ4 selection'
assert_eq "$(cat "$case_root/swappiness")" 60 'unconfirmed LZ4 left swappiness unchanged'

setup_case missing_device
if run_manager start; then fail 'start succeeded without zram device'; fi
[ ! -s "$case_root/events" ] || fail 'commands ran without device'
assert_eq "$(cat "$case_root/swappiness")" 60 'missing device left swappiness unchanged'

setup_case busy_device
mkdir -p "$case_root/dev"; touch "$case_root/dev/zram0"
printf '8192\n' > "$case_root/sys/block/zram0/disksize"
if run_manager start; then fail 'inactive initialized device was accepted'; fi
assert_eq "$(cat "$case_root/sys/block/zram0/disksize")" 8192 'other initialized device preserved'
[ ! -s "$case_root/events" ] || fail 'commands ran on another initialized device'

setup_case already_active
mkdir -p "$case_root/dev"; touch "$case_root/dev/zram0"
printf '%s partition 16380 1 100\n' "$case_root/dev/zram0" >> "$case_root/swaps"
run_manager start || fail 'already active device rejected'
[ ! -s "$case_root/events" ] || fail 'already active device was reformatted'

setup_case mkswap_failure
mkdir -p "$case_root/dev"; touch "$case_root/dev/zram0"
if FAIL_MKSWAP=1 run_manager start; then fail 'mkswap failure reported success'; fi
assert_eq "$(cat "$case_root/sys/block/zram0/reset")" 1 'mkswap failure reset owned device'
assert_eq "$(cat "$case_root/swappiness")" 60 'mkswap failure left swappiness unchanged'

setup_case mkswap_failure_active
mkdir -p "$case_root/dev"; touch "$case_root/dev/zram0"
if FAIL_MKSWAP=1 FAIL_MKSWAP_ACTIVE=1 run_manager start; then fail 'active mkswap failure reported success'; fi
assert_eq "$(cat "$case_root/sys/block/zram0/reset")" '' 'active device was not reset after mkswap failure'

setup_case swapon_failure
mkdir -p "$case_root/dev"; touch "$case_root/dev/zram0"
if FAIL_SWAPON=1 run_manager start; then fail 'swapon failure reported success'; fi
assert_eq "$(cat "$case_root/sys/block/zram0/reset")" 1 'swapon failure reset owned device'
assert_eq "$(cat "$case_root/swappiness")" 60 'swapon failure left swappiness unchanged'

setup_case swapon_failure_active
mkdir -p "$case_root/dev"; touch "$case_root/dev/zram0"
if FAIL_SWAPON=1 FAIL_SWAPON_ACTIVE=1 run_manager start; then fail 'active swapon failure reported success'; fi
assert_eq "$(cat "$case_root/sys/block/zram0/reset")" '' 'active device was not reset after swapon failure'

setup_case stop
mkdir -p "$case_root/dev"; touch "$case_root/dev/zram0"
printf '%s partition 16380 1 100\n' "$case_root/dev/zram0" >> "$case_root/swaps"
run_manager stop || fail 'stop failed'
assert_eq "$(wc -l < "$case_root/swaps")" 2 'stop retained swap'

echo 'compas-zram host tests passed'
