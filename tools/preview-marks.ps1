# Draws the combat marks (the target ring, the called blow's arrow, the bursts and the count) over two eye
# captures of a VR session, without a headset, and writes PNG sheets of their states and poses.
#
#   tools\preview-marks.ps1 -Far <capture.png> -FarAt 337,262 -FarMetres 2.6 `
#                           -Near <capture.png> -NearAt 300,400 -NearMetres 0.75
#
# A capture is one of a session report's `*-eyes\NNNN-view.png` (512 pixels wide). -FarAt and -NearAt are the
# capture's pixel at the enemy's chest; the metres, how far off he is. Without a capture the backdrop is plain,
# dark to the left and pale to the right. The sheets go to -Out (reports\mark-preview, or reports\mark-preview-tech
# for -Style Tech, the SPIDY VR tab's UI STYLE): for each of the two, `marks.png` (the ring and the calls, at rest
# and as they land), `blows.png` (the bursts and the count, pose by pose), `sense.png`, `aim.png` (the aim markers)
# and `eye.png` (the whole eye image with a call on him), and for the near one `wheel.png`, `meter.png` (slow
# motion's) and `panel.png` (the calibration's), at about the headset's pixel density. -BuildDir takes a build
# folder of its own instead of build\<Build>.
param([string]$Far, [int[]]$FarAt = @(256, 272), [double]$FarMetres = 2.6,
      [string]$Near, [int[]]$NearAt = @(256, 272), [double]$NearMetres = 0.75,
      [string]$Out, [ValidateRange(1, 8)][int]$Scale = 4, [string]$Build = 'windows-ninja',
      [ValidateSet('Comic', 'Tech')][string]$Style = 'Comic', [string]$BuildDir)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = if ($BuildDir) { Join-Path $BuildDir 'spidy_mark_preview.exe' } else { Join-Path $root "build\$Build\spidy_mark_preview.exe" }
if (-not (Test-Path -LiteralPath $exe)) { throw "Build first (tools\build.ps1): $exe is missing." }
if (-not $Out) { $Out = Join-Path $root $(if ($Style -eq 'Tech') { 'reports\mark-preview-tech' } else { 'reports\mark-preview' }) }
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path -LiteralPath $Out).Path
Add-Type -AssemblyName System.Drawing
# The capture enlarged to about the headset's pixel density, as a 24-bit BMP.
function Backdrop([string]$png, [string]$name) {
    if (-not $png) { return '-' }
    $bmp = Join-Path $Out "$name-backdrop.bmp"
    $image = [System.Drawing.Image]::FromFile((Resolve-Path -LiteralPath $png).Path)
    try {
        $big = New-Object System.Drawing.Bitmap ($image.Width * $Scale), ($image.Height * $Scale), ([System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
        $graphics = [System.Drawing.Graphics]::FromImage($big)
        $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $graphics.DrawImage($image, 0, 0, $big.Width, $big.Height)
        $graphics.Dispose()
        $big.Save($bmp, [System.Drawing.Imaging.ImageFormat]::Bmp)
        $big.Dispose()
    } finally { $image.Dispose() }
    return $bmp
}
if ($FarAt.Count -ne 2 -or $NearAt.Count -ne 2) { throw '-FarAt and -NearAt take two numbers: x,y' }
$number = [System.Globalization.CultureInfo]::InvariantCulture
& $exe $Out $Scale (Backdrop $Far 'far') $FarAt[0] $FarAt[1] $FarMetres.ToString($number) `
    (Backdrop $Near 'near') $NearAt[0] $NearAt[1] $NearMetres.ToString($number) $Style.ToLowerInvariant()
if ($LASTEXITCODE) { throw "spidy_mark_preview failed ($LASTEXITCODE)" }
Get-ChildItem -LiteralPath $Out -Filter *.bmp | ForEach-Object {
    if ($_.Name -notlike '*-backdrop.bmp') {
        $image = [System.Drawing.Image]::FromFile($_.FullName)
        $image.Save([System.IO.Path]::ChangeExtension($_.FullName, '.png'), [System.Drawing.Imaging.ImageFormat]::Png)
        $image.Dispose()
    }
    Remove-Item -LiteralPath $_.FullName
}
Get-ChildItem -LiteralPath $Out -Filter *.png | ForEach-Object { $_.FullName }
