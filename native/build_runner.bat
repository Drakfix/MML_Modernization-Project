@echo off
rem Builds native\build\mml_run.exe: the standalone runner (src\runner.c, M3 item 4). The same
rem sources as mml.dll with MML_RUNNER defined, and the recompiled code (build\gen, needed) built
rem for the runner's guest memory block. Run build.bat once first (the shader headers).
rem RECOMP_CLANG=path\to\clang-cl.exe compiles the generated code with clang, as in build.bat.
rem build_runner.bat x64: the 64-bit runner, build\mml_run64.exe, all compiled with clang-cl
rem (RECOMP_CLANG needed). It loads SDL3.dll and librashader.dll from x64\ in the game folder.
setlocal
set ROOT=%~dp0
set "ARCH=x86"
if /i "%~1"=="x64" set "ARCH=x64"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto novs
call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" %ARCH% >nul || exit /b 1
set "OUT=%ROOT%build"
if not exist "%OUT%\shaders\game_vs.h" (
  echo run build.bat first: the shader headers are missing
  exit /b 1
)
if not exist "%OUT%\gen\recomp_table.c" (
  echo no generated code in build\gen: the runner has nothing to run
  exit /b 1
)
call "%ROOT%vk_shaders.bat" || exit /b 1
rem the Japanese program's recompiled code (Rockman DASH, build\gen_jp, tools\recomp on its own
rem Ghidra project) goes in as well when it is there: the runner picks the one for the exe it loads
set "GENJP=%OUT%\gen_jp"
set "JPDEF="
if exist "%GENJP%\recomp_table.c" set "JPDEF=/DRECOMP_HAVE_JP"
if "%ARCH%"=="x64" goto x64

set "RUN=%OUT%\runner"
set "GENRUN=%OUT%\gen_runner"
if not exist "%RUN%" mkdir "%RUN%"
if not exist "%GENRUN%" mkdir "%GENRUN%"
rem the generated code only changes with a regeneration: compiled again when its objects are gone
rem (delete build\gen_runner after regenerating) or with RUNNER_REGEN=1
if "%RUNNER_REGEN%"=="1" del /q "%GENRUN%\*.obj" 2>nul
set "GENRUNJP=%OUT%\gen_runner_jp"
set "JPOBJ="
if defined JPDEF (
  if not exist "%GENRUNJP%" mkdir "%GENRUNJP%"
  if "%RUNNER_REGEN%"=="1" del /q "%GENRUNJP%\*.obj" 2>nul
  set "JPOBJ="%GENRUNJP%\*.obj""
)
if exist "%GENRUN%\recomp_table.obj" goto genjp
if defined RECOMP_CLANG (
  powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%clang_gen.ps1" -Clang "%RECOMP_CLANG%" -Src "%ROOT%src" -Gen "%OUT%\gen" -Out "%GENRUN%" -Defs "/DMML_RUNNER" || exit /b 1
) else (
  cl /nologo /c /MP4 /O2 /MT /W0 /GS- /DMML_RUNNER /I"%ROOT%src" /Fo"%GENRUN%\\" "%OUT%\gen\recomp_*.c" >nul || exit /b 1
)
:genjp
if not defined JPDEF goto link
if exist "%GENRUNJP%\recomp_table.obj" goto link
if defined RECOMP_CLANG (
  powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%clang_gen.ps1" -Clang "%RECOMP_CLANG%" -Src "%ROOT%src" -Gen "%GENJP%" -Out "%GENRUNJP%" -Defs "/DMML_RUNNER" || exit /b 1
) else (
  cl /nologo /c /MP4 /O2 /MT /W0 /GS- /DMML_RUNNER /I"%ROOT%src" /Fo"%GENRUNJP%\\" "%GENJP%\recomp_*.c" >nul || exit /b 1
)
:link
rem the icon (runner.rc)
rc /nologo /fo "%OUT%\runner.res" "%ROOT%runner.rc" || exit /b 1
rem C4201: the SDK's own D3D11 headers use nameless unions
rem /BASE /FIXED: our code above the game's whole address range (guest_mem.c: 512 MB), so a host
rem function address (in an import slot, a patched JMP) is never also a game address
cl /nologo /O2 /MT /W4 /WX /wd4201 /GS- /DMML_RUNNER %VKDEF% %JPDEF% /I"%OUT%" /Fo"%RUN%\\" "%ROOT%src\*.c" "%GENRUN%\*.obj" %JPOBJ% ^
   "%OUT%\runner.res" /Fe"%OUT%\mml_run.exe" /link /SUBSYSTEM:WINDOWS /LARGEADDRESSAWARE /INCREMENTAL:NO /MAP:"%OUT%\mml_run.map" ^
   /BASE:0x60000000 /FIXED /DYNAMICBASE:NO ^
   user32.lib kernel32.lib gdi32.lib d3d11.lib dxgi.lib ole32.lib windowscodecs.lib winmm.lib || exit /b 1
echo built %OUT%\mml_run.exe
exit /b 0

:x64
if not defined RECOMP_CLANG (
  echo the 64-bit runner needs RECOMP_CLANG=path\to\clang-cl.exe
  exit /b 1
)
set "RUN=%OUT%\runner64"
set "GENRUN=%OUT%\gen_runner64"
if not exist "%RUN%" mkdir "%RUN%"
if not exist "%GENRUN%" mkdir "%GENRUN%"
if "%RUNNER_REGEN%"=="1" del /q "%GENRUN%\*.obj" 2>nul
if not exist "%GENRUN%\recomp_table.obj" (
  powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%clang_gen.ps1" -Clang "%RECOMP_CLANG%" -Src "%ROOT%src" -Gen "%OUT%\gen" -Out "%GENRUN%" -Defs "/DMML_RUNNER" -Target "x86_64-pc-windows-msvc" || exit /b 1
)
set "GENRUNJP=%OUT%\gen_runner64_jp"
set "JPOBJ="
if not defined JPDEF goto x64src
if not exist "%GENRUNJP%" mkdir "%GENRUNJP%"
if "%RUNNER_REGEN%"=="1" del /q "%GENRUNJP%\*.obj" 2>nul
set "JPOBJ="%GENRUNJP%\*.obj""
if not exist "%GENRUNJP%\recomp_table.obj" (
  powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%clang_gen.ps1" -Clang "%RECOMP_CLANG%" -Src "%ROOT%src" -Gen "%GENJP%" -Out "%GENRUNJP%" -Defs "/DMML_RUNNER" -Target "x86_64-pc-windows-msvc" || exit /b 1
)
:x64src
rem one file at a time: clang-cl stops at the first file that fails
del /q "%RUN%\*.obj" 2>nul
for %%f in ("%ROOT%src\*.c") do (
  "%RECOMP_CLANG%" --target=x86_64-pc-windows-msvc /c /O2 /MT /W3 /GS- /DMML_RUNNER %VKDEF% %JPDEF% /I"%OUT%" /I"%ROOT%src" /Fo"%RUN%\\" "%%f" || exit /b 1
)
rc /nologo /fo "%OUT%\runner64.res" "%ROOT%runner.rc" || exit /b 1
link /nologo /OUT:"%OUT%\mml_run64.exe" /SUBSYSTEM:WINDOWS /INCREMENTAL:NO /MAP:"%OUT%\mml_run64.map" ^
   "%RUN%\*.obj" "%GENRUN%\*.obj" %JPOBJ% "%OUT%\runner64.res" ^
   user32.lib kernel32.lib gdi32.lib d3d11.lib dxgi.lib ole32.lib windowscodecs.lib winmm.lib || exit /b 1
echo built %OUT%\mml_run64.exe
exit /b 0

:novs
echo Visual Studio Build Tools not found (need the C++ workload with x86 and x64 tools).
exit /b 1
