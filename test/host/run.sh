#!/bin/sh
# Build and run the host tests (no ESP-IDF needed).
set -eu
cd "$(dirname "$0")"
mkdir -p build
${CXX:-c++} -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined \
    -I ../../components/octo_protocol/include \
    ../../components/octo_protocol/octo_protocol.cpp test_protocol.cpp \
    -o build/test_protocol
./build/test_protocol
