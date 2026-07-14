@echo off
REM Build "MML-Setup.exe", the graphical installer end users run.
REM Prerequisites:
REM   - run build_gui.bat first (it produces the launcher this bundles)
REM   - set MML_GAME_DIR to your game folder (make_patcher needs the wrapper DLLs and intro)
cd /d "%~dp0.."

if not exist "dist\MML Launcher.exe" (
  echo Run build_gui.bat first. & pause & exit /b 1
)

echo Assembling the release package...
python "tools\make_patcher.py"
if errorlevel 1 (echo make_patcher failed - is MML_GAME_DIR set? & pause & exit /b 1)

echo Building the installer...
python -m PyInstaller --noconfirm --onefile --windowed --name "MML-Setup" ^
  --icon "art\mml.ico" ^
  --add-data "dist\release\game-files;game-files" ^
  --add-data "dist\release\CAPCOM.AVI;." ^
  --add-data "art\mml.ico;." ^
  --paths "src" ^
  --distpath "dist" --workpath "%TEMP%\mml_isetup" --specpath "%TEMP%\mml_isetup" ^
  "src\mml_installer.py"
if errorlevel 1 (echo BUILD FAILED & pause & exit /b 1)

echo.
echo Done: dist\MML-Setup.exe
pause
