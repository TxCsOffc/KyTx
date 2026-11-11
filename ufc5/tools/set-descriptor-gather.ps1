[CmdletBinding()]
param(
    [Parameter(Mandatory)] [ValidateSet('off','shadow','on')] [string] $Mode,
    [string] $ControlFile = 'D:\PS5\ufc5-descriptor-gather.control'
)
$ErrorActionPreference = 'Stop'
# Rename a complete file so the running GPU thread never sees a partial command.
$taskControlPath = [IO.Path]::GetFullPath($ControlFile)
$taskTempPath = $taskControlPath + '.new'
Set-Content -LiteralPath $taskTempPath -Value $Mode -Encoding Ascii
Move-Item -LiteralPath $taskTempPath -Destination $taskControlPath -Force
Write-Output "Descriptor experiment requested: $Mode. Applied within about 500 ms at the selected shader."
if ($Mode -eq 'on') { Write-Output 'Unvalidated resource plans stay on the reference path; any mismatch disables the experiment.' }
