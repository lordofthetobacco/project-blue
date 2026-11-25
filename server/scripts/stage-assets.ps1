# Stages the cooked assets the dedicated server reads into server/assets/
# (gitignored; the Dockerfile copies it into the image):
#   range.mscn, every *.pbo (scene and weapon prefabs, with their relative
#   paths) and every *.cfg (weapon catalogue).
# Render-only files (env/*.hdr, *.fingerprint, *.generated.mscn) are skipped.
#
# Usage: server/scripts/stage-assets.ps1 [-SourceDir <game asset dir>]
#   SourceDir defaults to out/build/x64-release/bin/assets (repo root
#   relative). Only server/assets/ is modified: it is emptied, then filled.
param(
    [string]$SourceDir = ''
)
$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if ($SourceDir -eq '') {
    $SourceDir = Join-Path $repoRoot 'out\build\x64-release\bin\assets'
}
if (-not (Test-Path (Join-Path $SourceDir 'range.mscn'))) {
    Write-Error "$SourceDir\range.mscn not found (build the game first)"
    exit 1
}
$SourceDir = (Resolve-Path $SourceDir).Path.TrimEnd('\')
$targetDir = Join-Path $repoRoot 'server\assets'

New-Item -ItemType Directory -Force -Path $targetDir | Out-Null
# Empty server/assets only (never anything outside it).
Get-ChildItem -LiteralPath $targetDir -Force |
    Remove-Item -Recurse -Force -Confirm:$false

$files = Get-ChildItem -LiteralPath $SourceDir -Recurse -File |
    Where-Object { $_.Name -eq 'range.mscn' -or $_.Extension -in '.pbo', '.cfg' } |
    Sort-Object FullName
$total = 0
foreach ($file in $files) {
    $relative = $file.FullName.Substring($SourceDir.Length + 1)
    $destination = Join-Path $targetDir $relative
    New-Item -ItemType Directory -Force -Path (Split-Path $destination) | Out-Null
    Copy-Item -LiteralPath $file.FullName -Destination $destination
    '{0,12}  {1}' -f $file.Length, $relative
    $total += $file.Length
}
'staged {0} files, {1:N1} MB, into {2}' -f $files.Count, ($total / 1MB), $targetDir
