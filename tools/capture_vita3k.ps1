Add-Type -AssemblyName Microsoft.VisualBasic
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$vita = Get-Process Vita3K -ErrorAction Stop | Select-Object -First 1
[Microsoft.VisualBasic.Interaction]::AppActivate($vita.Id) | Out-Null
Start-Sleep -Seconds 2

$bounds = [System.Windows.Forms.SystemInformation]::VirtualScreen
$bitmap = New-Object System.Drawing.Bitmap $bounds.Width, $bounds.Height
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$graphics.CopyFromScreen($bounds.Location, [System.Drawing.Point]::Empty, $bounds.Size)
$bitmap.Save(
    'C:\Users\amir\AppData\Local\Temp\astrarecomp-generated-vita3k-clean.png',
    [System.Drawing.Imaging.ImageFormat]::Png)
$graphics.Dispose()
$bitmap.Dispose()
