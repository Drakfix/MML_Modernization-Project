param([string]$Action, [string]$Arg = "", [int]$Wait = 0)
# Drive the game for automated visual testing.
#   start                      launch $env:MML_EXE (default MEGAMAN_NATIVE.EXE) in $env:MML_GAME_DIR
#   pad start,a,down*2         press pad buttons (needs mml.ini [debug] remote_pad=1)
#   stick "rx=32767" -Wait 2   hold stick values (lx/ly/rx/ry) for N seconds
#   shot NAME                  DPI-aware screenshot (960x540, full size with MML_SHOT_FULL=1) into $env:MML_SHOTS
#   stop                       kill the game
# -Wait N sleeps N seconds after the action.
# The game only reads the pad while its window is in the foreground, and it
# ignores synthesized keystrokes, so everything goes through the debug pad.
$ErrorActionPreference = "Stop"
$game = $env:MML_GAME_DIR
if (-not $game) { throw "Set MML_GAME_DIR to the game folder" }
$exe = if ($env:MML_EXE) { $env:MML_EXE } else { "MEGAMAN_NATIVE.EXE" }
$proc = [System.IO.Path]::GetFileNameWithoutExtension($exe)
$out = if ($env:MML_SHOTS) { $env:MML_SHOTS } else { Join-Path $env:TEMP "mml-shots" }
New-Item -ItemType Directory -Force $out | Out-Null
Add-Type -ReferencedAssemblies System.Drawing,System.Windows.Forms -TypeDefinition @"
using System; using System.Runtime.InteropServices;
public static class Drv {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
}
"@
[Drv]::SetProcessDPIAware() | Out-Null
$btn = @{ up=0x1; down=0x2; left=0x4; right=0x8; start=0x10; back=0x20; l3=0x40; r3=0x80; lb=0x100; rb=0x200; a=0x1000; b=0x2000; x=0x4000; y=0x8000 }
# layout of Local\MML_DEBUG_PAD: LONG active; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry
function PadView { return [System.IO.MemoryMappedFiles.MemoryMappedFile]::OpenExisting("Local\MML_DEBUG_PAD").CreateViewAccessor() }
function Focus { $p = Get-Process $proc -ErrorAction SilentlyContinue | Select-Object -First 1; if ($p -and $p.MainWindowHandle -ne 0) { [Drv]::SetForegroundWindow($p.MainWindowHandle) | Out-Null } }
switch ($Action) {
  "start" { Start-Process -FilePath (Join-Path $game $exe) -WorkingDirectory $game | Out-Null; "started" }
  "pad" {
    Focus; $v = PadView
    foreach ($k in $Arg.Split(',')) {
      $parts = $k.Split('*'); $n = 1; if ($parts.Length -gt 1) { $n = [int]$parts[1] }
      for ($i = 0; $i -lt $n; $i++) {
        $v.Write(4, [uint16]$btn[$parts[0]]); $v.Write(0, [int]1); Start-Sleep -Milliseconds 150
        $v.Write(4, [uint16]0); Start-Sleep -Milliseconds 450
      }
    }
    $v.Write(0, [int]0); "pad $Arg"
  }
  "stick" {
    Focus; $v = PadView
    foreach ($kv in $Arg.Split(',')) { $p = $kv.Split('='); $off = @{lx=8; ly=10; rx=12; ry=14}[$p[0]]; $v.Write($off, [int16]$p[1]) }
    $v.Write(0, [int]1); Start-Sleep -Seconds $Wait
    foreach ($o in 8,10,12,14) { $v.Write($o, [int16]0) }
    $v.Write(0, [int]0); $Wait = 0; "stick $Arg"
  }
  "shot" {
    $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
    [System.Drawing.Graphics]::FromImage($bmp).CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
    # MML_SHOT_FULL=1 keeps the full screen resolution (for small details)
    $sm = if ($env:MML_SHOT_FULL -eq "1") { $bmp } else { New-Object System.Drawing.Bitmap $bmp, 960, 540 }
    $path = Join-Path $out "$Arg.png"; $sm.Save($path); $path
  }
  "stop" { Get-Process $proc -ErrorAction SilentlyContinue | Stop-Process -Force; "stopped" }
}
if ($Wait -gt 0) { Start-Sleep -Seconds $Wait }
