#!/usr/bin/env bash
# Starts the server in the background every time the codespace starts. Kept
# apart from run.sh so that one stays a plain foreground runner for a terminal.
set -uo pipefail
cd "$(dirname "$0")/.."

# A resumed codespace can still hold the old process; don't stack a second one
# on the same port.
if pgrep -f '[s]tudent_profiles' > /dev/null; then
    echo "server already running on port 8090"
    exit 0
fi

# A container whose build never finished has no binary. Say so and let the
# codespace start anyway: a failure here would block the whole session.
if [ ! -x ./build/student_profiles ]; then
    echo "build/student_profiles is missing - run ./.devcontainer/setup.sh" >&2
    exit 0
fi

nohup ./.devcontainer/run.sh > /tmp/server.log 2>&1 < /dev/null &
echo "server starting on port 8090, log in /tmp/server.log"
