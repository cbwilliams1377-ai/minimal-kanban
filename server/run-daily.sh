#!/usr/bin/env bash
# Daily BugBot run:
#   1. pull  2. sync issues  3. run the agent  4. (optional) bump version + build
#   5. push  6. publish release  7. close fixed issues
set -euo pipefail

export GH_TOKEN="$GITHUB_TOKEN"
REPO="$GITHUB_OWNER/$GITHUB_REPO"
cd "$WORKSPACE"

log() { echo "[run-daily] $*"; }

# 1. Pull latest. Record the commit we started from so we can tell if the agent changed source.
git -c credential.helper= pull --ff-only \
    "https://x-access-token:${GITHUB_TOKEN}@github.com/${REPO}.git" master || \
    log "WARN: pull failed; continuing with local state"
BASE="$(git rev-parse HEAD)"

# 2. Turn open GitHub issues into reports/queue/.
/opt/bugbot/sync_issues.sh

# 3. Run the BugBot agent (auto mode: it implements, builds, commits, moves to done/).
if [ ! -d "$WORKSPACE/.bugbot" ]; then
    log "ERROR: workspace is missing .bugbot/; aborting"
    exit 1
fi
bash "$WORKSPACE/.bugbot/run_bugbot.sh" || true

# 4. Decide whether the agent changed source and we owe the world a new release.
CHANGED="$(git diff --quiet "$BASE" HEAD -- minimal_kanban.cpp build.bat build.sh .bugbot/ \
    && echo no || echo yes)"

if [ "$CHANGED" = "yes" ]; then
    version="$(sed -n 's/.*APP_VERSION = L"\([0-9.]*\)";.*/\1/p' minimal_kanban.cpp)"
    [ -n "$version" ] || version="0.0.0"
    last="$(gh release list --repo "$REPO" --limit 1 --json tagName --jq '.[0].tagName' 2>/dev/null || true)"
    last="${last#v}"; last="${last:-$version}"

    IFS=. read -r a b c <<<"$last"
    [ -z "$c" ] && { c=0; [ -z "$b" ] && b=0; }
    newver="$a.$b.$((c + 1))"
    tag="v${newver}"

    sed -i "s/static const wchar_t\* APP_VERSION = L\"[0-9.]*\";/static const wchar_t* APP_VERSION = L\"$newver\";/" minimal_kanban.cpp
    git add minimal_kanban.cpp
    git commit -m "chore: bump version to $newver" || true

    ./build.sh
    log "built release $tag"
fi

# 5. Push agent commits + (if any) the version bump.
git push "https://x-access-token:${GITHUB_TOKEN}@github.com/${REPO}.git" master 2>&1 || \
    log "WARN: push failed"

# 6. Publish the release if there is new source.
if [ "$CHANGED" = "yes" ]; then
    gh release create "$tag" MinimalKanban.exe --repo "$REPO" \
        --title "Minimal Kanban $newver" --notes "Automated build from BugBot run." \
        && log "published release $tag" || log "WARN: release create failed"
fi

# 7. Close issues whose reports the agent moved to done/.
for f in "$WORKSPACE"/reports/done/*.md; do
    [ -f "$f" ] || continue
    num="${f##*#}"; num="${num%%-*}"
    [[ "$num" =~ ^[0-9]+$ ]] || continue
    if ! gh issue view "$num" --repo "$REPO" --json state --jq '.state' 2>/dev/null | grep -qx open; then
        continue
    fi
    gh issue close "$num" --repo "$REPO" --comment "Fixed and released." \
        && log "closed issue #$num" || log "WARN: failed to close #$num"
done

log "run complete"