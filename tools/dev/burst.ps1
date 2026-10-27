# Burst-capture the screen while pressing a debug-pad button (needs [debug] remote_pad=1):
# ~20 captures/s of the centre into $env:TEMP\mml-burst, for catching single-frame glitches.
#   burst.ps1 -Count 60 -Btn 0x4000   (0x4000 = X = roll in the modern layout)
param([int]$Count = 60, [int]$DelayMs = 0, [string]$Out = "$env:TEMP\mml-burst", [int]$Btn = 0x4000)
Add-Type -ReferencedAssemblies System.Drawing,System.Windows.Forms -TypeDefinition @"
using System; using System.Runtime.InteropServices;
public static class B { [DllImport("user32.dll")] public static extern bool SetProcessDPIAware(); }
"@
[B]::SetProcessDPIAware() | Out-Null
Add-Type -AssemblyName System.Drawing, System.Windows.Forms
New-Item -ItemType Directory -Force $Out | Out-Null
Get-ChildItem $Out -Filter *.png | Remove-Item
$v = [System.IO.MemoryMappedFiles.MemoryMappedFile]::OpenExisting("Local\MML_DEBUG_PAD").CreateViewAccessor()
$b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
# crop the middle where Mega Man is
$cw = 700; $ch = 700; $cx = [int](($b.Width - $cw) / 2); $cy = [int]($b.Height - $ch - 40)
$frames = New-Object System.Collections.ArrayList
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$v.Write(0, [int]1); $v.Write(4, [uint16]$Btn)
$pressed = $true
for ($i = 0; $i -lt $Count; $i++) {
  if ($pressed -and $sw.ElapsedMilliseconds -gt 120) { $v.Write(4, [uint16]0); $pressed = $false }
  $bmp = New-Object System.Drawing.Bitmap $cw, $ch
  [System.Drawing.Graphics]::FromImage($bmp).CopyFromScreen($cx, $cy, 0, 0, (New-Object System.Drawing.Size $cw, $ch))
  [void]$frames.Add(@($sw.ElapsedMilliseconds, $bmp))
  if ($DelayMs) { Start-Sleep -Milliseconds $DelayMs }
}
$v.Write(4, [uint16]0); $v.Write(0, [int]0)
$k = 0
foreach ($f in $frames) { $f[1].Save((Join-Path $Out ("{0:D3}_{1}ms.png" -f $k, $f[0]))); $k++ }
"$($frames.Count) frames in $($sw.ElapsedMilliseconds) ms -> $Out"
