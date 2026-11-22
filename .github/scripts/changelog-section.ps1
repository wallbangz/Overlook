# Writes one version's section of CHANGELOG.md (without its heading) to a file, for release notes.
# Usage: changelog-section.ps1 -Version 0.2.0 -Out notes.md    (a leading "v" is ignored)
param([Parameter(Mandatory)][string]$Version, [Parameter(Mandatory)][string]$Out)

$v = $Version.TrimStart('v')
$lines = Get-Content (Join-Path $PSScriptRoot '../../CHANGELOG.md') -Encoding utf8
$start = -1
for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match "^## \[$([regex]::Escape($v))\]") { $start = $i + 1; break }
}
if ($start -lt 0) { [Console]::Error.WriteLine("CHANGELOG.md has no section for $v"); exit 1 }
$end = $lines.Count
for ($i = $start; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match '^## ' -or $lines[$i] -match '^\[[^\]]+\]: ') { $end = $i; break }
}
$body = ($lines[$start..($end - 1)] -join "`n").Trim()
if (-not $body) { [Console]::Error.WriteLine("The $v section of CHANGELOG.md is empty"); exit 1 }
Set-Content -Path $Out -Value $body -Encoding utf8 -NoNewline   # no trailing CRLF in the notes
exit 0
