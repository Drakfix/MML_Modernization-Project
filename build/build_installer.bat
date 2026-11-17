@echo off
REM Build "MML-Setup.exe", the graphical installer end users run.
REM Prerequisites:
REM   - native\build.bat and native\build_runner.bat x64 (mml.dll, and mml_run64.exe: the start
REM     menu and the standalone game); build_release.bat does all of it in one go
REM   - set MML_GAME_DIR to your game folder (make_patcher needs the wrapper DLLs and intro)
setlocal
for %%I in ("%~dp0..") do set "ROOT=%%~fI"

echo Assembling the release package...
python "%ROOT%\tools\make_patcher.py"
if errorlevel 1 (echo make_patcher failed - is MML_GAME_DIR set? & pause & exit /b 1)

echo Building the installer...
python -m PyInstaller --noconfirm --onefile --windowed --name "MML-Setup" ^
  --icon "%ROOT%\art\mml.ico" --add-data "%ROOT%\dist\release\game-files;game-files" ^
  --add-data "%ROOT%\dist\release\CAPCOM.AVI;." --add-data "%ROOT%\art\mml.ico;." --paths "%ROOT%\src" ^
  --paths "%ROOT%\tools\voice" --hidden-import build_jp_voices --hidden-import build_subtitles ^
  --distpath "%ROOT%\dist" --workpath "%TEMP%\mml_isetup" --specpath "%TEMP%\mml_isetup" ^
  "%ROOT%\src\mml_installer.py"
if errorlevel 1 (echo BUILD FAILED & pause & exit /b 1)

echo.
echo Done: %ROOT%\dist\MML-Setup.exe
pause
