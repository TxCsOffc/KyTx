[CmdletBinding()]
param(
    [Parameter(Mandatory)] [ValidateSet('off','shadow','on')] [string] $Mode,
    [ValidatePattern('^(all|[0-9a-fA-F]{16})$')] [string] $Shader,
    [string] $ControlFile = 'D:\PS5\ufc5-flat-srt.control'
)
$ErrorActionPreference = 'Stop'
$taskControlPath = [IO.Path]::GetFullPath($ControlFile)
$taskTempPath = $taskControlPath + '.new'
$taskCommand = if ($Shader) { "$Mode $($Shader.ToLowerInvariant())" } else { $Mode }
Set-Content -LiteralPath $taskTempPath -Value $taskCommand -Encoding Ascii
Move-Item -LiteralPath $taskTempPath -Destination $taskControlPath -Force
Write-Output "FlatSRT experiment requested: $taskCommand. Applied within about 500 ms; omitted shader preserves the current scope."
if ($Mode -eq 'on') { Write-Output 'Unvalidated plans stay on reference evaluation; a mismatch rejects the candidate.' }
