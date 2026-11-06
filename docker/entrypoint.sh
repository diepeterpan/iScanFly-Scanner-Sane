#!/bin/bash
set -euo pipefail

# ==============================================================================
# entrypoint.sh - Starts D-Bus, Avahi daemon, and AirSane for iScanFly
# ==============================================================================

mdns_announce="${MDNS_ANNOUNCE:-true}"
airsane_debug="${AIRSANE_DEBUG:-false}"
airsane_access_log="${AIRSANE_ACCESS_LOG:-}"
airsane_listen_port="${AIRSANE_LISTEN_PORT:-8090}"
announce_secure="${ANNOUNCE_SECURE:-false}"
announce_base_url="${ANNOUNCE_BASE_URL:-}"
scanner_ip="${SCANNER_IP:-192.168.18.33}"
iscanfly_log_level="${ISCANFLY_LOG_LEVEL:-warn}"
iscanfly_max_bad_lines="${ISCANFLY_MAX_BAD_LINES:-2}"
export ISCANFLY_LOG_LEVEL="$iscanfly_log_level"
export ISCANFLY_MAX_BAD_LINES="$iscanfly_max_bad_lines"

echo "=== Starting iScanFly AirSane (eSCL) Server ==="

# 1. Update /etc/sane.d/iscanfly.conf if SCANNER_IP is set
if [ -n "$scanner_ip" ]; then
    echo "[entrypoint] Setting scanner address in /etc/sane.d/iscanfly.conf: $scanner_ip (log_level: $iscanfly_log_level, max_bad_lines: $iscanfly_max_bad_lines)"
    {
        echo "log_level $iscanfly_log_level"
        echo "max_bad_lines $iscanfly_max_bad_lines"
        echo "$scanner_ip"
    } > /etc/sane.d/iscanfly.conf
fi

# 2. Start D-Bus system daemon
mkdir -p /run/dbus
rm -f /run/dbus/pid /run/dbus/system_bus_socket
dbus-daemon --system --fork

# 3. Start Avahi daemon if mDNS announce is requested
if [ "$mdns_announce" = "true" ]; then
    echo "[entrypoint] Starting Avahi daemon for mDNS/Bonjour discovery..."
    mkdir -p /run/avahi-daemon
    rm -f /run/avahi-daemon/pid

    # Configure Avahi hostname if supplied
    if [ -n "${AVAHI_HOST_NAME:-}" ]; then
        sed -i "s/^#*host-name=.*/host-name=${AVAHI_HOST_NAME}/" /etc/avahi/avahi-daemon.conf
    fi

    avahi-daemon -D
    sleep 1
fi

# 4. Start Wi-Fi connector in background if enabled
wifi_pid=
if [ "${WIFI_ENABLED:-false}" = "true" ]; then
    echo "[entrypoint] Launching RT5370 Wi-Fi Client manager..."
    /usr/local/bin/wifi-connector.sh &
    wifi_pid=$!
fi

cleanup() {
    echo "[entrypoint] Caught signal, shutting down..."
    if [ -n "$wifi_pid" ]; then
        kill -TERM "$wifi_pid" 2>/dev/null || true
    fi
    if [ -f /run/avahi-daemon/pid ]; then
        kill -TERM "$(cat /run/avahi-daemon/pid)" 2>/dev/null || true
    fi
    exit 0
}

trap cleanup SIGINT SIGTERM

echo "[entrypoint] Probing SANE scanner backend:"
scanimage -L || true

echo "[entrypoint] Executing airsaned on port $airsane_listen_port (debug=$airsane_debug)..."

# Execute airsaned directly as user saned
airsane_args=(
    "--listen-port=$airsane_listen_port"
    "--mdns-announce=$mdns_announce"
    "--announce-secure=$announce_secure"
    "--announce-base-url=$announce_base_url"
    "--debug=$airsane_debug"
)

if [ -n "$airsane_access_log" ]; then
    airsane_args+=("--access-log=$airsane_access_log")
fi

exec runuser -u saned -- /usr/local/bin/airsaned "${airsane_args[@]}"
