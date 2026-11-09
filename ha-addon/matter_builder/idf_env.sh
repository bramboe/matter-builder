# Sourced by toolchain.sh and build.sh: where ESP-IDF is. Espressif's own
# image has it in /opt/esp/idf (the release build on GitHub); the add-on's
# light image downloads it into $MB_DATA/toolchain when developer mode
# needs it.
DATA="${MB_DATA:-/data}"
if [ -f /opt/esp/idf/export.sh ]; then
    export IDF_PATH=/opt/esp/idf
else
    export IDF_PATH="$DATA/toolchain/esp-idf"
    export IDF_TOOLS_PATH="$DATA/toolchain/espressif"
fi
