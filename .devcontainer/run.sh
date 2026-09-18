#!/usr/bin/env bash
# Every path in main.cpp is relative to the project root, so start from there.
set -euo pipefail
cd "$(dirname "$0")/.."
exec ./build/student_profiles
