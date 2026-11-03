@echo off
rem Builds native\build\mml_run.exe: the standalone runner (src\runner.c, M3 item 4). The same
rem sources as mml.dll with MML_RUNNER defined, and the recompiled code (build\gen, needed) built
rem for the runner's guest memory block. Run build.bat once first (the shader headers).
rem RECOMP_CLANG=path\to\clang-cl.exe compiles the generated code with clang, as in build.bat.
setlocal
set ROOT=%~dp0
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto novs
call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul || exit /b 1
set "OUT=%ROOT%build"
set "RUN=%OUT%\runner"
set "GENRUN=%OUT%\gen_runner"
if not exist "%OUT%\shaders\game_vs.h" (
  echo run build.bat first: the shader headers are missing
  exit /b 1
)
if not exist "%OUT%\gen\recomp_table.c" (
  echo no generated code in build\gen: the runner has nothing to run
  exit /b 1
)
if not exist "%RUN%" mkdir "%RUN%"
if not exist "%GENRUN%" mkdir "%GENRUN%"
rem the generated code only changes with a regeneration: compiled again when its objects are gone
rem (delete build\gen_runner after regenerating) or with RUNNER_REGEN=1
if "%RUNNER_REGEN%"=="1" del /q "%GENRUN%\*.obj" 2>nul
if exist "%GENRUN%\recomp_table.obj" goto link
if defined RECOMP_CLANG (
  powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%clang_gen.ps1" -Clang "%RECOMP_CLANG%" -Src "%ROOT%src" -Gen "%OUT%\gen" -Out "%GENRUN%" -Defs "/DMML_RUNNER" || exit /b 1
) else (
  cl /nologo /c /MP4 /O2 /MT /W0 /GS- /DMML_RUNNER /I"%ROOT%src" /Fo"%GENRUN%\\" "%OUT%\gen\recomp_*.c" >nul || exit /b 1
)
:link
rem C4201: the SDK's own D3D11 headers use nameless unions
rem /BASE /FIXED: our code above the game's whole address range (guest_mem.c: 512 MB), so a host
rem function address (in an import slot, a patched JMP) is never also a game address
cl /nologo /O2 /MT /W4 /WX /wd4201 /GS- /DMML_RUNNER /I"%OUT%" /Fo"%RUN%\\" "%ROOT%src\*.c" "%GENRUN%\*.obj" ^
   /Fe"%OUT%\mml_run.exe" /link /SUBSYSTEM:WINDOWS /LARGEADDRESSAWARE /INCREMENTAL:NO /MAP:"%OUT%\mml_run.map" ^
   /BASE:0x60000000 /FIXED /DYNAMICBASE:NO ^
   user32.lib kernel32.lib gdi32.lib d3d11.lib dxgi.lib ole32.lib windowscodecs.lib winmm.lib || exit /b 1
echo built %OUT%\mml_run.exe
exit /b 0

:novs
echo Visual Studio Build Tools not found (need the C++ workload with x86 tools).
exit /b 1
