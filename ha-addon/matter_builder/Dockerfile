# ESP-IDF v5.5.5 from Espressif, the same version the firmware is built with
# (not BUILD_FROM: Home Assistant would put its own base image there)
FROM espressif/idf:v5.5.5

# What the Matter bootstrap (connectedhomeip) needs to build its Python
# packages, as listed in its Linux build guide
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential pkg-config libffi-dev libssl-dev libdbus-1-dev libglib2.0-dev \
        libavahi-client-dev libgirepository1.0-dev libcairo2-dev libreadline-dev \
        python3-dev python3-venv python3-pip unzip openssl \
    && rm -rf /var/lib/apt/lists/*

# Finding devices on the network (mDNS) for the add-on's own server, and
# esptool for installing over USB on the Home Assistant machine
RUN python3 -m pip install --no-cache-dir --break-system-packages zeroconf==0.149.16 esptool==4.8.1

# esp-matter at a fixed commit (release/v1.6), downloaded once into /data
# (±6 GB) by toolchain.sh, together with the Python packages: the IDF
# Python environment lives there too, so it survives restarts and updates
ENV ESP_MATTER_COMMIT=fe1efc6cdf62f19b5ca44213900333a6aad7a8b2
ENV IDF_PYTHON_ENV_PATH=/data/toolchain/python_env

# The firmware sources this version builds (copied in by tools/addon_sync.sh)
COPY src /opt/octo-matter
COPY server.py toolchain.sh build.sh release-key.pem /opt/matter-builder/
COPY www /opt/matter-builder/www

WORKDIR /opt/matter-builder
ENTRYPOINT []
CMD [ "python3", "-u", "/opt/matter-builder/server.py" ]
