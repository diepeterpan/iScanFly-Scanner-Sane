# iScanFly / Mustek S400W (iScan Air) SANE Backend Driver

A Linux [SANE](http://www.sane-project.org/) (Scanner Access Now Easy) backend driver for the **Mustek S400W iScan Air** (also branded as **iScanFly**) Wi-Fi sheet-fed scanner.

This driver allows standard Linux scanning utilities (such as `scanimage`, GNOME Document Scanner / Simple Scan, XSane, and OCR suites) to communicate with the scanner over Wi-Fi.

---

## Features

- **Network Interface**: Communicates directly over TCP (default port `23`).
- **Scan Modes**:
  - `Color`: 24-bit RGB (decompressed from the scanner's native JPEG stream).
  - `Gray`: 8-bit Grayscale (integer-weighted luminance calculation: $Y = 0.299R + 0.587G + 0.114B$).
  - `Lineart`: 1-bit Black & White text mode with adaptive Bradley-Roth local thresholding or manual thresholding (8 pixels per byte, MSB first).
- **Auto-Crop & Deskew**:
  - Automatically detects paper boundaries against the scanner backing.
  - Straightens tilted pages via high-precision bilinear interpolation (angles up to $\pm 15^\circ$).
  - Crops flush to the paper edge across all color modes and resolutions.
  - In `Lineart` mode, crop and deskew run *before* binarization, completely eliminating scanner backing black borders.
  - Enabled by default (`yes`), with configurable overrides via SANE options (`--autocrop`, `--deskew`) or `/etc/sane.d/iscanfly.conf`.
- **Resolutions**:
  - `300 dpi` (Standard hardware resolution).
  - `600 dpi` (Fine hardware resolution for firmware $\ge 26$).
  - `75 dpi` & `150 dpi` (Software-scaled in driver via `libjpeg` DCT integer downscaling with zero CPU overhead).
- **Paper Insertion Polling**: Configurable grace period (`--paper-wait-timeout`, default 15s) so the scanner waits for single sheets to be inserted rather than failing immediately.
- **Maintenance Actions**: SANE buttons to trigger calibration (`--calibrate`) and cleaning (`--clean`) cycles with corresponding maintenance sheets.
- **Sensor Telemetry**: Queries scanner state including battery level (`battery-low`) and document presence (`page-loaded`).

---

## Repository Structure

```
├── Dockerfile              # Container environment with libsane-dev & libjpeg-dev
├── Makefile                # Builds libsane-iscanfly.so shared library and dist package
├── conf/
│   └── iscanfly.conf       # SANE backend IP/port configuration
├── dist/                   # Ready-to-deploy distribution artifacts
│   ├── libsane-iscanfly.so*
│   ├── iscanfly.conf
│   └── install.sh
├── include/
│   ├── iscanfly.h          # SANE backend headers and options
│   └── s400w.h             # Core scanner protocol definitions
├── scripts/
│   ├── install.sh          # Installs backend into system SANE directories
│   └── distribute.sh       # Deploys dist/ to a remote host via SCP/SSH
├── src/
│   ├── iscanfly-sane.c     # SANE v1 API implementation & JPEG processing
│   └── s400w.c             # Low-level TCP socket protocol implementation
└── tools/
    ├── Makefile            # Standalone CLI build file
    ├── scan.sh             # Batch script example
    └── scanner.c           # Standalone CLI scanner utility
```

---

## 1. Building with Docker

Docker eliminates the need to install development libraries (`libsane-dev`, `libjpeg-dev`) directly on your host development system.

### Build the Docker Image
```bash
docker build -t iscanfly-builder .
```

### Compile the Driver and Generate `dist/`
```bash
docker run --rm -v "$(pwd)":/build -u "$(id -u):$(id -g)" iscanfly-builder make dist
```

This compiles `libsane-iscanfly.so.1.0.0` and populates the `dist/` folder with everything required for deployment.

---

## 2. Local Installation

If you are running the scanner from your local machine:

```bash
cd dist
sudo ./install.sh
```

### Configure Scanner IP & Options
Edit `/etc/sane.d/iscanfly.conf` to set the IP address of your scanner on your Wi-Fi network and default preprocessing options:

```ini
# /etc/sane.d/iscanfly.conf

# Global toggles (default: yes)
autocrop yes
deskew yes

# Scanner IP and optional port (default port: 23)
192.168.18.33
# Or with explicit port:
# 192.168.18.33:23
```

---

## 3. Remote Distribution (`scripts/distribute.sh`)

If you run a dedicated headless scanner server (such as a Raspberry Pi or home server), you can push and install the driver remotely with a single command:

```bash
# Copy dist files to remote machine
./scripts/distribute.sh pi@192.168.1.50

# Copy and immediately execute remote installation with sudo
./scripts/distribute.sh --install pi@192.168.1.50

# Custom SSH port and identity key
./scripts/distribute.sh -P 2222 -i ~/.ssh/id_ed25519 --install user@scan-server.local
```

---

## 4. Running via Docker & AirSane (eSCL / Apple AirScan Server)

For headless home servers, NAS devices, or Docker environments, you can run the scanner server using Docker and Docker Compose located in [`docker/`](file:///home/andre/Development/iScanFly-Scanner/docker).

This container bundles **SANE**, the **iScanFly driver**, and **[AirSane](https://github.com/SimulPiscator/AirSane)** (an Apple AirScan / eSCL server) with **Avahi mDNS/Bonjour discovery**. Any phone (iOS/Android), Mac, or Linux computer on your local network will automatically discover the scanner wirelessly with zero drivers required on client devices.

### Quick Start with Docker Compose
```bash
cd docker
docker compose up -d
```

### Configuration (`docker/docker-compose.yaml`)
You can configure your scanner IP, Wi-Fi parameters, and Bonjour parameters directly in the compose file or `.env`:

```yaml
services:
  airsane:
    build:
      context: ..
      dockerfile: docker/Dockerfile
    image: iscanfly-airsane:latest
    container_name: iscanfly-airsane
    privileged: true
    network_mode: host
    restart: unless-stopped
    volumes:
      - /dev/bus/usb:/dev/bus/usb          # Passthrough for USB RT5370 Wi-Fi adapter
    environment:
      - TZ=Africa/Johannesburg
      # Wireless Client Configuration (USB RT5370)
      - WIFI_ENABLED=true                  # Enable automatic Wi-Fi connection
      - WIFI_SSID=iScanAir                 # Scanner AP SSID
      - WIFI_PASSWORD=secret               # WPA/WPA2-PSK password (or empty if open AP)
      - WIFI_INTERFACE=                    # Auto-detected (or explicit e.g. wlan0)
      - WIFI_SCAN_INTERVAL=5               # Reconnection/scan interval in seconds
      - WIFI_VERBOSE=false                 # Set to true to log reconnects on every check interval
      # Scanner & AirSane Settings
      - SCANNER_IP=192.168.18.33          # IP address of the iScanFly scanner
      - MDNS_ANNOUNCE=true                # Announce via mDNS (Bonjour / Avahi)
      - AIRSANE_LISTEN_PORT=8090          # Port for AirSane Web & eSCL interface
      - AIRSANE_DEBUG=false               # Log AirSane internal security/debug events
      - AIRSANE_ACCESS_LOG=               # Set to '-' to log HTTP access requests (e.g. /eSCL/ScannerStatus)
      - AVAHI_HOST_NAME=iscanfly          # Advertised Bonjour hostname
      - AVAHI_DOMAIN=local
```

### USB RT5370 Wi-Fi Client Features & Host Prerequisites

#### ⚠️ Host Prerequisite 1: Wireless Firmware
Because the Linux kernel driver (`rt2800usb`) executes in the **host kernel**, the host system must have the Ralink / MediaTek firmware package installed. The container does not and cannot inject kernel firmware into the host kernel.

Install the firmware package on your host Linux system using your package manager (ensure `non-free-firmware` / `non-free` repositories are enabled):

```bash
# Debian / Ubuntu host:
sudo apt update && sudo apt install -y firmware-misc-nonfree firmware-mediatek

# Arch Linux:
# sudo pacman -S linux-firmware

# Fedora / RHEL:
# sudo dnf install linux-firmware
```

#### ⚠️ Host Prerequisite 2: Unmanage Wi-Fi Dongle in Host NetworkManager
If your host runs NetworkManager (`systemctl status NetworkManager`), NetworkManager and the host's background `wpa_supplicant` will attempt to control the RT5370 USB Wi-Fi dongle at the same time as the container. This causes radio conflicts and kernel filter collision errors:
```text
nl80211: kernel reports: Match already configured
```

To give the container exclusive, stable control over the dongle, configure host NetworkManager to ignore it:

```bash
# Create NetworkManager unmanaged override for USB wireless interfaces:
sudo tee /etc/NetworkManager/conf.d/99-unmanage-iscanfly.conf << 'EOF'
[keyfile]
unmanaged-devices=interface-name:wlx*
EOF

# Reload NetworkManager configuration on the host:
sudo systemctl reload NetworkManager

# Verify the interface is unmanaged:
nmcli dev status
```

#### Wi-Fi Connector Capabilities
When `WIFI_ENABLED=true` is set:
- **Automatic Interface Discovery**: Dynamically detects the RT5370 interface (`wlan0`, `wlx...`) and unblocks `rfkill`.
- **Protected Routing**: The container requests a DHCP lease without installing a default gateway (`0.0.0.0/0`), binding only the local scanner subnet (`192.168.18.0/24`) to the Wi-Fi card. This ensures your host machine's internet and primary network routes are never interrupted.
- **Continuous 5-Second Loop**: If the scanner AP goes out of range or drops association, the background manager continuously scans and attempts reconnection every 5 seconds without burning unnecessary CPU cycles when connected.

### Entrypoint Behavior (`docker/entrypoint.sh`)
When the container boots, [`docker/entrypoint.sh`](file:///home/andre/Development/iScanFly-Scanner/docker/entrypoint.sh):
1. Configures `/etc/sane.d/iscanfly.conf` with the scanner's IP (`SCANNER_IP`).
2. Starts the system D-Bus and Avahi daemons for zero-config mDNS discovery.
3. Launches the background RT5370 Wi-Fi connector if `WIFI_ENABLED=true`.
4. Tests backend connectivity with `scanimage -L`.
5. Executes `airsaned` on port `8090` in the foreground.

---

## 5. Usage & Verification

### List Detected Scanners
Verify that SANE recognizes the device:
```bash
scanimage -L
```
*Expected Output:*
```text
device `iscanfly:net:192.168.18.33:23' is a Mustek iScan Air / S400W document feeder
```

### Inspect Available Options
```bash
scanimage -d "iscanfly:net:192.168.18.33:23" --help
```

### Command Line Scanning (`scanimage`)

- **Standard 300 DPI Color Scan (JPEG output with Auto-Crop & Deskew)**:
  ```bash
  scanimage -d iscanfly --resolution 300 --mode Color --format=jpeg > scan.jpg
  ```

- **150 DPI Fast Grayscale Scan (PNG output)**:
  ```bash
  scanimage -d iscanfly --resolution 150 --mode Gray --format=png > doc.png
  ```

- **1-bit Clean Black & White Text / OCR Mode**:
  ```bash
  scanimage -d iscanfly --mode Lineart --format=png > text.png
  ```
  *(Uses adaptive Bradley-Roth thresholding by default. To specify manual threshold: `--threshold 120`)*

- **Disable Auto-Crop or Deskew for Raw Scan**:
  ```bash
  scanimage -d iscanfly --mode Color --autocrop=no --deskew=no --format=jpeg > raw.jpg
  ```

- **Fine 600 DPI Scan**:
  ```bash
  scanimage -d iscanfly --resolution 600 --mode Color --format=png > photo.png
  ```

### GUI Frontends
The scanner is automatically accessible to standard Linux desktop applications:
- **Document Scanner (`simple-scan`)**: Open the application, choose "Mustek iScan Air", and scan.
- **XSane**: Full option sliders and preview support.
- **Paperless-ngx / OCRmyPDF**: Works with any network scan pipeline using `scanimage`.

---

## Maintenance

To calibrate or clean the scanner:
1. Insert the white calibration sheet or wet cleaning sheet into the slot until the roller grabs it.
2. Trigger the action via `scanimage`:
   ```bash
   scanimage -d iscanfly --calibrate
   # or
   scanimage -d iscanfly --clean
   ```

---

### Code Verification & Quality Checks

Run the automated quality assurance suite (`ShellCheck`, `Cppcheck`, `clang-format`, and strict GCC compiler checks):

```bash
# Run verification checks:
./scripts/verify.sh

# Automatically format C code with clang-format:
./scripts/verify.sh --fix
```

If linters are not installed locally on your host machine, `scripts/verify.sh` automatically runs them inside the Docker build container.

---

## License

Core protocol code is licensed under [CC0 1.0 (Public Domain)](http://creativecommons.org/publicdomain/zero/1.0/).
The SANE driver integration is provided under standard SANE / GPL v2+ terms.

