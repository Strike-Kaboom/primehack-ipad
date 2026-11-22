param([string]$Out)
Add-Type -AssemblyName System.Drawing
$S = 1024
$bmp = New-Object System.Drawing.Bitmap $S, $S
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.SmoothingMode = 'AntiAlias'; $g.InterpolationMode = 'HighQualityBicubic'; $g.PixelOffsetMode = 'HighQuality'
$c = $S / 2

# Background: deep space, radial navy to near-black.
$g.Clear([System.Drawing.Color]::FromArgb(255, 4, 8, 20))
$path = New-Object System.Drawing.Drawing2D.GraphicsPath
$path.AddEllipse(-200, -200, $S + 400, $S + 400)
$bg = New-Object System.Drawing.Drawing2D.PathGradientBrush $path
$bg.CenterColor = [System.Drawing.Color]::FromArgb(255, 18, 40, 78)
$bg.SurroundColors = @([System.Drawing.Color]::FromArgb(255, 3, 6, 16))
$g.FillRectangle($bg, 0, 0, $S, $S)

# Faint star specks (fixed seed so the icon is reproducible).
$rnd = New-Object System.Random 7
for ($i = 0; $i -lt 90; $i++) {
  $x = $rnd.Next(0, $S); $y = $rnd.Next(0, $S); $r = $rnd.Next(1, 4); $a = $rnd.Next(40, 140)
  $g.FillEllipse((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb($a, 200, 220, 255))), $x, $y, $r, $r)
}

function Ring($radius, $width, $color, $start, $sweep) {
  $pen = New-Object System.Drawing.Pen $color, $width
  $pen.StartCap = 'Round'; $pen.EndCap = 'Round'
  $g.DrawArc($pen, $c - $radius, $c - $radius, 2 * $radius, 2 * $radius, $start, $sweep)
}

# Outer glow halo behind the energy ring.
for ($k = 0; $k -lt 14; $k++) {
  Ring (365) (70 - 4 * $k) ([System.Drawing.Color]::FromArgb(6 + $k, 255, 140, 30)) 0 360
}

# Segmented orange energy ring: 6 segments with gaps.
$orange = [System.Drawing.Color]::FromArgb(255, 255, 138, 28)
for ($s = 0; $s -lt 6; $s++) { Ring 365 34 $orange (-80 + 60 * $s) 46 }
# Thin bright inner edge on each segment.
$pale = [System.Drawing.Color]::FromArgb(255, 255, 214, 140)
for ($s = 0; $s -lt 6; $s++) { Ring 344 6 $pale (-78 + 60 * $s) 42 }

# Teal scanner rings.
$teal = [System.Drawing.Color]::FromArgb(230, 60, 230, 210)
Ring 285 8 $teal 200 140
Ring 285 8 $teal 20 140
Ring 250 3 ([System.Drawing.Color]::FromArgb(150, 60, 230, 210)) 0 360

# Tick marks around the teal ring.
$tick = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(200, 90, 240, 220)), 6
for ($d = 0; $d -lt 360; $d += 15) {
  $rad = $d * [Math]::PI / 180; $r1 = 300; $r2 = if ($d % 45 -eq 0) { 330 } else { 315 }
  $g.DrawLine($tick, [float]($c + $r1 * [Math]::Cos($rad)), [float]($c + $r1 * [Math]::Sin($rad)), [float]($c + $r2 * [Math]::Cos($rad)), [float]($c + $r2 * [Math]::Sin($rad)))
}

# Glowing green energy core.
for ($k = 0; $k -lt 18; $k++) {
  $r = 210 - 8 * $k
  $g.FillEllipse((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(8 + 2 * $k, 80, 255, 120))), $c - $r, $c - $r, 2 * $r, 2 * $r)
}
$core = New-Object System.Drawing.Drawing2D.GraphicsPath
$core.AddEllipse($c - 120, $c - 120, 240, 240)
$cb = New-Object System.Drawing.Drawing2D.PathGradientBrush $core
$cb.CenterColor = [System.Drawing.Color]::FromArgb(255, 235, 255, 225)
$cb.SurroundColors = @([System.Drawing.Color]::FromArgb(255, 40, 200, 90))
$g.FillEllipse($cb, $c - 120, $c - 120, 240, 240)

# Crosshair notches pointing at the core.
$cross = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(235, 255, 160, 60)), 14
$cross.StartCap = 'Round'; $cross.EndCap = 'Round'
foreach ($d in 0, 90, 180, 270) {
  $rad = $d * [Math]::PI / 180
  $g.DrawLine($cross, [float]($c + 150 * [Math]::Cos($rad)), [float]($c + 150 * [Math]::Sin($rad)), [float]($c + 215 * [Math]::Cos($rad)), [float]($c + 215 * [Math]::Sin($rad)))
}

# HUD corner brackets.
$br = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(200, 60, 230, 210)), 12
$br.StartCap = 'Round'; $br.EndCap = 'Round'

$g.Dispose()
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
"saved $Out"
