param([string]$OutIco, [string]$OutPng)
Add-Type -AssemblyName System.Drawing

function Draw-Icon([int]$S) {
    $bmp = New-Object System.Drawing.Bitmap $S, $S, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.Clear([System.Drawing.Color]::Transparent)
    # rounded dark square
    $r = [single]($S * 0.22)
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $m = [single]($S * 0.04); $w = [single]($S - 2 * $m)
    $path.AddArc($m, $m, $r, $r, 180, 90)
    $path.AddArc($m + $w - $r, $m, $r, $r, 270, 90)
    $path.AddArc($m + $w - $r, $m + $w - $r, $r, $r, 0, 90)
    $path.AddArc($m, $m + $w - $r, $r, $r, 90, 90)
    $path.CloseFigure()
    $bg = New-Object System.Drawing.Drawing2D.LinearGradientBrush((New-Object System.Drawing.PointF 0, 0), (New-Object System.Drawing.PointF 0, $S),
        [System.Drawing.Color]::FromArgb(255, 34, 38, 46), [System.Drawing.Color]::FromArgb(255, 16, 18, 22))
    $g.FillPath($bg, $path)
    # lens ring
    $c = [single]($S / 2)
    $ro = [single]($S * 0.31)
    $pw = [single]([Math]::Max(1.5, $S * 0.085))
    $ring = New-Object System.Drawing.Drawing2D.LinearGradientBrush((New-Object System.Drawing.PointF 0, ($c - $ro - $pw)), (New-Object System.Drawing.PointF 0, ($c + $ro + $pw)),
        [System.Drawing.Color]::FromArgb(255, 130, 186, 255), [System.Drawing.Color]::FromArgb(255, 70, 120, 235))
    $pen = New-Object System.Drawing.Pen($ring, $pw)
    $g.DrawEllipse($pen, $c - $ro, $c - $ro, 2 * $ro, 2 * $ro)
    # inner glass with a highlight
    $ri = [single]($S * 0.15)
    $glass = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 110, 168, 255))
    $g.FillEllipse($glass, $c - $ri, $c - $ri, 2 * $ri, 2 * $ri)
    if ($S -ge 32) {
        $rh = [single]($S * 0.055)
        $hl = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(230, 235, 245, 255))
        $g.FillEllipse($hl, $c - $ri * 0.55 - $rh, $c - $ri * 0.55 - $rh, 2 * $rh, 2 * $rh)
    }
    $g.Dispose()
    return $bmp
}

$sizes = 16, 24, 32, 48, 64, 128, 256
$pngs = @()
foreach ($s in $sizes) {
    $b = Draw-Icon $s
    $ms = New-Object System.IO.MemoryStream
    $b.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    if ($s -eq 256) { $b.Save($OutPng, [System.Drawing.Imaging.ImageFormat]::Png) }
    $pngs += , $ms.ToArray()
    $b.Dispose()
}
# ICO container with PNG-compressed images
$fs = [System.IO.File]::Create($OutIco)
$bw = New-Object System.IO.BinaryWriter $fs
$bw.Write([UInt16]0); $bw.Write([UInt16]1); $bw.Write([UInt16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]; $d = $pngs[$i]
    $bw.Write([byte]($(if ($s -ge 256) { 0 } else { $s }))); $bw.Write([byte]($(if ($s -ge 256) { 0 } else { $s })))
    $bw.Write([byte]0); $bw.Write([byte]0); $bw.Write([UInt16]1); $bw.Write([UInt16]32)
    $bw.Write([UInt32]$d.Length); $bw.Write([UInt32]$offset)
    $offset += $d.Length
}
foreach ($d in $pngs) { $bw.Write($d) }
$bw.Close()
"ok"
