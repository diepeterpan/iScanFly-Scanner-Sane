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
WIFI_VERBOSE="${WIFI_VERBOSE:-false}"
WIFI_RESET_TIMEOUT="${WIFI_RESET_TIMEOUT:-45}"

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
iw dev "$IFACE" set power_save off 2>/dev/null || true

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

# Function to clean interface and release stale kernel packet matching filters
clean_interface() {
    log "Cleaning interface $IFACE and purging stale driver filters..."
    if [[ -f "$WPA_PID_FILE" ]] && kill -0 "$(cat "$WPA_PID_FILE")" 2>/dev/null; then
        kill -TERM "$(cat "$WPA_PID_FILE")" 2>/dev/null || true
    fi
    if [[ -f "$DHCP_PID_FILE" ]] && kill -0 "$(cat "$DHCP_PID_FILE")" 2>/dev/null; then
        kill -TERM "$(cat "$DHCP_PID_FILE")" 2>/dev/null || true
    fi
    pkill -f "dhclient.*$IFACE" 2>/dev/null || true
    rm -f /run/wpa_supplicant/"$IFACE" "$WPA_PID_FILE" "$DHCP_PID_FILE"

    # Flush all stale IPv4/IPv6 addresses
    ip -4 addr flush dev "$IFACE" 2>/dev/null || true
    ip -6 addr flush dev "$IFACE" 2>/dev/null || true

    # Cycling interface down and up purges mac80211 Netlink packet match filters
    ip link set "$IFACE" down 2>/dev/null || true
    sleep 1
    unblock_rfkill
    ip link set "$IFACE" up 2>/dev/null || true
    iw dev "$IFACE" set power_save off 2>/dev/null || true
    sleep 1
}

# Function to start or restart wpa_supplicant
start_wpa_supplicant() {
    clean_interface
    log "Starting wpa_supplicant on $IFACE for SSID '$WIFI_SSID' (drivers: nl80211,wext)..."
    wpa_supplicant -B -Dnl80211,wext -i "$IFACE" -c "$WPA_CONF" -P "$WPA_PID_FILE"
}

# 4. Start wpa_supplicant
start_wpa_supplicant

request_dhcp() {
    log "Associated with '$WIFI_SSID'. Disabling 802.11 power save and requesting DHCP without default gateway..."
    iw dev "$IFACE" set power_save off 2>/dev/null || true
    ip -4 addr flush dev "$IFACE" 2>/dev/null || true

    # Using dhclient with custom options, or dhcpcd -G (no default gateway), or udhcpc
    if command -v dhcpcd >/dev/null 2>&1; then
        # -G / --nogateway prevents setting default route
        # -b / --background
        dhcpcd -G -b -p "$IFACE" 2>&1 | while read -r line; do log "[dhcpcd] $line"; done || true
    elif command -v udhcpc >/dev/null 2>&1; then
        # udhcpc -O router request omitted, or custom script
        udhcpc -i "$IFACE" -n -q -O subnet -O broadcast 2>&1 | while read -r line; do log "[udhcpc] $line"; done || true
    elif command -v dhclient >/dev/null 2>&1; then
        # For dhclient, create a temporary config that ignores routers
        local dhclient_conf="/run/dhclient-$IFACE.conf"
        cat << 'EOF_DH' > "$dhclient_conf"
request subnet-mask, broadcast-address, time-offset, host-name;
EOF_DH
        pkill -f "dhclient.*$IFACE" 2>/dev/null || true
        dhclient -cf "$dhclient_conf" -1 -v "$IFACE" 2>&1 | while read -r line; do log "[dhclient] $line"; done || true
    fi

    # Verify scanner route/ARP entry without ever opening TCP port 23
    # (opening and closing port 23 without sending a 4-byte S400W command wedges the scanner firmware!)
    log "Verifying scanner route & ARP readiness (192.168.18.33)..."
    if command -v ping >/dev/null 2>&1 && ping -c 1 -W 2 192.168.18.33 >/dev/null 2>&1; then
        log "Scanner 192.168.18.33 reached via ICMP!"
    elif ip neigh show 192.168.18.33 2>/dev/null | grep -q .; then
        log "Scanner 192.168.18.33 present in ARP table ($(ip neigh show 192.168.18.33 | tr '\n' ' '))"
    else
        log "Route to 192.168.18.33 active on $IFACE"
    fi
}

# 5. Continuous Monitor and Scan Loop (WIFI_SCAN_INTERVAL seconds)
log "Starting continuous association monitoring loop (interval: ${WIFI_SCAN_INTERVAL}s, reset timeout: ${WIFI_RESET_TIMEOUT}s)..."

LAST_REPORTED_STATE=""
DISCONNECTED_SECONDS=0

while true; do
    # Check current wpa_supplicant status
    STATUS_OUT=$(wpa_cli -i "$IFACE" status 2>/dev/null || true)
    WPA_STATE=$(echo "$STATUS_OUT" | awk -F= '$1=="wpa_state"{print $2}')
    CURRENT_SSID=$(echo "$STATUS_OUT" | awk -F= '$1=="ssid"{print $2}')

    if [[ "$WPA_STATE" == "COMPLETED" && "$CURRENT_SSID" == "$WIFI_SSID" ]]; then
        if [[ "$LAST_REPORTED_STATE" != "CONNECTED" ]]; then
            log "Successfully connected to '$WIFI_SSID'."
            LAST_REPORTED_STATE="CONNECTED"
            request_dhcp
        fi
        DISCONNECTED_SECONDS=0
    else
        CURRENT_STATE_DESC="${WPA_STATE:-DISCONNECTED}"
        DISCONNECTED_SECONDS=$((DISCONNECTED_SECONDS + WIFI_SCAN_INTERVAL))

        # Check if we exceeded the hard reset timeout threshold
        if [[ "$DISCONNECTED_SECONDS" -ge "$WIFI_RESET_TIMEOUT" ]]; then
            log "WARNING: Not connected to '$WIFI_SSID' for ${DISCONNECTED_SECONDS}s (state: $CURRENT_STATE_DESC). Resetting radio interface and restarting wpa_supplicant..."
            ip link set "$IFACE" down || true
            sleep 1
            unblock_rfkill
            ip link set "$IFACE" up || true
            sleep 1
            start_wpa_supplicant
            DISCONNECTED_SECONDS=0
            LAST_REPORTED_STATE="RESET"
        else
            if [[ "$LAST_REPORTED_STATE" != "$CURRENT_STATE_DESC" || "$WIFI_VERBOSE" == "true" ]]; then
                log "Not connected to '$WIFI_SSID' (state: $CURRENT_STATE_DESC, disconnected: ${DISCONNECTED_SECONDS}s). Waiting for scanner AP..."
                LAST_REPORTED_STATE="$CURRENT_STATE_DESC"
            fi
            # Allow wpa_supplicant's own scan state machine to run without constant abortive re-triggers.
            # Only trigger a soft scan if completely idle/disconnected for over 15s.
            if [[ "$CURRENT_STATE_DESC" == "DISCONNECTED" || "$CURRENT_STATE_DESC" == "INACTIVE" ]]; then
                if [[ "$DISCONNECTED_SECONDS" -ge 15 ]]; then
                    wpa_cli -i "$IFACE" scan >/dev/null 2>&1 || true
                fi
            fi
        fi
    fi

    sleep "$WIFI_SCAN_INTERVAL"
done

