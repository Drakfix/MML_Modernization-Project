@echo off
rem Builds native\build\mml.dll (32-bit, static CRT) with the MSVC Build Tools.
setlocal
set ROOT=%~dp0
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
echo using %VSDIR%
if not defined VSDIR goto novs
call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul || exit /b 1
if not exist "%ROOT%build" mkdir "%ROOT%build"
cl /nologo /O2 /MT /LD /W4 /WX /GS- /Fo"%ROOT%build\\" "%ROOT%src\*.c" ^
   /Fe"%ROOT%build\mml.dll" /link /DEF:"%ROOT%mml.def" /INCREMENTAL:NO user32.lib kernel32.lib || exit /b 1
echo built %ROOT%build\mml.dll
exit /b 0

:novs
echo Visual Studio Build Tools not found (need the C++ workload with x86 tools).
exit /b 1
