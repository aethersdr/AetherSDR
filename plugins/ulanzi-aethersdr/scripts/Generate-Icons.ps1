# Generate-Icons.ps1
#
# Programmatic PNG icon generator for the AetherSDR Ulanzi Studio plugin.
# Replaces the earlier SVG generator (generate-icons.js) because the Ulanzi
# device LCD renderer strips <text> elements from SVGs.  Studio's own
# marketplace convention is 196x196 PNG with baked-in graphics + text —
# we follow that here.
#
# Tooling: pure Windows PowerShell + System.Drawing.  No npm, no Node, no
# external dependencies.  Runs in a few seconds.
#
#   pwsh scripts/Generate-Icons.ps1     # PowerShell 7+ (preferred)
#   powershell scripts/Generate-Icons.ps1   # Windows PowerShell 5.1 also fine
#
# Outputs:
#   com.g0jkn.aethersdr.ulanziPlugin/assets/icons/*.png      (18 action tiles)
#   com.g0jkn.aethersdr.ulanziPlugin/assets/launchers/*.png  (5 launcher tiles)
#   plus pluginIcon.png + category.png for Studio's picker

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$scriptDir  = Split-Path -Parent $MyInvocation.MyCommand.Path
$pluginRoot = Join-Path (Split-Path -Parent $scriptDir) 'com.g0jkn.aethersdr.ulanziPlugin'
$outAction   = Join-Path $pluginRoot 'assets\icons'
$outLauncher = Join-Path $pluginRoot 'assets\launchers'

# ─── Palette ─────────────────────────────────────────────────────────────
# Same palette as the SVG generator — matches AetherSDR dark theme.
$PALETTE = @{
  tx     = '#c0394b'   # TX / MOX
  tune   = '#d9853b'   # ATU tune
  rx     = '#1ea672'   # RX-side: mode/slice/rit
  band   = '#3a78c2'   # band navigation
  gain   = '#9b7fc2'   # AF / RF / mic gain
  spec   = '#00b4d8'   # VFO / spectrum (cyan brand)
  bg     = '#0f0f1a'   # dark backdrop
  fg     = '#ffffff'
}

# ─── Action catalogue ────────────────────────────────────────────────────
# Each: name(filename stem) / label(main text) / color(palette key) / sub(optional secondary)
$ICONS = @(
  @{ name = 'mox';            label = 'MOX';   color = 'tx'   }
  @{ name = 'tune';           label = 'TUNE';  color = 'tune' }
  @{ name = 'rit';            label = 'RIT';   color = 'rx'   }
  @{ name = 'mode';           label = 'MODE';  color = 'rx'   }
  @{ name = 'slice';          label = 'SLICE'; color = 'rx'   }
  @{ name = 'band-up';        label = 'BAND';  color = 'band'; sub = [char]0x25B2 }  # ▲
  @{ name = 'band-down';      label = 'BAND';  color = 'band'; sub = [char]0x25BC }  # ▼
  @{ name = 'vfo';            label = 'VFO';   color = 'spec' }

  @{ name = 'mode-usb';       label = 'USB';   color = 'rx'   }
  @{ name = 'mode-lsb';       label = 'LSB';   color = 'rx'   }
  @{ name = 'mode-cw';        label = 'CW';    color = 'rx'   }
  @{ name = 'mode-digu';      label = 'DIGU';  color = 'rx'   }

  @{ name = 'af-gain-up';     label = 'AF';    color = 'gain'; sub = [char]0x25B2 }
  @{ name = 'af-gain-down';   label = 'AF';    color = 'gain'; sub = [char]0x25BC }
  @{ name = 'rf-gain-up';     label = 'RF';    color = 'gain'; sub = [char]0x25B2 }
  @{ name = 'rf-gain-down';   label = 'RF';    color = 'gain'; sub = [char]0x25BC }
  @{ name = 'mic-gain-up';    label = 'MIC';   color = 'gain'; sub = [char]0x25B2 }
  @{ name = 'mic-gain-down';  label = 'MIC';   color = 'gain'; sub = [char]0x25BC }
)

# ─── Launcher catalogue ──────────────────────────────────────────────────
# Two-line wordmarks on dark bg with accent border + ↗ glyph.
$LAUNCHERS = @(
  @{ name = 'aethersdr';   line1 = 'AETHER'; line2 = 'SDR';  accent = '#00b4d8' }
  @{ name = 'tci-monitor'; line1 = 'TCI';    line2 = 'MON';  accent = '#56c6e8' }
  @{ name = 'shacklog';    line1 = 'SHACK';  line2 = 'LOG';  accent = '#1ea672' }
  @{ name = 'iq-capture';  line1 = 'IQ';     line2 = 'CAP';  accent = '#e85d75' }
  @{ name = 'aether-pad';  line1 = 'AETHER'; line2 = 'PAD';  accent = '#9b7fc2' }
)

# ─── Rendering helpers ───────────────────────────────────────────────────

# Pick a font point-size that fills the tile without overflowing horizontally.
# Empirical values tuned against 196x196 with Arial Bold — conservative enough
# to avoid auto-wrap even for wide glyphs (M, W).
function Get-LabelSize {
  param([string]$Text)
  switch ($Text.Length) {
    1 { 120 }
    2 {  90 }
    3 {  62 }
    4 {  48 }
    5 {  40 }
    default { 32 }
  }
}

# Standard centered StringFormat with auto-wrap DISABLED.  Anything that
# overflows the rect just gets clipped — much better failure mode than
# unexpected line-wrapping into "MO" / "X" etc.
function New-CenterFormat {
  $sf = New-Object System.Drawing.StringFormat
  $sf.Alignment     = [System.Drawing.StringAlignment]::Center
  $sf.LineAlignment = [System.Drawing.StringAlignment]::Center
  $sf.FormatFlags   = [System.Drawing.StringFormatFlags]::NoWrap
  return $sf
}

# Build a rounded-rectangle GraphicsPath inside the given rectangle.
function New-RoundedRectPath {
  param([int]$x, [int]$y, [int]$w, [int]$h, [int]$radius)
  $path = New-Object System.Drawing.Drawing2D.GraphicsPath
  $d = $radius * 2
  $path.AddArc($x,               $y,               $d, $d, 180, 90)
  $path.AddArc($x + $w - $d,     $y,               $d, $d, 270, 90)
  $path.AddArc($x + $w - $d,     $y + $h - $d,     $d, $d,   0, 90)
  $path.AddArc($x,               $y + $h - $d,     $d, $d,  90, 90)
  $path.CloseFigure()
  return $path
}

function New-HQGraphics {
  param([System.Drawing.Bitmap]$Bitmap)
  $g = [System.Drawing.Graphics]::FromImage($Bitmap)
  $g.SmoothingMode      = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
  $g.InterpolationMode  = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
  $g.PixelOffsetMode    = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
  $g.TextRenderingHint  = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
  return $g
}

# ─── Action icon renderer ────────────────────────────────────────────────
function Render-ActionIcon {
  param([hashtable]$Icon)

  $size   = 196
  $radius = 28
  $bgHex  = $PALETTE[$Icon.color]
  if (-not $bgHex) { $bgHex = $PALETTE.bg }
  $bg = [System.Drawing.ColorTranslator]::FromHtml($bgHex)
  $fg = [System.Drawing.ColorTranslator]::FromHtml($PALETTE.fg)

  $bmp = New-Object System.Drawing.Bitmap($size, $size)
  $g   = New-HQGraphics -Bitmap $bmp

  # Rounded-rect background
  $path = New-RoundedRectPath -x 0 -y 0 -w $size -h $size -radius $radius
  $brush = New-Object System.Drawing.SolidBrush($bg)
  $g.FillPath($brush, $path)
  $brush.Dispose()
  $path.Dispose()

  # Main label
  $labelSize = Get-LabelSize -Text $Icon.label
  $font = New-Object System.Drawing.Font('Arial', $labelSize, [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
  $sf = New-CenterFormat
  $textBrush = New-Object System.Drawing.SolidBrush($fg)

  # If we have a sub-glyph, shift label up and put glyph below.  Otherwise centre.
  if ($Icon.ContainsKey('sub')) {
    $labelRect = New-Object System.Drawing.RectangleF(0, 8, $size, 116)
    $g.DrawString($Icon.label, $font, $textBrush, $labelRect, $sf)

    $subFont = New-Object System.Drawing.Font('Arial', 48, [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
    $subRect = New-Object System.Drawing.RectangleF(0, 124, $size, 60)
    $g.DrawString($Icon.sub, $subFont, $textBrush, $subRect, $sf)
    $subFont.Dispose()
  } else {
    $labelRect = New-Object System.Drawing.RectangleF(0, 0, $size, $size)
    $g.DrawString($Icon.label, $font, $textBrush, $labelRect, $sf)
  }

  $font.Dispose()
  $textBrush.Dispose()
  $g.Dispose()
  return $bmp
}

# ─── Launcher tile renderer ──────────────────────────────────────────────
function Render-LauncherIcon {
  param([hashtable]$Launcher)

  $size   = 196
  $radius = 28
  $bg     = [System.Drawing.ColorTranslator]::FromHtml($PALETTE.bg)
  $accent = [System.Drawing.ColorTranslator]::FromHtml($Launcher.accent)
  $fg     = [System.Drawing.ColorTranslator]::FromHtml($PALETTE.fg)

  $bmp = New-Object System.Drawing.Bitmap($size, $size)
  $g   = New-HQGraphics -Bitmap $bmp

  # Background + thin accent border
  $path = New-RoundedRectPath -x 0 -y 0 -w $size -h $size -radius $radius
  $bgBrush = New-Object System.Drawing.SolidBrush($bg)
  $g.FillPath($bgBrush, $path)
  $bgBrush.Dispose()

  $borderPath = New-RoundedRectPath -x 3 -y 3 -w ($size - 6) -h ($size - 6) -radius ($radius - 3)
  $borderPen = New-Object System.Drawing.Pen($accent, 3)
  $borderPen.Alignment = [System.Drawing.Drawing2D.PenAlignment]::Center
  $g.DrawPath($borderPen, $borderPath)
  $borderPen.Dispose()
  $borderPath.Dispose()
  $path.Dispose()

  $sf = New-CenterFormat

  # ↗ launcher glyph top-right
  $arrowFont = New-Object System.Drawing.Font('Arial', 28, [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
  $accentBrush = New-Object System.Drawing.SolidBrush($accent)
  $arrowRect = New-Object System.Drawing.RectangleF(($size - 50), 14, 36, 36)
  $g.DrawString([char]0x2197, $arrowFont, $accentBrush, $arrowRect, $sf)
  $arrowFont.Dispose()

  # Two-line wordmark — line1 in accent colour, line2 in white
  $lineSize1 = Get-LabelSize -Text $Launcher.line1
  $lineSize2 = Get-LabelSize -Text $Launcher.line2
  $lineFont1 = New-Object System.Drawing.Font('Arial', ([Math]::Min($lineSize1, 64)), [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
  $lineFont2 = New-Object System.Drawing.Font('Arial', ([Math]::Min($lineSize2, 64)), [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)

  $whiteBrush = New-Object System.Drawing.SolidBrush($fg)
  $line1Rect = New-Object System.Drawing.RectangleF(0, 60, $size, 60)
  $line2Rect = New-Object System.Drawing.RectangleF(0, 120, $size, 60)
  $g.DrawString($Launcher.line1, $lineFont1, $accentBrush, $line1Rect, $sf)
  $g.DrawString($Launcher.line2, $lineFont2, $whiteBrush,  $line2Rect, $sf)
  $lineFont1.Dispose()
  $lineFont2.Dispose()
  $accentBrush.Dispose()
  $whiteBrush.Dispose()

  $g.Dispose()
  return $bmp
}

# ─── Plugin + category icons ─────────────────────────────────────────────
function Render-PluginIcon {
  $size = 196
  $bg     = [System.Drawing.ColorTranslator]::FromHtml($PALETTE.bg)
  $accent = [System.Drawing.ColorTranslator]::FromHtml($PALETTE.spec)
  $fg     = [System.Drawing.ColorTranslator]::FromHtml($PALETTE.fg)

  $bmp = New-Object System.Drawing.Bitmap($size, $size)
  $g   = New-HQGraphics -Bitmap $bmp
  $path = New-RoundedRectPath -x 0 -y 0 -w $size -h $size -radius 28
  $bgBrush = New-Object System.Drawing.SolidBrush($bg)
  $g.FillPath($bgBrush, $path)
  $bgBrush.Dispose()
  $path.Dispose()

  $pen = New-Object System.Drawing.Pen($accent, 6)
  $g.DrawEllipse($pen, 60, 50, 76, 76)
  $pen.Dispose()
  $accentBrush = New-Object System.Drawing.SolidBrush($accent)
  $g.FillEllipse($accentBrush, 84, 74, 28, 28)

  $sf = New-CenterFormat
  $font = New-Object System.Drawing.Font('Arial', 24, [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
  $whiteBrush = New-Object System.Drawing.SolidBrush($fg)
  $textRect = New-Object System.Drawing.RectangleF(0, 150, $size, 36)
  $g.DrawString('AetherSDR', $font, $whiteBrush, $textRect, $sf)
  $font.Dispose()
  $whiteBrush.Dispose()
  $accentBrush.Dispose()
  $g.Dispose()
  return $bmp
}

function Render-CategoryIcon {
  $size = 196
  $bg     = [System.Drawing.ColorTranslator]::FromHtml($PALETTE.bg)
  $accent = [System.Drawing.ColorTranslator]::FromHtml($PALETTE.spec)
  $bmp = New-Object System.Drawing.Bitmap($size, $size)
  $g   = New-HQGraphics -Bitmap $bmp
  $path = New-RoundedRectPath -x 0 -y 0 -w $size -h $size -radius 28
  $bgBrush = New-Object System.Drawing.SolidBrush($bg)
  $g.FillPath($bgBrush, $path)
  $bgBrush.Dispose()
  $path.Dispose()
  $sf = New-CenterFormat
  $font = New-Object System.Drawing.Font('Arial', 110, [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
  $accentBrush = New-Object System.Drawing.SolidBrush($accent)
  $textRect = New-Object System.Drawing.RectangleF(0, 0, $size, $size)
  $g.DrawString('AE', $font, $accentBrush, $textRect, $sf)
  $font.Dispose()
  $accentBrush.Dispose()
  $g.Dispose()
  return $bmp
}

# ─── Main ────────────────────────────────────────────────────────────────
if (-not (Test-Path $outAction))   { New-Item -ItemType Directory -Path $outAction   | Out-Null }
if (-not (Test-Path $outLauncher)) { New-Item -ItemType Directory -Path $outLauncher | Out-Null }

"Action icons (196x196 PNG):"
foreach ($icon in $ICONS) {
  $bmp = Render-ActionIcon -Icon $icon
  $out = Join-Path $outAction "$($icon.name).png"
  $bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
  $bmp.Dispose()
  $sub = if ($icon.ContainsKey('sub')) { " $($icon.sub)" } else { "" }
  "  {0,-18} {1,-6} {2}{3}" -f $icon.name, $icon.color, $icon.label, $sub
}

# Plugin + category icons
$pluginBmp = Render-PluginIcon
$pluginBmp.Save((Join-Path $outAction 'pluginIcon.png'), [System.Drawing.Imaging.ImageFormat]::Png)
$pluginBmp.Dispose()
$catBmp = Render-CategoryIcon
$catBmp.Save((Join-Path $outAction 'category.png'), [System.Drawing.Imaging.ImageFormat]::Png)
$catBmp.Dispose()

""
"Launcher tiles (for Studio's System -> Open action):"
foreach ($tile in $LAUNCHERS) {
  $bmp = Render-LauncherIcon -Launcher $tile
  $out = Join-Path $outLauncher "$($tile.name).png"
  $bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
  $bmp.Dispose()
  "  {0,-18} {1,7}  {2} {3}" -f $tile.name, $tile.accent, $tile.line1, $tile.line2
}

""
"Generated $($ICONS.Count + 2) action PNGs in $outAction"
"Generated $($LAUNCHERS.Count) launcher PNGs in $outLauncher"
