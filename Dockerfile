FROM debian:trixie-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    libsane-dev \
    libjpeg-dev \
    sane-utils \
    imagemagick \
    curl \
    pkg-config \
    ca-certificates \
    cppcheck \
    clang-format \
    shellcheck \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build

CMD ["make", "dist"]

