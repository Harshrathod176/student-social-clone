#!/usr/bin/env bash
# Builds the server inside the codespace. Runs once, when the codespace is
# created.
set -euo pipefail

if ! command -v apt-get > /dev/null; then
    echo "This expects the Ubuntu container from devcontainer.json." >&2
    echo "An Alpine shell here means the codespace came up in recovery mode:" >&2
    echo "rebuild it (Codespaces menu -> Rebuild Container) and try again." >&2
    exit 1
fi

echo "--- system packages ---"
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build git curl zip unzip tar \
    pkg-config libcurl4-openssl-dev

echo "--- vcpkg ---"
# The C++ image normally ships vcpkg, but not every image does, so check
# instead of trusting it and fall back to a copy of our own.
if [ -z "${VCPKG_ROOT:-}" ] || [ ! -x "${VCPKG_ROOT}/vcpkg" ]; then
    VCPKG_ROOT="$HOME/vcpkg"

    if [ ! -d "$VCPKG_ROOT" ]; then
        git clone --depth 1 https://github.com/microsoft/vcpkg "$VCPKG_ROOT"
    fi

    if [ ! -x "$VCPKG_ROOT/vcpkg" ]; then
        "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics
    fi
fi

echo "using vcpkg at $VCPKG_ROOT"

echo "--- libraries (the slow part, ~5-10 minutes) ---"
# The same three ports as the vcpkg folder on the Mac. curl is left out on
# purpose: find_package(CURL) picks up the system copy, the way it picks up
# the macOS one.
"$VCPKG_ROOT/vcpkg" install crow sqlite3 libsodium

echo "--- build ---"
cmake -B build -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build

echo
echo "Done. Start the server with:  ./.devcontainer/run.sh"
