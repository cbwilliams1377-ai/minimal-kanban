# OpenCode handoff — complete Debian deployment

## Goal and known state

The owner wants this Debian computer to run the Minimal Kanban BugBot worker.
Docker Compose is already installed and running. Preserve existing services.
This is a Windows desktop app plus an outbound-only maintenance worker, not a web
app requiring a reverse proxy, ports, DNS, or TLS setup.

Repository: https://github.com/cbwilliams1377-ai/minimal-kanban.git
Use branch **master** explicitly; a separate `main` branch exists.
Read `AGENTS.md` and `MinimalKanban-Bug-Pipeline-Setup.md` before deployment.
This is a deployment task, not an instruction to process a local bug report.

The development computer has prepared these changes:
- Correct Compose build context and persistent workspace, logs and OpenCode auth.
- Explicit branch selection and token-free Git remote URLs.
- Debian cron environment/timezone setup; schedule and startup processing disabled.
- Agent exit-status propagation and whole-pipeline locking.
- Persistent release transaction; build/push/upload failures cannot close tickets.
- Draft upload followed by publication at the exact built commit; retry same version.
- Exact issue-number matching, paginated imports, and fresh owner-only replies.
- Nine offline integration tests, shell syntax and Compose validation passed.

Not yet verified: image build, container startup, actual model authentication or
availability, real GitHub writes/releases, scheduled execution, Windows GUI/update.
Local Docker daemon access was unavailable on the development computer. Do not
claim these checks have passed based on the offline tests.

## Work to complete

1. Inspect the Debian host and existing Docker services. Clone into
   `~/apps/minimal-kanban` if absent; otherwise inspect and update the existing
   checkout without overwriting local work. Use `master`.
2. Read the setup guide. Run the offline tests if Python 3 is available, validate
   Compose, build the actual image, and fix any deployment defects discovered.
   Record changes and verification results in this repository.
3. Preserve any existing `server/.env`. Otherwise copy `.env.example` and keep
   mode 600. Defaults: owner `cbwilliams1377-ai`, repo `minimal-kanban`, branch
   `master`, reply owner `cbwilliams1377-ai`, timezone `America/Chicago`, 09:00.
   Keep `RUN_ON_START=0` and `SCHEDULE_ENABLED=0` during initial setup.
4. Help the owner enter a repository-scoped fine-grained GitHub PAT locally, with
   Contents R/W and Issues R/W. Do not request secrets in chat, echo them, commit
   them, or print expanded Compose configuration. Reuse working credentials if
   already configured. Only pause for input that is actually missing.
5. Start this worker and authenticate the model **inside the container**. Host
   OpenCode login does not imply container login. Check whether the configured
   `opencode/big-pickle` model is available and can answer a harmless prompt before
   invoking BugBot. If unavailable, get the owner's model choice rather than
   silently changing providers or costs. Persist auth in the configured volume.
6. Verify `bash build.sh` inside `/workspace` produces the Windows executable.
   Check GitHub Issues availability, repository visibility and existing releases.
   Follow the guide for a baseline release if needed; do not overwrite an existing
   tag or release. The app's updater needs publicly accessible releases.
7. Inspect existing open issues and report queues before invoking the worker:
   one run processes the oldest report and auto mode can push fixes and publish.
   Use a suitable real report for the end-to-end test. If none exists, ask the
   owner for a small reproducible bug; do not invent a bug or silently process
   an unrelated backlog. A Windows owner must verify GUI/update behavior.
8. After successful verification, enable the daily schedule at the configured
   time, recreate only this service, and inspect cron configuration/environment
   and logs. Keep startup processing off unless the owner asks otherwise.
9. Finish with container status, verified checks, any outstanding Windows check,
   schedule/timezone, log and backup locations, and exact remaining actions.
   Never report full deployment success if authentication or a real run failed.

## Recovery and constraints

The guide describes `.bugbot/server-state/pending.json` and recovery. A `release`
phase retries automatically. An interrupted `agent` phase requires inspection.
Do not delete pending state, reset source, drop volumes, close historical tickets,
or publish a conflicting tag as a shortcut. Preserve existing server workloads.
No inbound ports are needed. Do not run `docker compose down -v`.

The host checkout holds container scripts; `/workspace` in a named volume holds
application source and reports. Pull/rebuild the host checkout after changing
container scripts. Keep intentionally changed application/model configuration
consistent with the branch pulled by the worker.

## Useful verification commands

From repository root:

```bash
python3 server/tests/test_pipeline.py -v
bash -n server/entrypoint.sh server/run-daily.sh server/sync_issues.sh .bugbot/run_bugbot.sh
cd server
docker compose config --quiet
docker compose up -d --build
docker compose logs --tail=100 bugbot
docker compose exec bugbot opencode auth login
docker compose exec bugbot opencode models opencode
docker compose exec -w /workspace bugbot bash build.sh
```

Only after inspecting the queue and completing credentials/model checks:

```bash
docker compose exec bugbot /opt/bugbot/run-daily.sh
```
