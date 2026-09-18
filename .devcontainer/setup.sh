#!/usr/bin/env bash
# Builds the server inside the codespace. Runs once, when the codespace is
# created. The base image already has cmake, ninja, gcc and vcpkg.
set -euo pipefail

echo "--- system packages ---"
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
    pkg-config libcurl4-openssl-dev

echo "--- vcpkg libraries (this is the slow part, ~5-10 minutes) ---"
# Classic mode and the same three ports as the vcpkg folder on the Mac, so
# nothing is added to the project root and a build there is unaffected.
# curl is left out on purpose: find_package(CURL) picks up the system copy,
# the way it picks up the macOS one.
"$VCPKG_ROOT/vcpkg" install crow sqlite3 libsodium

echo "--- build ---"
cmake -B build -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build

echo
echo "Done. Start the server with:  ./.devcontainer/run.sh"
