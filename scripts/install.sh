#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
cd "$REPO_ROOT"

if [ ! -d "dist/punchfx" ]; then
    echo "Error: dist/punchfx not found. Run ./scripts/build.sh first."
    exit 1
fi

ssh root@move.local "mkdir -p /data/UserData/schwung/modules/audio_fx/punchfx"
scp -r dist/punchfx/* root@move.local:/data/UserData/schwung/modules/audio_fx/punchfx/
ssh root@move.local "chown -R ableton:users /data/UserData/schwung/modules/audio_fx/punchfx"

echo "Installed Punch-In FX to Move. Restart Schwung to load."
