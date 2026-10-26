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
rem C4201: the SDK's own D3D11 headers use nameless unions
cl /nologo /O2 /MT /LD /W4 /WX /wd4201 /GS- /I"%OUT%" /Fo"%OUT%\\" "%ROOT%src\*.c" ^
   /Fe"%OUT%\mml.dll" /link /DEF:"%ROOT%mml.def" /INCREMENTAL:NO user32.lib kernel32.lib d3d11.lib dxgi.lib || exit /b 1
echo built %OUT%\mml.dll
exit /b 0

:novs
echo Visual Studio Build Tools not found (need the C++ workload with x86 tools).
exit /b 1
