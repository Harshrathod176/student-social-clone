#!/usr/bin/env bash
# Starts the server in the background every time the codespace starts. Kept
# apart from run.sh so that one stays a plain foreground runner for a terminal.
set -uo pipefail
cd "$(dirname "$0")/.."

# A resumed codespace can still hold the old process; don't stack a second one
# on the same port. It has to be *this* copy though: the test script runs the
# same binary from a directory under /tmp, and one of those left behind goes
# on answering on the port with its own throwaway database. Finding any
# process called student_profiles was not enough to tell those apart.
here="$(pwd -P)"

for pid in $(pgrep -f '[s]tudent_profiles'); do
    if [ "$(readlink -f "/proc/$pid/cwd" 2> /dev/null)" = "$here" ]; then
        echo "server already running on port 8090"
        exit 0
    fi

    # Same binary, somewhere else: a leftover holding the port we need.
    echo "stopping a stray server from $(readlink "/proc/$pid/cwd" 2> /dev/null)" >&2
    kill "$pid" 2> /dev/null
done

sleep 1

# A container whose build never finished has no binary. Say so and let the
# codespace start anyway: a failure here would block the whole session.
if [ ! -x ./build/student_profiles ]; then
    echo "build/student_profiles is missing - run ./.devcontainer/setup.sh" >&2
    exit 0
fi

# setsid, not a plain background job. The devcontainer CLI kills the
# postStartCommand's process group the moment the command returns, and that
# reaps an ordinary '&' child before it has even opened its log file.
setsid nohup ./.devcontainer/run.sh > /tmp/server.log 2>&1 < /dev/null &

# Hold the start open until the port answers, so a codespace is not reported
# ready while the site is still dead, and so a failed start is visible in the
# creation log rather than silent.
for _ in $(seq 1 30); do
    if curl -sf -o /dev/null --max-time 2 http://127.0.0.1:8090/; then
        echo "server up on port 8090"
        exit 0
    fi
    sleep 1
done

echo "server did not answer on 8090 within 30s - see /tmp/server.log" >&2
