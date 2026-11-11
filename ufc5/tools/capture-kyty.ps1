<#
.SYNOPSIS
    Capture the running kyty_emulator window to D:\PS5\KytyWindow.png

.DESCRIPTION
    Uses CopyFromScreen (not PrintWindow) so Vulkan swapchain contents are visible.
    Dot-source is not required; run it as a script.
#>
[CmdletBinding()]
param(
    [string]$OutPath = 'D:\PS5\KytyWindow.png'
)

Add-Type -AssemblyName System.Drawing
if (-not ('KytyCapture.Native' -as [type])) {
    Add-Type @"
using System;
using System.Runtime.InteropServices;
namespace KytyCapture {
  public static class Native {
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr hWnd);
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
  }
}
"@
}

$proc = Get-Process -Name 'kyty_emulator*' -ErrorAction SilentlyContinue |
    Where-Object { $_.MainWindowHandle -ne [IntPtr]::Zero } | Select-Object -First 1
if (-not $proc) { throw 'kyty_emulator is not running' }
$hwnd = $proc.MainWindowHandle
if ($hwnd -eq [IntPtr]::Zero) { throw 'kyty_emulator has no main window' }
if ([KytyCapture.Native]::IsIconic($hwnd)) { [void][KytyCapture.Native]::ShowWindow($hwnd, 9) }
[void][KytyCapture.Native]::SetForegroundWindow($hwnd)
Start-Sleep -Milliseconds 250

$rect = New-Object KytyCapture.Native+RECT
[void][KytyCapture.Native]::GetWindowRect($hwnd, [ref]$rect)
$w = $rect.Right - $rect.Left
$h = $rect.Bottom - $rect.Top
if ($w -le 0 -or $h -le 0) { throw "invalid window size ${w}x${h}" }

$bmp = New-Object System.Drawing.Bitmap $w, $h
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($rect.Left, $rect.Top, 0, 0, (New-Object System.Drawing.Size $w, $h))
$g.Dispose()
$bmp.Save($OutPath, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Output ("{0}  {1}x{2}  {3}" -f $proc.MainWindowTitle, $w, $h, $OutPath)
