param([Parameter(Mandatory=$true)][string]$ImagePath)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$bitmap = [System.Drawing.Bitmap]::new((Resolve-Path -LiteralPath $ImagePath).Path)
try {
    if ($bitmap.Width -lt 128 -or $bitmap.Height -lt 128) {
        throw 'Reference render-target crop is smaller than the 128x128 fixture region.'
    }
    $mismatches = 0
    $quarterGridMismatches = 0
    # Compare RGB only, over the defined native fixture region. PCSX2 may
    # include a padding row/column in the RT crop; those are not golden pixels.
    for ($y = 0; $y -lt 128; $y++) {
        for ($x = 0; $x -lt 128; $x++) {
            $expected = if ($x + $y -ge 128) { 0 } elseif ($x -ge 64) {
                0x00FF00
            } elseif ($y -ge 64) { 0x0000FF } else { 0xFF0000 }
            $actual = $bitmap.GetPixel($x, $y).ToArgb() -band 0xFFFFFF
            if ($actual -ne $expected) {
                $mismatches++
                if (($x % 4 -eq 0) -and ($y % 4 -eq 0)) { $quarterGridMismatches++ }
            }
        }
    }
    Write-Output "Native RGB mismatches: $mismatches / 16384; quarter-grid mismatches: $quarterGridMismatches / 1024"
    if ($mismatches -ne 0) { throw 'PATH3 reference RGB comparison failed.' }
} finally {
    $bitmap.Dispose()
}
