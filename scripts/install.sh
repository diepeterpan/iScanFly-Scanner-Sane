#!/bin/bash
set -e

# SANE Backend Installer for Mustek S400W / iScanFly
echo "=== Installing SANE iscanfly backend ==="

if [ "$EUID" -ne 0 ]; then
  echo "Error: Please run as root (e.g. using sudo ./install.sh)" >&2
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Detect SANE library directory
SANEDIR=""
if [ -d "/usr/lib/x86_64-linux-gnu/sane" ]; then
    SANEDIR="/usr/lib/x86_64-linux-gnu/sane"
elif [ -d "/usr/lib64/sane" ]; then
    SANEDIR="/usr/lib64/sane"
elif [ -d "/usr/lib/sane" ]; then
    SANEDIR="/usr/lib/sane"
elif [ -d "/usr/lib/aarch64-linux-gnu/sane" ]; then
    SANEDIR="/usr/lib/aarch64-linux-gnu/sane"
elif [ -d "/usr/lib/arm-linux-gnueabihf/sane" ]; then
    SANEDIR="/usr/lib/arm-linux-gnueabihf/sane"
else
    echo "Warning: Could not detect default SANE backend directory. Defaulting to /usr/lib/sane"
    SANEDIR="/usr/lib/sane"
    mkdir -p "$SANEDIR"
fi

echo "Installing shared libraries to $SANEDIR..."
cp -a "$SCRIPT_DIR"/libsane-iscanfly.so.1.0.0 "$SANEDIR/"
# Ensure correct symlinks exist on target even if scp created duplicates or copied dereferenced files
cd "$SANEDIR"
ln -sf libsane-iscanfly.so.1.0.0 libsane-iscanfly.so.1
ln -sf libsane-iscanfly.so.1 libsane-iscanfly.so
chmod 755 libsane-iscanfly.so.1.0.0
cd "$SCRIPT_DIR"

# Install config file to /etc/sane.d/
mkdir -p /etc/sane.d
if [ ! -f /etc/sane.d/iscanfly.conf ]; then
    echo "Installing default configuration to /etc/sane.d/iscanfly.conf..."
    cp "$SCRIPT_DIR/iscanfly.conf" /etc/sane.d/
    chmod 644 /etc/sane.d/iscanfly.conf
else
    echo "Notice: /etc/sane.d/iscanfly.conf already exists, not overwriting."
fi

# Enable backend in SANE dll configuration
if [ -d /etc/sane.d/dll.d ]; then
    echo "Enabling backend in /etc/sane.d/dll.d/iscanfly..."
    echo "iscanfly" > /etc/sane.d/dll.d/iscanfly
    chmod 644 /etc/sane.d/dll.d/iscanfly
elif [ -f /etc/sane.d/dll.conf ]; then
    if ! grep -q "^iscanfly$" /etc/sane.d/dll.conf; then
        echo "Appending iscanfly to /etc/sane.d/dll.conf..."
        echo "iscanfly" >> /etc/sane.d/dll.conf
    fi
fi

ldconfig

echo ""
echo "=== Installation complete! ==="
echo "You can check device status with:"
echo "    scanimage -L"
echo "Or scan an image with:"
echo "    scanimage -d iscanfly --format=jpeg > test.jpg"

