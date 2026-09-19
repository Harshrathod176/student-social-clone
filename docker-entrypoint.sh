#!/bin/sh
# The volume comes up empty on a first deploy, so the directories the program
# writes into have to exist before it starts.
set -e
mkdir -p /data/uploads
exec "$@"
