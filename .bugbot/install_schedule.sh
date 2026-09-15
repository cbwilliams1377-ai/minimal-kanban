#!/usr/bin/env bash
# Register/remove a daily cron entry for the BugBot (Linux/macOS equivalent of
# install_schedule.bat). Per-user crontab — no root needed.
#
#   ./install_schedule.sh           -> daily at 09:00
#   ./install_schedule.sh 12:30     -> daily at 12:30
#   ./install_schedule.sh /delete   -> remove the entry
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TAG="# bugbot"

if [ "${1:-}" = "/delete" ]; then
    crontab -l 2>/dev/null | grep -v -F "$TAG" | grep -vF "$SCRIPT_DIR/run_bugbot.sh" | crontab -
    echo "Removed BugBot cron entry."
    exit 0
fi

time="${1:-09:00}"
if ! [[ "$time" =~ ^[0-2]?[0-9]:[0-5][0-9]$ && "${time%%:*}" -le 23 ]]; then
    echo "Bad time '$time'. Use HH:MM (e.g. 09:00)." >&2
    exit 1
fi
hour="${time%%:*}"
min="${time##*:}"

crontab -l 2>/dev/null | grep -v -F "$TAG" | grep -vF "$SCRIPT_DIR/run_bugbot.sh" > /tmp/bugbot_cron.$$
echo "$min $hour * * * $SCRIPT_DIR/run_bugbot.sh >> \"$SCRIPT_DIR/cron.log\" 2>&1 $TAG" >> /tmp/bugbot_cron.$$
crontab /tmp/bugbot_cron.$$
rm -f /tmp/bugbot_cron.$$

echo "Installed daily cron: $min $hour * * *"
echo "Runner: $SCRIPT_DIR/run_bugbot.sh"
echo "Log: $SCRIPT_DIR/cron.log"
echo "Remove later with: $0 /delete"