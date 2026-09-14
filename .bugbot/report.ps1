param([string]$QuickNote)

$ErrorActionPreference = 'Stop'

$ws = $PSScriptRoot | Split-Path -Parent
$queue = Join-Path $ws 'reports\queue'
New-Item -ItemType Directory -Force -Path $queue | Out-Null
$template = Join-Path $PSScriptRoot 'template.md'

function Slug([string]$s) {
    $s = $s -replace '[^a-zA-Z0-9 ]', ''
    $s = $s.Trim()
    if ($s.Length -gt 40) { $s = $s.Substring(0, 40) }
    if ($s.Length -eq 0) { $s = 'report' }
    return ($s -replace '\s+', '-')
}

if ($QuickNote) {
    $type = 'bug'
    $severity = 'medium'
    $title = $QuickNote
    $desc = ''
    $steps = ''
} else {
    Write-Host ''
    Write-Host 'BugBot report capture' -ForegroundColor Cyan
    Write-Host '----------------------'
    $type = ''
    while ($type -notin @('bug', 'feature', 'other')) {
        $type = Read-Host 'Type (bug / feature / other)'
        $type = $type.ToLower().Trim()
        if ($type -eq '') { $type = 'bug' }
    }
    $severity = ''
    while ($severity -notin @('low', 'medium', 'high')) {
        $severity = Read-Host 'Severity (low / medium / high)'
        $severity = $severity.ToLower().Trim()
        if ($severity -eq '') { $severity = 'medium' }
    }
    $title = Read-Host 'Title (one line describing the issue)'
    if ($title.Trim() -eq '') { $title = 'Untitled report' }

    Write-Host ''
    Write-Host 'A Notepad window will open so you can write a description, then another for repro steps.' -ForegroundColor DarkGray
    $descFile = Join-Path $env:TEMP ('bugbot_desc_' + [guid]::NewGuid().ToString('N') + '.txt')
    $stepsFile = Join-Path $env:TEMP ('bugbot_steps_' + [guid]::NewGuid().ToString('N') + '.txt')
    Set-Content -LiteralPath $descFile -Value '' -Encoding UTF8
    Set-Content -LiteralPath $stepsFile -Value '' -Encoding UTF8

    Start-Process -FilePath 'notepad.exe' -ArgumentList "`"$descFile`"" -Wait | Out-Null
    Start-Process -FilePath 'notepad.exe' -ArgumentList "`"$stepsFile`"" -Wait | Out-Null

    $desc = (Get-Content -LiteralPath $descFile -Raw -Encoding UTF8).Trim()
    $steps = (Get-Content -LiteralPath $stepsFile -Raw -Encoding UTF8).Trim()
    Remove-Item -LiteralPath $descFile, $stepsFile -Force -ErrorAction SilentlyContinue
}

if ($desc -eq '') { $desc = '_No description provided._' }
if ($steps -eq '') { $steps = '_None provided._' }

$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$name = $stamp + '-' + (Slug $title) + '.md'
$path = Join-Path $queue $name

$content = Get-Content -LiteralPath $template -Raw -Encoding UTF8
$content = $content.Replace('{TITLE}', $title)
$content = $content.Replace('{type}', $type)
$content = $content.Replace('{severity}', $severity)
$content = $content.Replace('{timestamp}', (Get-Date -Format 'yyyy-MM-dd HH:mm'))
$content = $content.Replace('{description}', $desc)
$content = $content.Replace('{steps}', $steps)

Set-Content -LiteralPath $path -Value $content -Encoding UTF8
Write-Host ''
Write-Host "Report written to:  reports\queue\$name" -ForegroundColor Green
Write-Host 'It will be picked up by the next BugBot run.' -ForegroundColor DarkGray