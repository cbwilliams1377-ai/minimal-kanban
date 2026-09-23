#!/usr/bin/env bash
# Container entrypoint: ensure the repo is checked out, then schedule the daily run.
set -euo pipefail

: "${GITHUB_OWNER:?GITHUB_OWNER is required}"
: "${GITHUB_REPO:?GITHUB_REPO is required}"
: "${GITHUB_TOKEN:?GITHUB_TOKEN is required}"
: "${RUN_TIME:=09:00}"
: "${RUN_ON_START:=1}"

export GH_TOKEN="$GITHUB_TOKEN"

mkdir -p "$WORKSPACE"
git config --global user.name "BugBot"
git config --global user.email "bugbot@localhost"

# Clone or pull the repo. The named volume keeps reports/ across runs.
if [ ! -d "$WORKSPACE/.git" ]; then
    echo "[entrypoint] cloning $GITHUB_OWNER/$GITHUB_REPO"
    git clone "https://x-access-token:${GITHUB_TOKEN}@github.com/${GITHUB_OWNER}/${GITHUB_REPO}.git" "$WORKSPACE"
else
    echo "[entrypoint] pulling latest"
    git -C "$WORKSPACE" -c credential.helper= pull --ff-only "https://x-access-token:${GITHUB_TOKEN}@github.com/${GITHUB_OWNER}/${GITHUB_REPO}.git" master
fi

# Authenticate gh once so release/issue commands work.
echo "$GITHUB_TOKEN" | gh auth login --with-token

# Daily schedule in the container's local time.
HOUR="${RUN_TIME%%:*}"
MIN="${RUN_TIME##*:}"
printf '%s %s * * * /opt/bugbot/run-daily.sh >> /var/log/bugbot.log 2>&1\n' "$MIN" "$HOUR" > /etc/cron.d/bugbot
chmod 0644 /etc/cron.d/bugbot
crontab /etc/cron.d/bugbot

echo "[entrypoint] scheduled daily run at $RUN_TIME; running cron"

if [ "$RUN_ON_START" = "1" ]; then
    /opt/bugbot/run-daily.sh >> /var/log/bugbot.log 2>&1 || echo "[entrypoint] startup run-daily.sh exited non-zero (logged)"
fi

exec cron -f