[CmdletBinding()]
param(
    [Parameter(Mandatory)] [ValidateSet('off','shadow','on')] [string] $Mode,
    [string] $ControlFile = 'D:\PS5\ufc5-bda-sync.control'
)
$ErrorActionPreference = 'Stop'
$taskControlPath = [IO.Path]::GetFullPath($ControlFile)
$taskTempPath = $taskControlPath + '.new'
Set-Content -LiteralPath $taskTempPath -Value $Mode -Encoding Ascii
Move-Item -LiteralPath $taskTempPath -Destination $taskControlPath -Force
Write-Output "BDA sync requested: $Mode. Applies within about 500 ms."
if ($Mode -ne 'off') {
    Write-Output 'Shadow executes reference uploads and checks candidate coverage. On requires successful validation; stable mismatches disable the candidate for this process. Concurrent-write mismatches are inconclusive.'
}
