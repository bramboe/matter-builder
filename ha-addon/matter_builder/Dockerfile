# A light image on Home Assistant's Debian base (build.yaml), for amd64 and
# aarch64: what every user needs to install, link and update devices. The
# ESP32 build tools (ESP-IDF and esp-matter, several GB) are downloaded into
# /data by toolchain.sh only when developer mode is turned on.
# Home Assistant passes its base image (build.yaml); standalone uses plain Debian
ARG BUILD_FROM=debian:bookworm-slim
FROM $BUILD_FROM

# Python for the add-on; git, cmake, ninja, ccache and the libraries the
# Matter bootstrap (connectedhomeip) builds its Python packages against, for
# developer mode; tzdata gives devices with time rules Home Assistant's zone
RUN apt-get update && apt-get install -y --no-install-recommends \
        python3 python3-pip python3-venv python3-dev git wget unzip openssl ca-certificates \
        cmake ninja-build ccache flex bison gperf libusb-1.0-0 \
        build-essential pkg-config libffi-dev libssl-dev libdbus-1-dev libglib2.0-dev \
        libavahi-client-dev libgirepository1.0-dev libcairo2-dev libreadline-dev \
        tzdata \
    && rm -rf /var/lib/apt/lists/*

# Finding devices on the network (mDNS) for the add-on's own server, and
# esptool for installing over USB on the Home Assistant machine
RUN python3 -m pip install --no-cache-dir --break-system-packages zeroconf==0.149.16 esptool==4.8.1

# Developer mode's toolchain lives in /data, so it survives restarts and
# updates: ESP-IDF at the firmware's version, its tools and Python
# environment, and esp-matter at a fixed commit (release/v1.6)
ENV ESP_IDF_VERSION=v5.5.5
ENV ESP_MATTER_COMMIT=fe1efc6cdf62f19b5ca44213900333a6aad7a8b2
ENV IDF_PYTHON_ENV_PATH=/data/toolchain/python_env
# Components of configs, fetched from GitHub once per tag or commit
ENV MB_COMPONENT_CACHE=/data/component-cache

# The firmware sources this version builds (copied in by tools/addon_sync.sh)
COPY src /opt/octo-matter
COPY server.py toolchain.sh build.sh idf_env.sh release-key.pem /opt/matter-builder/
COPY www /opt/matter-builder/www

WORKDIR /opt/matter-builder
ENTRYPOINT []
CMD [ "python3", "-u", "/opt/matter-builder/server.py" ]
