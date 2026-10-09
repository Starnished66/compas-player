#!/bin/sh
# COMPAS_WIFI_ON_WRAPPER

MAC_MANAGER=${COMPAS_WIFI_MAC_MANAGER:-/usr/bin/compas-wifi-mac}
VENDOR_WIFI_ON=${COMPAS_WIFI_VENDOR_WIFI_ON:-/usr/libexec/compas/wifi_on.vendor.sh}

if ! "$MAC_MANAGER" wait "${COMPAS_WIFI_MAC_TIMEOUT:-5}"; then
    echo "WiFi MAC is not ready; leaving wlan0 down" >&2
    exit 1
fi

if ! "$MAC_MANAGER" connection-begin "$$"; then
    echo "WiFi setup is already active or MAC readiness was lost" >&2
    exit 1
fi

release_connection() {
    connection_status=$?
    "$MAC_MANAGER" connection-end "$$" "$connection_status" >/dev/null 2>&1 || :
}
trap release_connection 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

if ! "$MAC_MANAGER" connection-check "$$"; then
    echo "WiFi shutdown or MAC change interrupted setup" >&2
    exit 1
fi

COMPAS_WIFI_CONNECTION_PID=$$
export COMPAS_WIFI_CONNECTION_PID
. "$VENDOR_WIFI_ON"
vendor_status=$?
exit "$vendor_status"
