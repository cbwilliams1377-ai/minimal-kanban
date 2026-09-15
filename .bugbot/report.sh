#!/usr/bin/env bash
# BugBot report capture for Linux/macOS. Mirrors report.ps1.
# Usage: ./report.sh                (interactive prompts)
#        ./report.sh "a bug note"   (fast capture, defaults to bug/medium)
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE="$(dirname "$SCRIPT_DIR")"
QUEUE="$WORKSPACE/reports/queue"
TEMPLATE="$SCRIPT_DIR/template.md"
mkdir -p "$QUEUE"

slug() {
    local s
    s="$(printf '%s' "$1" | tr -c '[:alnum:]' ' ' | tr -s ' ' | sed 's/^ //; s/ $//' | tr ' ' '-')"
    s="${s:0:40}"
    [ -n "$s" ] || s=report
    printf '%s' "$s"
}

if [ $# -ge 1 ]; then
    type=bug
    severity=medium
    title="$1"
    desc=""
    steps=""
else
    printf '\nBugBot report capture\n----------------------\n'
    type=bug
    printf 'Type (bug/feature/other) [bug]: '
    read -r t
    case "$t" in
        feature) type=feature ;;
        other) type=other ;;
        "") : ;;
        *) type=bug ;;
    esac

    severity=medium
    printf 'Severity (low/medium/high) [medium]: '
    read -r s
    case "$s" in
        low|high) severity="$s" ;;
        medium|"") severity=medium ;;
        *) severity=medium ;;
    esac

    printf 'Title (one line describing the issue): '
    read -r title
    title="${title:-Untitled report}"

    # Description and steps via $EDITOR (fallback: nano/vim/vi, else stdin).
    editor="${EDITOR:-}"
    if [ -z "$editor" ]; then
        for e in nano vim vi; do
            if command -v "$e" >/dev/null 2>&1; then editor="$e"; break; fi
        done
    fi

    desc_file="$(mktemp)"
    steps_file="$(mktemp)"

    if [ -n "$editor" ]; then
        echo "Opening $editor for a description (save & quit when done)..."
        "$editor" "$desc_file"
        echo "Opening $editor for repro steps (save & quit when done)..."
        "$editor" "$steps_file"
    else
        echo "No editor found. Type a description; finish with Ctrl-D."
        cat > "$desc_file"
        echo "Now type repro steps; finish with Ctrl-D."
        cat > "$steps_file"
    fi

    desc="$(cat "$desc_file")"
    steps="$(cat "$steps_file")"
    rm -f "$desc_file" "$steps_file"
fi

[ -n "$desc" ] || desc="_No description provided._"
[ -n "$steps" ] || steps="_None provided._"

stamp="$(date +%Y%m%d_%H%M%S)"
name="${stamp}-$(slug "$title").md"
path="$QUEUE/$name"

content="$(cat "$TEMPLATE")"
content="${content//\{TITLE\}/$title}"
content="${content//\{type\}/$type}"
content="${content//\{severity\}/$severity}"
content="${content//\{timestamp\}/$(date '+%Y-%m-%d %H:%M')}"
content="${content//\{description\}/$desc}"
content="${content//\{steps\}/$steps}"
printf '%s\n' "$content" > "$path"

printf '\nReport written to:  reports/queue/%s\n' "$name"
echo 'It will be picked up by the next BugBot run.'