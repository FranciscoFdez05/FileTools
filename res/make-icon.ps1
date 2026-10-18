# Regenerates res/FileTools.ico (PNG-compressed entries, 16-256 px).
# Run with Windows PowerShell 5.1:  powershell -File res\make-icon.ps1
Add-Type -AssemblyName System.Drawing

function New-Color([int]$a, [int]$r, [int]$g, [int]$b) { [System.Drawing.Color]::FromArgb($a, $r, $g, $b) }

function Segment-Path([float]$c, [float]$ro, [float]$ri, [float]$start, [float]$sweep) {
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $path.AddArc($c - $ro, $c - $ro, 2 * $ro, 2 * $ro, $start, $sweep)
    $path.AddArc($c - $ri, $c - $ri, 2 * $ri, 2 * $ri, $start + $sweep, -$sweep)
    $path.CloseFigure()
    return $path
}

# Liquid-glass wheel: a mostly transparent disc with frosted segments and one accent segment.
function Draw-Icon([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap $size, $size
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.PixelOffsetMode = 'HighQuality'
    $s = $size / 256.0
    $c = $size / 2.0
    $r = 120 * $s
    $line = [Math]::Max(1.0, 3 * $s)

    # Clear glass disc: barely tinted, slightly brighter at the top-left.
    $disc = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        [System.Drawing.PointF]::new(0, 0), [System.Drawing.PointF]::new($size, $size),
        (New-Color 95 235 242 255), (New-Color 45 170 185 210))
    $g.FillEllipse($disc, $c - $r, $c - $r, 2 * $r, 2 * $r)

    # Frosted segments with a fine dark edge so they read on light and dark taskbars.
    $ro = 106 * $s; $ri = 46 * $s; $gap = 7.0
    $frost = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        [System.Drawing.PointF]::new(0, 0), [System.Drawing.PointF]::new($size, $size),
        (New-Color 210 255 255 255), (New-Color 150 235 240 250))
    $edge = New-Object System.Drawing.Pen((New-Color 70 40 50 70), [Math]::Max(0.8, 1.6 * $s))
    for ($i = 1; $i -lt 6; $i++) {
        $path = Segment-Path $c $ro $ri (-120 + $i * 60 + $gap / 2) (60 - $gap)
        $g.FillPath($frost, $path)
        if ($size -ge 24) { $g.DrawPath($edge, $path) }
    }
    # Accent segment (the "hovered" option).
    $accent = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        [System.Drawing.PointF]::new($c, $c - $ro), [System.Drawing.PointF]::new($c, $c - $ri),
        (New-Color 240 28 132 230), (New-Color 240 0 96 196))
    $accent.WrapMode = 'TileFlipXY'
    $path = Segment-Path $c $ro $ri (-120 + $gap / 2) (60 - $gap)
    $g.FillPath($accent, $path)

    # Centre well: clear glass with a bright rim.
    $rc = 32 * $s
    $well = New-Object System.Drawing.SolidBrush((New-Color 70 255 255 255))
    $g.FillEllipse($well, $c - $rc, $c - $rc, 2 * $rc, 2 * $rc)
    $wellRim = New-Object System.Drawing.Pen((New-Color 230 255 255 255), $line)
    $g.DrawEllipse($wellRim, $c - $rc, $c - $rc, 2 * $rc, 2 * $rc)

    # Outer rim: bright inner highlight plus a soft dark outline.
    $outline = New-Object System.Drawing.Pen((New-Color 110 30 40 60), [Math]::Max(1.0, 2 * $s))
    $g.DrawEllipse($outline, $c - $r, $c - $r, 2 * $r, 2 * $r)
    $hiBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        [System.Drawing.PointF]::new(0, 0), [System.Drawing.PointF]::new($size, $size),
        (New-Color 250 255 255 255), (New-Color 90 255 255 255))
    $hi = New-Object System.Drawing.Pen($hiBrush, $line)
    $g.DrawEllipse($hi, $c - $r + $line, $c - $r + $line, 2 * ($r - $line), 2 * ($r - $line))

    # Specular glint on the top-left of the glass.
    if ($size -ge 48) {
        $glint = New-Object System.Drawing.Drawing2D.GraphicsPath
        $glint.AddArc($c - $r + 12 * $s, $c - $r + 12 * $s, 2 * ($r - 12 * $s), 2 * ($r - 12 * $s), 200, 45)
        $gp = New-Object System.Drawing.Pen((New-Color 200 255 255 255), [Math]::Max(1.0, 5 * $s))
        $gp.StartCap = 'Round'; $gp.EndCap = 'Round'
        $g.DrawPath($gp, $glint)
    }
    $g.Dispose()
    return $bmp
}
$sizes = 16, 24, 32, 48, 64, 128, 256
$images = foreach ($sz in $sizes) {
    $ms = New-Object System.IO.MemoryStream
    (Draw-Icon $sz).Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    , $ms.ToArray()
}
$out = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter($out)
$w.Write([UInt16]0); $w.Write([UInt16]1); $w.Write([UInt16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $sz = $sizes[$i]
    $dim = if ($sz -ge 256) { 0 } else { $sz }
    $w.Write([byte]$dim); $w.Write([byte]$dim); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([UInt16]1); $w.Write([UInt16]32)
    $w.Write([UInt32]$images[$i].Length); $w.Write([UInt32]$offset)
    $offset += $images[$i].Length
}
foreach ($img in $images) { $w.Write($img) }
$w.Flush()
$path = Join-Path $PSScriptRoot "FileTools.ico"
[System.IO.File]::WriteAllBytes($path, $out.ToArray())
# Also a PNG for the README.
(Draw-Icon 256).Save((Join-Path $PSScriptRoot "..\docs\icon.png"), [System.Drawing.Imaging.ImageFormat]::Png)
"wrote $path"
