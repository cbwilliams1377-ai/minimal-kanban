# 'Continue' so native command stderr (opencode) doesn't abort the script.
$ErrorActionPreference = 'Continue'

$scriptDir = $PSScriptRoot
$configPath = Join-Path $scriptDir 'config.json'
$logPath = Join-Path $scriptDir 'bugbot.log'
$lockPath = Join-Path $scriptDir 'lock'
$promptPath = Join-Path $scriptDir 'BUGBOT.md'

function Write-Log([string]$msg) {
    $line = '{0}  {1}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $msg
    Write-Host $line
    Add-Content -LiteralPath $logPath -Value $line -Encoding UTF8
}

# Load config.
if (-not (Test-Path -LiteralPath $configPath)) {
    Write-Log 'ERROR: config.json not found. Aborting.'
    exit 1
}
$cfg = Get-Content -LiteralPath $configPath -Raw -Encoding UTF8 | ConvertFrom-Json
$workspace = $cfg.workspace
if (-not (Test-Path -LiteralPath $workspace)) {
    Write-Log "ERROR: workspace not found: $workspace. Aborting."
    exit 1
}
if (-not (Test-Path -LiteralPath $promptPath)) {
    Write-Log 'ERROR: BUGBOT.md not found. Aborting.'
    exit 1
}

# Lock to prevent overlapping runs.
if (Test-Path -LiteralPath $lockPath) {
    $lk = Get-Content -LiteralPath $lockPath -Raw -Encoding UTF8
    Write-Log "SKIP: another BugBot run is in progress ($lk). Exiting."
    exit 0
}
Set-Content -LiteralPath $lockPath -Value "PID $PID on $env:COMPUTERNAME" -Encoding UTF8

try {
    # Pick the oldest pending report.
    $queue = Join-Path $workspace 'reports\queue'
    $oldest = Get-ChildItem -LiteralPath $queue -Filter '*.md' -File -ErrorAction SilentlyContinue |
        Sort-Object Name | Select-Object -First 1

    if (-not $oldest) {
        Write-Log 'INFO: no pending reports in reports\queue. Exiting.'
        exit 0
    }

    Write-Log "Processing report: $($oldest.Name)"

    $oc = $cfg.opencodePath
    if (-not $oc) { $oc = 'opencode' }

    $prompt = Get-Content -LiteralPath $promptPath -Raw -Encoding UTF8
    $ocArgs = @('run', '--dir', $workspace, '--model', $cfg.model, '--auto', $prompt)

    Write-Log "Running: $oc run --dir <workspace> --model $($cfg.model) --auto (prompt = BUGBOT.md)"
    $output = & $oc @ocArgs 2>&1
    $outputText = ($output | Out-String).Trim()
    if ($outputText) { Write-Log ($outputText -replace "\r?\n", "`n    ") }
    Write-Log "Exit code: $LASTEXITCODE"
}
finally {
    Remove-Item -LiteralPath $lockPath -Force -ErrorAction SilentlyContinue
}

exit 0