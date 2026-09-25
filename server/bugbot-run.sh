#!/usr/bin/env bash
# Manually trigger one BugBot run — the same logic the 10-minute cron tick
# executes (git pull -> sync issues -> run agent if queued -> build/release/close).
# Idle queues exit immediately with "No queued reports".
set -euo pipefail
cd "$(dirname "$0")"

container="bugbot"
if ! docker ps --format '{{.Names}}' | grep -qx "$container"; then
    echo "Container '$container' is not running. Start it with: docker compose up -d" >&2
    exit 1
fi

exec docker compose exec "$container" bash /run/bugbot-scheduled