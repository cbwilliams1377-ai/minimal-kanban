#!/usr/bin/env bash
# One report per run. Persist a release transaction so failed builds/pushes/
# uploads can be retried without running another agent or closing tickets early.
set -euo pipefail
export GH_TOKEN="${GITHUB_TOKEN:?}"
: "${GITHUB_BRANCH:=master}"
REPO="$GITHUB_OWNER/$GITHUB_REPO"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$WORKSPACE"
STATE="$WORKSPACE/.bugbot/server-state"
mkdir -p "$STATE"
exec 8>"$STATE/pipeline.lock"
flock -n 8 || { echo 'Another pipeline run is active'; exit 0; }
# Also exclude direct invocations of the local agent runner.
exec 9>"$WORKSPACE/.bugbot/lock"
flock -n 9 || { echo 'Another agent run is active'; exit 0; }
export BUGBOT_LOCK_HELD=1
PENDING="$STATE/pending.json"
fail() { echo "ERROR: $*" >&2; exit 1; }
clean() { [[ -z "$(git status --porcelain --untracked-files=normal)" ]] || fail 'Workspace has uncommitted files; inspect before continuing.'; }
save() { printf '%s\n' "$1" > "$PENDING.tmp"; mv "$PENDING.tmp" "$PENDING"; }
[[ "$(git branch --show-current)" == "$GITHUB_BRANCH" ]] || fail 'Wrong workspace branch'
clean

if [[ ! -f "$PENDING" ]]; then
    git pull --ff-only origin "$GITHUB_BRANCH"
    bash "$SCRIPT_DIR/sync_issues.sh"
    shopt -s nullglob
    queue=(reports/queue/*.md)
    (( ${#queue[@]} > 0 )) || { echo 'No queued reports'; exit 0; }
    report="${queue[0]##*/}"
    base="$(git rev-parse HEAD)"
    save "$(jq -n --arg base "$base" --arg report "$report" '{phase:"agent",base:$base,report:$report}')"
    bash .bugbot/run_bugbot.sh
    clean
    [[ "$(git branch --show-current)" == "$GITHUB_BRANCH" ]] || fail 'Agent changed branches'
    if [[ ! -f "reports/done/$report" ]]; then
        [[ "$(git rev-parse HEAD)" == "$base" ]] || fail 'Agent committed without completing the report; inspect pending.json'
        bash "$SCRIPT_DIR/sync_issues.sh"
        rm "$PENDING"
        echo 'Report remains queued or blocked; no release'
        exit 0
    fi
    [[ "$(git rev-parse HEAD)" != "$base" ]] || fail 'Report marked done without a fix commit'
    # Keep the transaction in agent phase until the bump is committed; an
    # interrupted agent/bump requires inspection rather than guessing its result.
    version="$(sed -n 's/^static const wchar_t\* APP_VERSION = L"\([0-9.]*\)";.*/\1/p' minimal_kanban.cpp)"
    [[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || fail 'Invalid APP_VERSION'
    # Include existing release versions to avoid reusing a tag.
    releases="$(gh api --paginate "repos/$REPO/releases?per_page=100")"
    latest="$( { printf '%s\n' "$version"; jq -r '.[].tag_name | select(test("^v[0-9]+\\.[0-9]+\\.[0-9]+$")) | ltrimstr("v")' <<<"$releases"; } | sort -V | tail -1)"
    IFS=. read -r major minor patch <<<"$latest"
    version="$major.$minor.$((10#$patch + 1))"
    sed -i "s/^static const wchar_t\* APP_VERSION = L\"[0-9.]*\";/static const wchar_t* APP_VERSION = L\"$version\";/" minimal_kanban.cpp
    git add minimal_kanban.cpp
    git commit -m "chore: bump version to $version"
    save "$(jq --arg sha "$(git rev-parse HEAD)" --arg tag "v$version" '. + {phase:"release",sha:$sha,tag:$tag}' "$PENDING")"
fi

[[ "$(jq -r .phase "$PENDING")" == release ]] || fail 'Interrupted agent/bump: inspect pending.json and workspace before manual recovery (see setup guide)'
sha="$(jq -r .sha "$PENDING")"
tag="$(jq -r .tag "$PENDING")"
report="$(jq -r .report "$PENDING")"
[[ "$(git rev-parse HEAD)" == "$sha" ]] || fail 'HEAD differs from pending release'
# Build must succeed before pushing or publishing anything.
bash build.sh
clean
git push origin "HEAD:refs/heads/$GITHUB_BRANCH"
# Querying the list first distinguishes a missing release from an API failure.
releases="$(gh api --paginate "repos/$REPO/releases?per_page=100")"
release="$(jq -sc --arg tag "$tag" '[.[][] | select(.tag_name == $tag)] | .[0] // empty' <<<"$releases")"
if [[ -z "$release" ]]; then
    # Refuse pre-existing tags: they could point at an unrelated commit.
    [[ -z "$(git ls-remote --tags origin "refs/tags/$tag")" ]] || fail 'Release tag already exists; inspect before proceeding'
    gh release create "$tag" --repo "$REPO" --target "$sha" --draft \
        --title "Minimal Kanban ${tag#v}" --notes 'Automated build from BugBot run.'
    release="$(gh api "repos/$REPO/releases/tags/$tag")"
fi
[[ "$(jq -r .target_commitish <<<"$release")" == "$sha" ]] || fail 'Release target differs from pending commit'
if [[ "$(jq -r .draft <<<"$release")" == true ]]; then
    gh release upload "$tag" MinimalKanban.exe --repo "$REPO" --clobber
    gh release edit "$tag" --repo "$REPO" --draft=false --latest
fi
release="$(gh api "repos/$REPO/releases/tags/$tag")"
jq -e '.draft == false and .prerelease == false and any(.assets[]; .name == "MinimalKanban.exe" and .size > 0)' <<<"$release" >/dev/null || fail 'Published executable missing'
# Only the report associated with this successfully published transaction closes.
if [[ "$report" =~ -#([0-9]+)- ]]; then
    num="${BASH_REMATCH[1]}"
    status="$(gh issue view "$num" --repo "$REPO" --json state --jq .state)"
    if [[ "$status" == OPEN ]]; then
        gh issue close "$num" --repo "$REPO" --comment "Fixed and released in $tag."
    fi
fi
mv "$PENDING" "$STATE/last-release.json"
echo "Published $tag; run complete"
