[CmdletBinding()]
param(
    [Parameter(Mandatory)] [ValidateSet('off','shadow','on')] [string] $Mode,
    [string] $ControlFile = 'D:\PS5\ufc5-compiled-srt.control'
)
$ErrorActionPreference = 'Stop'
$taskControlPath = [IO.Path]::GetFullPath($ControlFile)
$taskTempPath = $taskControlPath + '.new'
Set-Content -LiteralPath $taskTempPath -Value $Mode -Encoding Ascii
Move-Item -LiteralPath $taskTempPath -Destination $taskControlPath -Force
Write-Output "Compiled SRT requested: $Mode. Applies within about 500 ms; all shader plans are in scope."
if ($Mode -ne 'off') { Write-Output 'Plans require a successful full comparison before enabling. Mismatches disable the candidate for this process.' }
