Add-Type -AssemblyName Microsoft.VisualBasic
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class AstraWindowCapture {
    [StructLayout(LayoutKind.Sequential)]
    public struct Rect { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr handle, out Rect rect);
}
'@

$vita = Get-Process Vita3K -ErrorAction Stop | Select-Object -First 1
[Microsoft.VisualBasic.Interaction]::AppActivate($vita.Id) | Out-Null
Start-Sleep -Seconds 2

$rect = New-Object AstraWindowCapture+Rect
if (![AstraWindowCapture]::GetWindowRect($vita.MainWindowHandle, [ref]$rect)) {
    throw 'Could not read the Vita3K window bounds.'
}
$width = $rect.Right - $rect.Left
$height = $rect.Bottom - $rect.Top
$bitmap = New-Object System.Drawing.Bitmap $width, $height
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$graphics.CopyFromScreen(
    (New-Object System.Drawing.Point $rect.Left, $rect.Top),
    [System.Drawing.Point]::Empty,
    (New-Object System.Drawing.Size $width, $height))
$bitmap.Save(
    (Join-Path $PSScriptRoot '..\docs\assets\vita3k-performance-gate.png'),
    [System.Drawing.Imaging.ImageFormat]::Png)
$graphics.Dispose()
$bitmap.Dispose()
