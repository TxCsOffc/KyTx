param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('on', 'off', 'status')]
    [string] $Mode
)

$controlFile = 'D:\PS5\ufc5-color-meta-control.txt'
if ($Mode -eq 'status') {
    if (Test-Path -LiteralPath $controlFile) {
        Get-Content -LiteralPath $controlFile -TotalCount 1
    } else {
        'off (control file absent)'
    }
    return
}

Set-Content -LiteralPath $controlFile -Value $Mode -NoNewline
"Color metadata CPU fill: $Mode (applies within 500 ms)"
