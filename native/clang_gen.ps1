# Compiles the generated recompiled code (build\gen\recomp_*.c) with clang-cl, several files at a
# time; called by build.bat when RECOMP_CLANG names a clang-cl.exe (the Android NDK has one).
# Run from a Visual Studio x86 environment (the MSVC headers). Exit code 1 if any file fails.
param([string]$Clang, [string]$Src, [string]$Gen, [string]$Defs = "", [int]$Jobs = 6)
$files = Get-ChildItem "$Gen\recomp_*.c"
$running = @()
$failed = 0
foreach ($f in $files) {
    while ($running.Count -ge $Jobs) {
        $done = $running | Where-Object { $_.HasExited }
        foreach ($p in $done) { if ($p.ExitCode -ne 0) { $failed++ } }
        $running = @($running | Where-Object { -not $_.HasExited })
        if ($running.Count -ge $Jobs) { Start-Sleep -Milliseconds 100 }
    }
    $args = "--target=i686-pc-windows-msvc /c /O2 /MT /W0 /GS- $Defs /I`"$Src`" /Fo`"$Gen\\`" `"$($f.FullName)`""
    $p = Start-Process -FilePath $Clang -ArgumentList $args -NoNewWindow -PassThru
    $null = $p.Handle   # (without this PowerShell 5 loses the exit code)
    $running += $p
}
foreach ($p in $running) { $p.WaitForExit(); if ($p.ExitCode -ne 0) { $failed++ } }
if ($failed) { Write-Output "clang-cl: $failed file(s) failed"; exit 1 }
exit 0
