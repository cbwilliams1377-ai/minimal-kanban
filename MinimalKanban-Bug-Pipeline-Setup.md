# Minimal Kanban — Debian BugBot setup

The Windows application saves its board locally. GitHub hosts issues and release
executables. Your separate Debian server runs the scheduled Docker worker; your
development computer does not need to remain running. No inbound port or public
server address is required: the worker makes outbound connections to GitHub and
the model provider.

## 1. Publish the deployment changes from your development computer

The app source is already on `cbwilliams1377-ai/minimal-kanban`, branch `master`.
The worker now explicitly clones, pulls and pushes `GITHUB_BRANCH=master`, so a
separate default `main` branch does not change what it runs. Releases target the
exact built commit. Do not rename branches as part of this deployment.

Review and commit the deployment files, then push `master`:

```bash
cd "/home/caleb/GoogleDrive/Workspaces/My Kanban app - Workspace"
git diff
git add server .bugbot/run_bugbot.sh .gitignore AGENTS.md MinimalKanban-Bug-Pipeline-Setup.md
git commit -m "fix: prepare BugBot for Debian deployment"
git push origin master
```

The application remains version `0.1.0`; this setup change does not publish a
release. Executables, reports, runtime state and `server/.env` are git-ignored.

## 2. Clone on the Debian server

Docker Compose is already installed on your server. Use a normal local directory
for the checkout, not a Google Drive synchronized folder.

```bash
mkdir -p ~/apps
cd ~/apps
git clone --branch master --single-branch https://github.com/cbwilliams1377-ai/minimal-kanban.git
cd minimal-kanban/server
cp .env.example .env
chmod 600 .env
nano .env
```

Set these values:

```dotenv
GITHUB_OWNER=cbwilliams1377-ai
GITHUB_REPO=minimal-kanban
GITHUB_BRANCH=master
GITHUB_TOKEN=YOUR_TOKEN_HERE
BUGBOT_OWNER=cbwilliams1377-ai
RUN_TIME=09:00
TZ=America/Chicago
RUN_ON_START=0
SCHEDULE_ENABLED=0
```

Create a fine-grained GitHub personal access token restricted to this repository,
with **Contents: read/write** and **Issues: read/write**. There is no separate
Releases permission: release operations use Contents. Keep the token on the server
in `.env`, never in Git or the application. The worker uses environment-token
authentication and stores a credential-free Git remote URL.

GitHub Issues must be enabled. The current Windows updater requires public access
to the repository's releases; it does not authenticate to private repositories.

## 3. Build, start and authenticate

From the server's `server/` directory:

```bash
docker compose config --quiet
docker compose up -d --build
docker compose logs --tail=100 bugbot
docker compose exec bugbot opencode auth login
docker compose exec bugbot opencode auth list
docker compose exec bugbot opencode models opencode
```

If Docker requires elevated privileges on this host, run the Docker commands with
`sudo` according to your existing setup. Do not print `docker compose config`
without `--quiet` when sharing logs; it can expose environment secrets.

The configured model is `opencode/big-pickle` in `.bugbot/config.json`. Confirm it
is available to your account before enabling automation. Provider credentials
persist in the `bugbot-opencode-data` volume at `/root/.local/share/opencode`;
configuration persists separately at `/root/.config/opencode`. If you select a
different model, deliberately update the repository configuration and push it.
The Linux runner derives the workspace from its own location; the Windows path
in `config.json` is not used by the container.

At this stage neither cron nor a startup run processes tickets. The worker clones
the repository into a separate persistent volume at `/workspace`.

## 4. Verify the build and first release

Verify the Windows cross-build without invoking the model:

```bash
docker compose exec -w /workspace bugbot bash build.sh
```

If no initial release exists, publish `v0.1.0` through GitHub's release UI from the
appropriate `master` commit, attaching a freshly built `MinimalKanban.exe`. To
copy the server build to a file on the host:

```bash
docker compose cp bugbot:/workspace/MinimalKanban.exe ./MinimalKanban.exe
```

Verify in GitHub that the release tag points to the source used for the build.
The worker also supports creating its first automated release after fixing a
report; a baseline release is useful for testing the Windows updater.

## 5. Test one real ticket, then enable the schedule

The repository is configured for **auto mode**: a manual pipeline run can modify
source, commit, push, publish a release and close the ticket. It processes the
oldest queued report, one per run. Inspect existing open issues first, then file
a small real bug with reproducible steps.

```bash
docker compose exec bugbot /opt/bugbot/run-daily.sh
```

Confirm the fix, release asset and ticket closure on GitHub. Run the Windows app
and test **Ctrl+R** (report a bug) and **Ctrl+U** (check for updates).

After the test succeeds, set `SCHEDULE_ENABLED=1` in `.env` and recreate:

```bash
docker compose up -d
```

`RUN_TIME` is interpreted in `TZ` (default example: 09:00 America/Chicago).
Keep `RUN_ON_START=0` unless you also want processing every time the container
starts. Debian cron receives the container environment through a root-readable
runtime file. The entire pipeline is locked against overlapping runs.

To react to a new ticket quickly instead of waiting for `RUN_TIME`, set
`CHECK_INTERVAL_MIN` (e.g. `10`): the same pipeline then runs every N minutes,
pulls, syncs open issues and exits immediately when nothing is queued. Leave it
empty for the single daily run. Requires `SCHEDULE_ENABLED=1`.

## 6. Reports, releases and failure recovery

- `reports/queue/`: pending reports, imported from open GitHub issues (not PRs).
- `reports/blocked/`: agent needs information or owner review. The worker posts
  the report on the linked issue. Only a new comment by `BUGBOT_OWNER`, posted
  after the report became blocked, resumes it. Old and other-author comments do
  not count. With an organization repo, set `BUGBOT_OWNER` to your personal login.
- `reports/done/`: locally completed fixes; this does not by itself mean released.
- `.bugbot/server-state/pending.json`: active transaction linking one report to
  its exact release commit and tag. Never remove it casually.
- `.bugbot/server-state/last-release.json`: last successful transaction.

Build, push or upload failure stops the pipeline before ticket closure. The next
run retries a transaction in `release` phase, using the same commit and version,
before accepting another report. Uploads go to a draft release; only a successful
upload is published. Only the transaction's linked issue is closed, after the
published release contains the executable. Existing files in `done/` are not
bulk-closed. An interrupted close can safely retry.

An agent failure or interruption during the version bump leaves the transaction
in `agent` phase and requires inspection. Review `pending.json`, Git history,
working-tree changes, the report and the agent log. Do not delete that marker
until you have deliberately restored a clean known state and re-queued the report,
or manually completed and verified the corresponding release. Dirty workspaces,
branch mismatches and conflicting release tags also stop for inspection. This
preserves evidence rather than automatically discarding partial work.

Useful commands:

```bash
docker compose logs --tail=100 bugbot
docker compose exec bugbot tail -n 100 /var/log/bugbot.log
docker compose exec bugbot tail -n 100 /workspace/.bugbot/bugbot.log
docker compose exec -w /workspace bugbot git status --short
docker compose exec bugbot cat /workspace/.bugbot/server-state/pending.json
```

Manual run output appears in your terminal; scheduled/startup output goes to
`/var/log/bugbot.log`. Named volumes preserve the checkout, report queue, credentials
and logs. Back them up. `docker compose down -v` deletes these volumes.

To update runner/container code, pull the host checkout and rebuild:

```bash
cd ~/apps/minimal-kanban
git pull --ff-only origin master
cd server
docker compose up -d --build
```

## 7. Local verification

Offline integration tests use temporary local Git repositories and mocked GitHub,
model and build commands. They do not create real issues, releases or model calls.

```bash
python3 server/tests/test_pipeline.py -v
bash -n server/entrypoint.sh server/run-daily.sh server/sync_issues.sh .bugbot/run_bugbot.sh
```

These checks do not replace a real Debian image build, provider authentication,
or Windows GUI/update testing.

References: [Compose build paths](https://docs.docker.com/reference/compose-file/build/),
[GitHub token authentication](https://cli.github.com/manual/gh_auth_login),
[release permissions](https://docs.github.com/en/rest/releases/releases),
[OpenCode credentials](https://opencode.ai/docs/cli/).
