# BugBot daily-runner container.
# Cross-compiles MinimalKanban on Linux (mingw-w64), runs the BugBot agent,
# pushes fixes, publishes GitHub releases, and closes resolved issues.
FROM debian:bookworm-slim

ENV DEBIAN_FRONTEND=noninteractive

# System deps: git, gh CLI, ffmpeg, mingw-w64 cross toolchain, node (for opencode-ai)
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl git gnupg jq nodejs npm bash cron tzdata util-linux \
        ffmpeg \
        g++-mingw-w64-x86-64 mingw-w64-x86-64-dev mingw-w64-tools \
    && rm -rf /var/lib/apt/lists/*

# GitHub CLI (official apt repo)
RUN curl -fsSL https://cli.github.com/packages/githubcli-archive-keyring.gpg \
        | gpg --dearmor -o /usr/share/keyrings/githubcli-archive-keyring.gpg \
    && echo "deb [arch=$(dpkg --print-architecture) signed-by=/usr/share/keyrings/githubcli-archive-keyring.gpg] https://cli.github.com/packages stable main" \
        > /etc/apt/sources.list.d/github-cli.list \
    && apt-get update && apt-get install -y gh && rm -rf /var/lib/apt/lists/*

# The BugBot agent itself (opencode). Installed globally so `opencode` is on PATH.
RUN npm install -g opencode-ai

# Runner scripts
WORKDIR /opt/bugbot
COPY run-daily.sh sync_issues.sh entrypoint.sh ./
RUN chmod +x run-daily.sh sync_issues.sh entrypoint.sh

# The repo checkout lives here; reports/ persists in a named volume.
ENV WORKSPACE=/workspace

ENTRYPOINT ["/opt/bugbot/entrypoint.sh"]
