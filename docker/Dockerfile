# ==============================================================================
# Multi-stage Dockerfile for iScanFly SANE + AirSane Server
# ==============================================================================

# Stage 1: Build the iScanFly SANE driver from source
FROM debian:trixie-slim AS driver-builder

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential \
    libsane-dev \
    libjpeg-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY include/ include/
COPY src/ src/
COPY conf/ conf/
COPY scripts/ scripts/
COPY Makefile Makefile

RUN make dist

# Stage 2: Runtime image with AirSane and iScanFly backend
FROM debian:trixie-slim

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    ca-certificates curl \
    libjpeg62-turbo libusb-1.0-0 \
    sane-utils libsane1 libsane-common sane-airscan avahi-daemon dbus \
    bash iproute2 iw wpasupplicant isc-dhcp-client rfkill \
    build-essential cmake git g++ pkg-config \
    libsane-dev libjpeg-dev libpng-dev libavahi-client-dev libusb-1.0-0-dev \
    && rm -rf /var/lib/apt/lists/*

# Build and install AirSane (eSCL AirScan server)
RUN git clone --depth 1 https://github.com/SimulPiscator/AirSane.git /tmp/AirSane && \
    cmake -S /tmp/AirSane -B /tmp/AirSane-build -DCMAKE_BUILD_TYPE=Release && \
    cmake --build /tmp/AirSane-build --parallel && \
    cmake --install /tmp/AirSane-build && \
    rm -f /lib/systemd/system/airsaned.service && \
    rm -rf /tmp/AirSane /tmp/AirSane-build

# Configure Avahi and saned user permissions
RUN sed -i '/^[[:space:]]*enable-dbus[[:space:]]*=/d; /^\[server\]/a enable-dbus=yes' /etc/avahi/avahi-daemon.conf && \
    sed -i '/^[[:space:]]*use-ipv6[[:space:]]*=/d; /^\[server\]/a use-ipv6=no' /etc/avahi/avahi-daemon.conf && \
    usermod -aG scanner,avahi saned

# Copy built iScanFly SANE driver from stage 1 driver-builder
COPY --from=driver-builder /build/dist/libsane-iscanfly.so.1.0.0 /usr/lib/x86_64-linux-gnu/sane/
RUN ln -sf libsane-iscanfly.so.1.0.0 /usr/lib/x86_64-linux-gnu/sane/libsane-iscanfly.so.1 && \
    ln -sf libsane-iscanfly.so.1 /usr/lib/x86_64-linux-gnu/sane/libsane-iscanfly.so && \
    mkdir -p /etc/sane.d/dll.d && \
    echo "iscanfly" > /etc/sane.d/dll.d/iscanfly && \
    ldconfig

COPY --from=driver-builder /build/dist/iscanfly.conf /etc/sane.d/iscanfly.conf

EXPOSE 8090

COPY docker/wifi-connector.sh /usr/local/bin/wifi-connector.sh
COPY docker/entrypoint.sh /entrypoint.sh
RUN chmod +x /usr/local/bin/wifi-connector.sh /entrypoint.sh

ENTRYPOINT ["/entrypoint.sh"]
