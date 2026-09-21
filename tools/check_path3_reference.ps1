param([Parameter(Mandatory=$true)][string]$ImagePath, [switch]$Perspective,
      [switch]$Highlight, [switch]$RegionRepeat, [switch]$Feedback)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$bitmap = [System.Drawing.Bitmap]::new((Resolve-Path -LiteralPath $ImagePath).Path)
try {
    if ($Feedback) {
        if ($bitmap.Width -lt 16 -or $bitmap.Height -lt 8) {
            throw 'Feedback output is smaller than the 16x8 fixture region.'
        }
        $mismatches = 0
        for ($y = 0; $y -lt 8; $y++) {
            for ($x = 0; $x -lt 16; $x++) {
                $expected = if ($x -lt 8) { 0xFF0000 } else { 0x0000FF }
                if (($bitmap.GetPixel($x, $y).ToArgb() -band 0xFFFFFF) -ne $expected) {
                    $mismatches++
                }
            }
        }
        Write-Output "Feedback native RGB mismatches: $mismatches / 128 (alpha constrained by guest EQUAL 64 test)"
        if ($mismatches -ne 0) { throw 'Framebuffer-feedback reference comparison failed.' }
        return
    }
    if ($bitmap.Width -lt 128 -or $bitmap.Height -lt 128) {
        throw 'Reference render-target crop is smaller than the 128x128 fixture region.'
    }
    $mismatches = 0
    $quarterGridMismatches = 0
    # Compare RGB only, over the defined native fixture region. PCSX2 may
    # include a padding row/column in the RT crop; those are not golden pixels.
    for ($y = 0; $y -lt 128; $y++) {
        for ($x = 0; $x -lt 128; $x++) {
            $green = if ($Perspective) { 3 * $x -ge 128 } else { $x -ge 64 }
            $blue = if ($Perspective) { 2 * $y -ge 128 + $x } else { $y -ge 64 }
            $expected = if ($x + $y -ge 128) { 0 } elseif ($green) {
                0x00FF00
            } elseif ($blue) { 0x0000FF } else { 0xFF0000 }
            if ($Highlight -and $x + $y -lt 128) { $expected = $expected -bor 0x404040 }
            if ($RegionRepeat -and $x + $y -lt 128) {
                $expected = if (([Math]::Floor($y / 32) % 2) -eq 1) { 0xFFFFFF } else { 0x00FF00 }
            }
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
