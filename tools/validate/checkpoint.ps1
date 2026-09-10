# One-command validation checkpoint: capture a recomp RDRAM dump at a game-state landmark, then diff it
# against the matching PJ64 golden at BOTH layers and print a pass/fail table.
#
#   LOGIC  layer (rdram_golden_diff.py) - faithful CPU/game state. A diff here = recomp/logic bug.
#   PARSER layer (dl_diff.py)           - the Factor 5 display list. A diff here (with logic clean) =
#                                         a parser/desync bug, localized to an opcode + chunk ordinal.
#
# Checkpoints (must have a dumps\pj64\rdram_<tag>.bin golden):
#   cine_frame<N>   e.g. cine_frame78  (recomp ROGUESQ_DUMP_RDRAM_ON_CINE_ITER=N)
#   menu            (recomp ROGUESQ_DUMP_RDRAM_ON_SCREEN=menu; golden rdram_menuinit1_screen9.bin)
#
#   .\tools\validate\checkpoint.ps1 cine_frame78
#   .\tools\validate\checkpoint.ps1 cine_frame120 -Timeout 360
#   .\tools\validate\checkpoint.ps1 cine_frame78 -SkipCapture   # reuse an existing recomp dump
param(
    [Parameter(Mandatory = $true, Position = 0)][string]$Checkpoint,
    [int]$Timeout = 300,
    [string]$Binary = "build\Debug\RogueSquadron64Recomp.exe",
    [string]$Python = "python",
    [switch]$SkipCapture,
    [switch]$SummaryOnly
)
$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path

# Map the checkpoint -> PJ64 golden + the recomp capture trigger.
$captureArgs = @{}
if ($Checkpoint -match '^cine_frame(\d+)$') {
    $golden = "dumps\pj64\rdram_cine_frame$($matches[1]).bin"
    $captureArgs = @{ CineIter = $matches[1] }
}
elseif ($Checkpoint -eq 'menu') {
    $golden = "dumps\pj64\rdram_menuinit1_screen9.bin"
    $captureArgs = @{ Screen = 'menu' }
}
else {
    Write-Output "UNKNOWN checkpoint '$Checkpoint' (expected cine_frame<N> or menu)"; exit 2
}
$goldenAbs = Join-Path $root $golden
$cand = "dumps\recomp\rdram_$Checkpoint.bin"
$candAbs = Join-Path $root $cand

if (-not (Test-Path $goldenAbs)) {
    Write-Output "GOLDEN MISSING: $golden  (capture it with tools\validate\capture_pj64_golden.ps1)"; exit 2
}

# 1. Capture the recomp dump at the landmark (unless reusing one).
if (-not $SkipCapture) {
    New-Item -ItemType Directory -Force (Split-Path $candAbs) | Out-Null
    Write-Output "[capture] running recomp to $Checkpoint (timeout ${Timeout}s)..."
    & (Join-Path $PSScriptRoot 'capture_menu_rdram.ps1') -Out $cand -Timeout $Timeout -Binary $Binary @captureArgs | Out-Null
}
if (-not (Test-Path $candAbs)) {
    Write-Output "CANDIDATE MISSING (capture failed/timed out): $cand  - see logs\capture_menu_rdram.log"; exit 2
}

# 2. LOGIC layer - rdram_golden_diff.py, summarized from its --json.
$logicJson = Join-Path $env:TEMP "rs64_logic_$Checkpoint.json"
& $Python (Join-Path $PSScriptRoot 'rdram_golden_diff.py') $goldenAbs $candAbs --json $logicJson | Out-Null
$lj = ConvertFrom-Json (Get-Content $logicJson -Raw)
$globDiff = @($lj.globals | Where-Object { -not $_.same }).Count
$globTot = @($lj.globals).Count
$ovMatch = ($lj.overlay.golden -eq $lj.overlay.candidate)
$logicPass = ($globDiff -eq 0 -and $ovMatch)

# 3. PARSER layer - dl_diff.py (the desync gate). Capture output + exit code.
$dlOut = & $Python (Join-Path $PSScriptRoot 'dl_diff.py') $goldenAbs $candAbs $(if ($SummaryOnly) { '--summary-only' }) 2>&1
$parserPass = ($LASTEXITCODE -eq 0)

# 4. Pass/fail table.
Write-Output ""
Write-Output "==================== checkpoint: $Checkpoint ===================="
Write-Output ("golden    : {0}" -f $golden)
Write-Output ("candidate : {0}" -f $cand)
Write-Output ("overlay   : golden={0} candidate={1}  {2}" -f $lj.overlay.golden, $lj.overlay.candidate, $(if ($ovMatch) { 'match' } else { 'MISMATCH' }))
Write-Output ("LOGIC  layer : {0}  ({1}/{2} globals differ)" -f $(if ($logicPass) { 'PASS' } else { 'REVIEW' }), $globDiff, $globTot)
Write-Output ("PARSER layer : {0}" -f $(if ($parserPass) { 'PASS  (DL identical to hardware)' } else { 'DIFF  (see below)' }))
Write-Output "-----------------------------------------------------------------"
$dlOut | ForEach-Object { Write-Output $_ }
if (-not $logicPass) {
    Write-Output "-----------------------------------------------------------------"
    Write-Output "LOGIC layer detail: rerun for full report ->"
    Write-Output ("  $Python tools\validate\rdram_golden_diff.py $golden $cand")
}

# Exit: 0 only when both layers pass; 1 if either diverges; capture/setup errors already returned 2.
if ($logicPass -and $parserPass) { exit 0 } else { exit 1 }
