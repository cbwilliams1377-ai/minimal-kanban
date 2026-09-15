#!/usr/bin/env bash
# BugBot daily runner for Linux/macOS. Mirrors run_bugbot.ps1.
# Usage: ./run_bugbot.sh
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE="$(dirname "$SCRIPT_DIR")"
CFG="$SCRIPT_DIR/config.json"
LOG="$SCRIPT_DIR/bugbot.log"
LOCK="$SCRIPT_DIR/lock"
PROMPT_FILE="$SCRIPT_DIR/BUGBOT.md"

write_log() {
    local line
    line="$(date '+%Y-%m-%d %H:%M:%S')  $*"
    echo "$line"
    echo "$line" >> "$LOG"
}

cfg_get() {
    local key="$1" val=""
    if command -v python3 >/dev/null 2>&1; then
        val="$(python3 -c 'import json,sys
try: print(json.load(open(sys.argv[1]))[sys.argv[2]])
except Exception: pass' "$CFG" "$key" 2>/dev/null)"
    elif command -v jq >/dev/null 2>&1; then
        val="$(jq -r --arg k "$key" '.[$k] // empty' "$CFG" 2>/dev/null)"
    fi
    printf '%s' "$val"
}

if [ ! -f "$CFG" ]; then
    write_log "ERROR: config.json not found. Aborting."
    exit 1
fi
if [ ! -f "$PROMPT_FILE" ]; then
    write_log "ERROR: BUGBOT.md not found. Aborting."
    exit 1
fi

MODEL="$(cfg_get model)"
[ -n "$MODEL" ] || { write_log "ERROR: model missing from config.json. Aborting."; exit 1; }

OC="$(cfg_get opencodePath)"
[ -n "$OC" ] || OC=opencode

# Lock to prevent overlapping runs (same "lock" file convention as the Windows runner).
exec 9>"$LOCK"
if ! flock -n 9; then
    write_log "SKIP: another BugBot run is in progress. Exiting."
    exit 0
fi

# Pick the oldest pending report (bash globs expand in sorted order).
QUEUE="$WORKSPACE/reports/queue"
oldest=""
for f in "$QUEUE"/*.md; do
    [ -f "$f" ] || continue
    oldest="$(basename "$f")"
    break
done
if [ -z "$oldest" ]; then
    write_log "INFO: no pending reports in reports/queue. Exiting."
    exit 0
fi

write_log "Processing report: $oldest"
write_log "Running: $OC run --dir <workspace> --model $MODEL --auto (prompt = BUGBOT.md)"

prompt="$(cat "$PROMPT_FILE")"
"$OC" run --dir "$WORKSPACE" --model "$MODEL" --auto "$prompt" 2>&1 | tee -a "$LOG"
write_log "Exit code: ${PIPESTATUS[0]}"