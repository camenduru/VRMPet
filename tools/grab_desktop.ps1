# Grab the desktop (all screens stitched) to a PNG.
#
# The overlay has no window of its own to screenshot - it is a layered,
# click-through, topmost window with no taskbar button - so verifying it means
# capturing the real desktop underneath it and looking.
#
# usage: powershell -File tools/grab_desktop.ps1 <out.png>

param([Parameter(Mandatory = $true)][string]$Out)

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

$bounds = [System.Windows.Forms.SystemInformation]::VirtualScreen
$bmp = New-Object System.Drawing.Bitmap($bounds.Width, $bounds.Height)
$gfx = [System.Drawing.Graphics]::FromImage($bmp)
$gfx.CopyFromScreen($bounds.Location, [System.Drawing.Point]::Empty, $bounds.Size)
$gfx.Dispose()

$dir = Split-Path -Parent $Out
if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }

$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()

Write-Output ("{0}  {1}x{2} at {3},{4}" -f $Out, $bounds.Width, $bounds.Height, $bounds.X, $bounds.Y)
