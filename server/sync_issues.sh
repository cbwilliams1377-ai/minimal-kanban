#!/usr/bin/env bash
# Fetch open GitHub issues and materialize them into reports/queue/ as BugBot report files.
# Skips issues that already have a report in queue/blocked/done (matched by "#<number>"),
# and resumes blocked reports whose issue has since been commented on by the owner.
set -euo pipefail

REPO="$GITHUB_OWNER/$GITHUB_REPO"
QUEUE="$WORKSPACE/reports/queue"
BLOCKED="$WORKSPACE/reports/blocked"
DONE="$WORKSPACE/reports/done"
mkdir -p "$QUEUE" "$BLOCKED" "$DONE"

# Resume blocked reports: if the linked issue has owner comments, append them and re-queue.
for f in "$BLOCKED"/*.md; do
    [ -f "$f" ] || continue
    num="${f##*#}"; num="${num%%-*}"
    [[ "$num" =~ ^[0-9]+$ ]] || continue
    comments="$(gh issue view "$num" --repo "$REPO" --json comments --jq '.comments[].body' 2>/dev/null || true)"
    if [ -n "$comments" ]; then
        {
            printf '\n## Owner response\n\n'
            echo "$comments"
        } >> "$f"
        mv "$f" "$QUEUE/"
        echo "resumed #$num from blocked (owner responded)"
    fi
done

# Materialize each open issue that isn't already tracked.
gh issue list --repo "$REPO" --state open --limit 100 \
    --json number,title,body,createdAt 2>/dev/null | jq -c '.[]' | while read -r iss; do
    num="$(jq -r '.number' <<<"$iss")"
    title="$(jq -r '.title' <<<"$iss")"
    body="$(jq -r '.body' <<<"$iss")"
    created="$(jq -r '.createdAt' <<<"$iss")"

    marker="#${num}"
    if grep -rlF "$marker" "$QUEUE" "$BLOCKED" "$DONE" >/dev/null 2>&1; then
        continue
    fi

    stamp="$(date -u -d "$created" '+%Y%m%d_%H%M%S' 2>/dev/null || date -u '+%Y%m%d_%H%M%S')"
    slug="$(printf '%s' "$title" | tr '[:upper:]' '[:lower:]' | sed 's/[^a-zA-Z0-9]/-/g' | tr -s '-' | cut -c1-40)"
    file="${stamp}-${marker}-${slug}.md"

    {
        printf '# %s\n\n' "$title"
        printf -- '- Type: %s\n' "bug"
        printf -- '- Severity: %s\n' "normal"
        printf -- '- Created: %s\n' "$created"
        printf -- '- Source: %s\n' "GitHub issue #${num}"
        printf '\n## Description\n\n%s\n\n' "$body"
        printf '## Steps to reproduce\n\n\n'
        printf '## Environment\n- OS:\n- App version / build:\n\n'
        printf '## Notes\n\n'
    } > "$QUEUE/$file"
    echo "queued #$num => $file"
done
echo "issues sync complete"