# Builds the Boneyard's pictures (assets/bones.png, assets/panel.png) from the artist's
# exports: three skeleton views and the panel, 1024 x 1536 and 2732 x 2048 Procreate canvases.
#   powershell -ExecutionPolicy Bypass -File build_assets.ps1 <folder with the four PNGs>
# The skeleton loses its glow, the profile is split into its arm and the rest (skeleton_spec.py),
# and the four are cropped and scaled into one sheet. It prints where each landed: the `pictures`
# table in boneyard_hud.cpp must match.
param([Parameter(Mandatory = $true)][string]$Source)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$assets = Join-Path (Split-Path -Parent $here) 'assets'
Add-Type -TypeDefinition (Get-Content (Join-Path $here 'BoneyardAssets.cs') -Raw) -ReferencedAssemblies System.Drawing

$scale = 0.6
$views = @(
    @{ Name = 'front'; File = 'skeleton front.png'; Crop = New-Object System.Drawing.Rectangle 105, 5, 813, 1528 },
    @{ Name = 'back';  File = 'skeleton back.png';  Crop = New-Object System.Drawing.Rectangle 130, 20, 764, 1510 },
    @{ Name = 'side';  File = 'skeleton side.png';  Crop = New-Object System.Drawing.Rectangle 405, 78, 227, 1415 }
)
$mask = & py (Join-Path $here 'skeleton_spec.py') mask
$pictures = @()
foreach ($v in $views) {
    $art = [BoneyardAssets]::Strip((Join-Path $Source $v.File), 40, 200)
    if ($v.Name -ne 'side') { $pictures += ,@($v.Name, $art, $v.Crop); continue }
    # The profile twice: its arm alone, and the rest without it.
    $pictures += ,@('side arm', [BoneyardAssets]::Keep($art, [string[]]$mask), $v.Crop)
    $pictures += ,@('side, no arm', [BoneyardAssets]::Clear($art, [string[]]$mask), $v.Crop)
    $art.Dispose()
}
$x = 0; $height = 0
foreach ($p in $pictures) { $height = [Math]::Max($height, [int][Math]::Round($p[2].Height * $scale)); $x += [int][Math]::Round($p[2].Width * $scale) + 4 }
$sheet = [BoneyardAssets]::Blank($x - 4, $height)
$x = 0
"sheet $($sheet.Width) x $($sheet.Height); picture: crop x, crop y, at x, at y (scale $scale)"
foreach ($p in $pictures) {
    [BoneyardAssets]::Place($sheet, $p[1], $p[2], $scale, $x, 0)
    "  {0,-12} {1}, {2}, {3}, 0" -f $p[0], $p[2].X, $p[2].Y, $x
    $x += [int][Math]::Round($p[2].Width * $scale) + 4
    $p[1].Dispose()
}
[BoneyardAssets]::Save($sheet, (Join-Path $assets 'bones.png'))
$sheet.Dispose()

# The panel: its artwork cropped to what is drawn, 480 pixels wide.
$panel = New-Object System.Drawing.Bitmap (Join-Path $Source 'boneyard window.png')
$crop = [BoneyardAssets]::Bounds($panel, 16)
$width = 480; $tall = [int][Math]::Round($crop.Height * $width / $crop.Width)
$out = [BoneyardAssets]::Blank($width, $tall)
[BoneyardAssets]::Place($out, $panel, $crop, $width / $crop.Width, 0, 0)
[BoneyardAssets]::Save($out, (Join-Path $assets 'panel.png'))
"panel $width x $tall (from $($crop.X), $($crop.Y), $($crop.Width) x $($crop.Height))"
$out.Dispose(); $panel.Dispose()
