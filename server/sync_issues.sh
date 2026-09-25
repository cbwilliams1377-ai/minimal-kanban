#!/usr/bin/env bash
# Import issues and resume blocked reports only on fresh owner replies.
set -euo pipefail
export GH_TOKEN="${GITHUB_TOKEN:?}"
REPO="$GITHUB_OWNER/$GITHUB_REPO"
OWNER="${BUGBOT_OWNER:-$GITHUB_OWNER}"
ALLOWED="${BUGBOT_ALLOWED_AUTHORS:-$OWNER}"
QUEUE="$WORKSPACE/reports/queue"
BLOCKED="$WORKSPACE/reports/blocked"
DONE="$WORKSPACE/reports/done"
STATE="$WORKSPACE/.bugbot/server-state"
mkdir -p "$QUEUE" "$BLOCKED" "$DONE" "$STATE"
shopt -s nullglob
for f in "$BLOCKED"/*.md; do
    name="${f##*/}"
    [[ "$name" =~ -#([0-9]+)- ]] || continue
    num="${BASH_REMATCH[1]}"
    # The report mtime establishes when it became blocked. Old comments and
    # unrelated authors cannot resume it; each new block gets a fresh cutoff.
    cutoff="$(date -u -r "$f" '+%Y-%m-%dT%H:%M:%SZ')"
    issue="$(gh issue view "$num" --repo "$REPO" --json state,comments)"
    [[ "$(jq -r .state <<<"$issue")" == OPEN ]] || continue
    comments="$(jq -r --arg owner "$OWNER" --arg cutoff "$cutoff" \
        '.comments[] | select(.author.login == $owner and .createdAt > $cutoff) | .body' <<<"$issue")"
    if [[ -n "$comments" ]]; then
        printf '\n## Owner response\n\n%s\n' "$comments" >> "$f"
        mv "$f" "$QUEUE/"
        echo "resumed #$num from blocked"
    elif [[ ! -f "$STATE/blocked-$num" || "$(cat "$STATE/blocked-$num")" != "$(sha256sum "$f")" ]]; then
        # Put questions on the issue so the owner can answer without server access.
        note="$(mktemp)"
        printf 'BugBot needs your attention. Reply here as @%s to resume.\n\n' "$OWNER" > "$note"
        cat "$f" >> "$note"
        gh issue comment "$num" --repo "$REPO" --body-file "$note"
        rm -f "$note"
        sha256sum "$f" > "$STATE/blocked-$num"
    fi
done

# Paginate instead of silently dropping issues after the first 100.
issues="$(gh api --paginate "repos/$REPO/issues?state=open&per_page=100")"
while IFS= read -r iss; do
    # Prompt-injection guard: only import issues opened by approved accounts.
    author="$(jq -r .user.login <<<"$iss")"
    case ",$ALLOWED," in
        *",$author,"*) ;;
        *) continue ;;
    esac
    num="$(jq -r .number <<<"$iss")"
    matches=("$QUEUE"/*-"#$num"-*.md "$BLOCKED"/*-"#$num"-*.md "$DONE"/*-"#$num"-*.md)
    (( ${#matches[@]} == 0 )) || continue
    title="$(jq -r .title <<<"$iss")"
    body="$(jq -r '.body // ""' <<<"$iss")"
    created="$(jq -r .created_at <<<"$iss")"
    stamp="$(date -u -d "$created" '+%Y%m%d_%H%M%S')"
    slug="$(printf '%s' "$title" | tr '[:upper:]' '[:lower:]' | sed 's/[^a-zA-Z0-9]/-/g' | tr -s '-' | cut -c1-40)"
    file="$QUEUE/$stamp-#$num-$slug.md"
    {
        printf '# %s\n\n- Type: bug\n- Severity: normal\n- Created: %s\n- Source: GitHub issue #%s\n' "$title" "$created" "$num"
        printf '\n## Description\n\n%s\n\n## Steps to reproduce\n\n\n## Environment\n- OS:\n- App version / build:\n\n## Notes\n\n' "$body"
    } > "$file"
    echo "queued #$num"
done < <(jq -c '.[] | select(has("pull_request") | not)' <<<"$issues")
