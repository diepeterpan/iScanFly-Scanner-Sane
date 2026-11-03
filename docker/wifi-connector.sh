#!/bin/bash
set -euo pipefail

# ==============================================================================
# wifi-connector.sh - Background RT5370 Wi-Fi Client Manager
# Continuously scans and connects to a configured AP via wpa_supplicant & DHCP.
# Prevents overwriting host default gateway.
# ==============================================================================

WIFI_SSID="${WIFI_SSID:-iScanAir}"
WIFI_PASSWORD="${WIFI_PASSWORD:-}"
WIFI_INTERFACE="${WIFI_INTERFACE:-}"
WIFI_SCAN_INTERVAL="${WIFI_SCAN_INTERVAL:-5}"

WPA_CONF="/run/wpa_supplicant.conf"
WPA_PID_FILE="/run/wpa_supplicant.pid"
DHCP_PID_FILE="/run/dhcpcd.pid"

log() {
    echo "[wifi-connector] $*" >&2
}

# 1. Unblock any software rfkill blocks
unblock_rfkill() {
    if command -v rfkill >/dev/null 2>&1; then
        rfkill unblock wifi 2>/dev/null || true
        rfkill unblock all 2>/dev/null || true
    fi
}
unblock_rfkill

# 2. Discover wireless interface if not explicitly defined
find_interface() {
    if [[ -n "$WIFI_INTERFACE" && -d "/sys/class/net/$WIFI_INTERFACE" ]]; then
        echo "$WIFI_INTERFACE"
        return 0
    fi

    # Try detecting wireless interfaces via /sys/class/net/*/wireless or phy80211
    for iface_path in /sys/class/net/*; do
        if [[ -d "$iface_path/wireless" || -d "$iface_path/phy80211" ]]; then
            basename "$iface_path"
            return 0
        fi
    done

    # Fallback checking via iw
    if command -v iw >/dev/null 2>&1; then
        local iw_iface
        iw_iface=$(iw dev 2>/dev/null | awk '$1=="Interface"{print $2; exit}')
        if [[ -n "$iw_iface" ]]; then
            echo "$iw_iface"
            return 0
        fi
    fi

    return 1
}

cleanup() {
    log "Stopping Wi-Fi connector..."
    if [[ -f "$WPA_PID_FILE" ]] && kill -0 "$(cat "$WPA_PID_FILE")" 2>/dev/null; then
        kill -TERM "$(cat "$WPA_PID_FILE")" 2>/dev/null || true
    fi
    if [[ -f "$DHCP_PID_FILE" ]] && kill -0 "$(cat "$DHCP_PID_FILE")" 2>/dev/null; then
        kill -TERM "$(cat "$DHCP_PID_FILE")" 2>/dev/null || true
    fi
    exit 0
}

trap cleanup SIGINT SIGTERM

log "Waiting for wireless interface (RT5370)..."
IFACE=""
while [[ -z "$IFACE" ]]; do
    if IFACE=$(find_interface); then
        break
    fi
    sleep 2
done

log "Using wireless interface: $IFACE"

# Unblock radio and bring interface UP
unblock_rfkill
if ! ip link set "$IFACE" up; then
    log "Warning: 'ip link set $IFACE up' failed. Retrying after rfkill unblock..."
    unblock_rfkill
    sleep 1
    ip link set "$IFACE" up || log "Notice: could not set interface UP directly, wpa_supplicant will attempt driver initialization."
fi

# 3. Generate wpa_supplicant.conf
mkdir -p /run/wpa_supplicant
cat << EOF > "$WPA_CONF"
ctrl_interface=/run/wpa_supplicant
update_config=1
ap_scan=1

network={
    ssid="$WIFI_SSID"
EOF

if [[ -n "$WIFI_PASSWORD" ]]; then
    cat << EOF >> "$WPA_CONF"
    key_mgmt=WPA-PSK
    psk="$WIFI_PASSWORD"
EOF
else
    cat << EOF >> "$WPA_CONF"
    key_mgmt=NONE
EOF
fi

cat << EOF >> "$WPA_CONF"
}
EOF

# 4. Start wpa_supplicant with nl80211 and wext fallback
rm -f /run/wpa_supplicant/"$IFACE"
log "Starting wpa_supplicant on $IFACE for SSID '$WIFI_SSID' (drivers: nl80211,wext)..."
wpa_supplicant -B -Dnl80211,wext -i "$IFACE" -c "$WPA_CONF" -P "$WPA_PID_FILE"

HAS_DHCP=0

request_dhcp() {
    log "Associated with '$WIFI_SSID'. Requesting DHCP without default gateway..."
    # Using dhclient with custom options, or dhcpcd -G (no default gateway), or udhcpc
    if command -v dhcpcd >/dev/null 2>&1; then
        # -G / --nogateway prevents setting default route
        # -b / --background
        dhcpcd -G -b -p "$IFACE" 2>&1 | while read -r line; do log "[dhcpcd] $line"; done || true
        HAS_DHCP=1
    elif command -v udhcpc >/dev/null 2>&1; then
        # udhcpc -O router request omitted, or custom script
        udhcpc -i "$IFACE" -n -q -O subnet -O broadcast 2>&1 | while read -r line; do log "[udhcpc] $line"; done || true
        HAS_DHCP=1
    elif command -v dhclient >/dev/null 2>&1; then
        # For dhclient, create a temporary config that ignores routers
        local dhclient_conf="/run/dhclient-$IFACE.conf"
        cat << 'EOF_DH' > "$dhclient_conf"
request subnet-mask, broadcast-address, time-offset, host-name;
EOF_DH
        dhclient -cf "$dhclient_conf" -1 -v "$IFACE" 2>&1 | while read -r line; do log "[dhclient] $line"; done || true
        HAS_DHCP=1
    fi
}

# 5. Continuous Monitor and Scan Loop (WIFI_SCAN_INTERVAL seconds)
log "Starting continuous association monitoring loop (interval: ${WIFI_SCAN_INTERVAL}s)..."

while true; do
    # Check current wpa_supplicant status
    STATUS_OUT=$(wpa_cli -i "$IFACE" status 2>/dev/null || true)
    WPA_STATE=$(echo "$STATUS_OUT" | awk -F= '$1=="wpa_state"{print $2}')
    CURRENT_SSID=$(echo "$STATUS_OUT" | awk -F= '$1=="ssid"{print $2}')

    if [[ "$WPA_STATE" == "COMPLETED" && "$CURRENT_SSID" == "$WIFI_SSID" ]]; then
        # Check if interface has an IPv4 address assigned
        IP_ASSIGNED=$(ip -4 addr show dev "$IFACE" | grep -o 'inet [0-9.]*' | awk '{print $2}' || true)
        if [[ -z "$IP_ASSIGNED" ]]; then
            request_dhcp
        fi
    else
        log "Not connected to '$WIFI_SSID' (state: ${WPA_STATE:-DISCONNECTED}). Triggering scan & reconnect..."
        HAS_DHCP=0
        wpa_cli -i "$IFACE" scan >/dev/null 2>&1 || true
        wpa_cli -i "$IFACE" reconnect >/dev/null 2>&1 || true
    fi

    sleep "$WIFI_SCAN_INTERVAL"
done

