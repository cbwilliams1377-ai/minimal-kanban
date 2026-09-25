#!/usr/bin/env bash
set -euo pipefail
: "${GITHUB_OWNER:?GITHUB_OWNER is required}"
: "${GITHUB_REPO:?GITHUB_REPO is required}"
: "${GITHUB_TOKEN:?GITHUB_TOKEN is required}"
: "${GITHUB_BRANCH:=master}"
: "${WORKSPACE:=/workspace}"
: "${RUN_TIME:=09:00}"
: "${RUN_ON_START:=0}"
: "${SCHEDULE_ENABLED:=0}"
: "${CHECK_INTERVAL_MIN:=}"
: "${TZ:=Etc/UTC}"
export GH_TOKEN="$GITHUB_TOKEN" GITHUB_BRANCH WORKSPACE
[[ "$RUN_TIME" =~ ^([01][0-9]|2[0-3]):[0-5][0-9]$ ]] || { echo 'RUN_TIME must be HH:MM'; exit 1; }
[[ "$RUN_ON_START" =~ ^[01]$ && "$SCHEDULE_ENABLED" =~ ^[01]$ ]] || exit 1
[[ -z "$CHECK_INTERVAL_MIN" || "$CHECK_INTERVAL_MIN" =~ ^[1-9][0-9]*$ ]] || { echo 'CHECK_INTERVAL_MIN must be a positive integer or empty'; exit 1; }
[[ -f "/usr/share/zoneinfo/$TZ" ]] || { echo 'Unknown TZ'; exit 1; }
ln -snf "/usr/share/zoneinfo/$TZ" /etc/localtime
printf '%s\n' "$TZ" > /etc/timezone

git config --global user.name BugBot
git config --global user.email bugbot@localhost
# Uses GH_TOKEN at runtime; never embeds the token in the remote URL.
gh auth setup-git --hostname github.com
mkdir -p "$WORKSPACE"
url="https://github.com/$GITHUB_OWNER/$GITHUB_REPO.git"
if [[ ! -d "$WORKSPACE/.git" ]]; then
    git clone --branch "$GITHUB_BRANCH" --single-branch "$url" "$WORKSPACE"
else
    git -C "$WORKSPACE" remote set-url origin "$url"
    [[ "$(git -C "$WORKSPACE" branch --show-current)" == "$GITHUB_BRANCH" ]] || {
        echo 'Workspace branch differs from GITHUB_BRANCH; inspect the volume before switching.'; exit 1;
    }
fi

# Debian cron does not inherit the container environment. Save shell-quoted exports
# for the scheduled wrapper, including any configured provider API keys.
umask 077
export -p > /run/bugbot-env
cat > /run/bugbot-scheduled <<'WRAPPER'
#!/bin/bash
set -euo pipefail
source /run/bugbot-env
exec /opt/bugbot/run-daily.sh
WRAPPER
chmod 700 /run/bugbot-scheduled
if [[ "$SCHEDULE_ENABLED" == 1 ]]; then
    if [[ -n "$CHECK_INTERVAL_MIN" ]]; then
        printf '*/%s * * * * root /run/bugbot-scheduled >> /var/log/bugbot.log 2>&1\n' \
            "$CHECK_INTERVAL_MIN" > /etc/cron.d/bugbot
        echo "Checking for new tickets every $CHECK_INTERVAL_MIN minutes ($TZ); empty queue exits immediately"
    else
        hour="${RUN_TIME%%:*}"; minute="${RUN_TIME##*:}"
        printf '%d %d * * * root /run/bugbot-scheduled >> /var/log/bugbot.log 2>&1\n' \
            "$((10#$minute))" "$((10#$hour))" > /etc/cron.d/bugbot
        echo "Scheduled daily run at $RUN_TIME ($TZ)"
    fi
    chmod 644 /etc/cron.d/bugbot
else
    rm -f /etc/cron.d/bugbot
    echo 'Schedule disabled; ready for authentication and a manual test.'
fi
touch /var/log/bugbot.log
if [[ "$RUN_ON_START" == 1 ]]; then
    /opt/bugbot/run-daily.sh >> /var/log/bugbot.log 2>&1 || echo 'Startup run failed; see /var/log/bugbot.log'
fi
exec cron -f
