# Synthesized keyboard/mouse for testing the keyboard+mouse controls. The game must be running.
# Reaches GetAsyncKeyState and raw input (what mml.dll reads), NOT the game's DirectInput.
#   kbm.ps1 hold W+LSHIFT -Ms 300 | move "400,0" -Ms 300 | lclick | rhold -Ms 500 | altf4
param([string]$Action, [string]$Arg = "", [int]$Ms = 300)
Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices;
public static class K {
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern void mouse_event(uint flags, int dx, int dy, uint data, UIntPtr extra);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
}
"@
$p = Get-Process MEGAMAN_NATIVE -ErrorAction SilentlyContinue | Select-Object -First 1
if ($p) { [K]::SetForegroundWindow($p.MainWindowHandle) | Out-Null; Start-Sleep -Milliseconds 100 }
$vk = @{ W=0x57; A=0x41; S=0x53; D=0x44; E=0x45; Q=0x51; M=0x4D; SPACE=0x20; LSHIFT=0xA0; LCTRL=0xA2; TAB=0x09; F5=0x74; F6=0x75; J=0x4A; DOWN=0x28; UP=0x26; ENTER=0x0D }
switch ($Action) {
  "hold" { $keys = $Arg.Split('+') | % { [byte]$vk[$_] }; foreach ($k in $keys) { [K]::keybd_event($k, 0, 0, [UIntPtr]::Zero) }; Start-Sleep -Milliseconds $Ms; foreach ($k in $keys) { [K]::keybd_event($k, 0, 2, [UIntPtr]::Zero) } }
  "move" { $xy = $Arg.Split(','); $steps = 20; for ($i = 0; $i -lt $steps; $i++) { [K]::mouse_event(1, [int]($xy[0] / $steps), [int]($xy[1] / $steps), 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds ([int]($Ms / $steps)) } }
  "lclick" { [K]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds $Ms; [K]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero) }
  "altf4" { [K]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero); [K]::keybd_event(0x73, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 80; [K]::keybd_event(0x73, 0, 2, [UIntPtr]::Zero); [K]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero) }
  "rhold" { [K]::mouse_event(8, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds $Ms; [K]::mouse_event(16, 0, 0, 0, [UIntPtr]::Zero) }
}
"$Action $Arg"
