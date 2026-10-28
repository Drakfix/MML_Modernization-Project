@echo off
rem Builds native\build\mml.dll (32-bit, static CRT) with the MSVC Build Tools.
rem Shaders (shaders\game.hlsl) are compiled with fxc into native\build\shaders\*.h first.
setlocal
set ROOT=%~dp0
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
echo using %VSDIR%
if not defined VSDIR goto novs
call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul || exit /b 1
set "OUT=%ROOT%build"
set "SH=%ROOT%build\shaders"
set "HLSL=%ROOT%shaders\game.hlsl"
if not exist "%SH%" mkdir "%SH%"
fxc /nologo /T vs_4_0 /E vs_main /Vn g_vs_main /Fh "%SH%\game_vs.h" "%HLSL%" >nul || exit /b 1
fxc /nologo /T ps_4_0 /E ps_main /Vn g_ps_main /Fh "%SH%\game_ps.h" "%HLSL%" >nul || exit /b 1
fxc /nologo /T vs_4_0 /E vs_blit /Vn g_vs_blit /Fh "%SH%\blit_vs.h" "%HLSL%" >nul || exit /b 1
fxc /nologo /T ps_4_0 /E ps_blit /Vn g_ps_blit /Fh "%SH%\blit_ps.h" "%HLSL%" >nul || exit /b 1
fxc /nologo /T ps_4_0 /E ps_depth_copy /Vn g_ps_depth_copy /Fh "%SH%\depth_copy_ps.h" "%HLSL%" >nul || exit /b 1
fxc /nologo /T ps_4_0 /E ps_show_depth /Vn g_ps_show_depth /Fh "%SH%\show_depth_ps.h" "%HLSL%" >nul || exit /b 1
fxc /nologo /T vs_4_0 /E vs_ui /Vn g_vs_ui /Fh "%SH%\ui_vs.h" "%HLSL%" >nul || exit /b 1
fxc /nologo /T ps_4_0 /E ps_ui /Vn g_ps_ui /Fh "%SH%\ui_ps.h" "%HLSL%" >nul || exit /b 1
rem recompiled game code (tools/recomp/recomp.py output, never committed): compiled on its own with
rem warnings off; without it the harness builds empty
set "GENOBJ="
set "GENDEF=/DRECOMP_NONE"
if exist "%OUT%\gen\recomp_*.c" (
  del /q "%OUT%\gen\*.obj" 2>nul
  cl /nologo /c /MP4 /O2 /MT /W0 /GS- /I"%ROOT%src" /Fo"%OUT%\gen\\" "%OUT%\gen\recomp_*.c" >nul || exit /b 1
  set "GENOBJ=%OUT%\gen\*.obj"
  set "GENDEF="
)
rem C4201: the SDK's own D3D11 headers use nameless unions
cl /nologo /O2 /MT /LD /W4 /WX /wd4201 /GS- %GENDEF% /I"%OUT%" /Fo"%OUT%\\" "%ROOT%src\*.c" %GENOBJ% ^
   /Fe"%OUT%\mml.dll" /link /DEF:"%ROOT%mml.def" /INCREMENTAL:NO /MAP:"%OUT%\mml.map" user32.lib kernel32.lib gdi32.lib d3d11.lib dxgi.lib ole32.lib windowscodecs.lib || exit /b 1
echo built %OUT%\mml.dll
exit /b 0

:novs
echo Visual Studio Build Tools not found (need the C++ workload with x86 tools).
exit /b 1
