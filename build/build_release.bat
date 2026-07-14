@echo off
REM One command to produce an updated installer after any code change.
REM Rebuilds the launcher, re-assembles the package, and rebuilds the installer.
REM Set MML_GAME_DIR to your game folder first (it holds the wrapper DLLs and the intro).
cd /d "%~dp0.."

if "%MML_GAME_DIR%"=="" (
  echo Set MML_GAME_DIR to your game folder first, e.g.:
  echo   set "MML_GAME_DIR=C:\Games\Mega Man Legends"
  pause & exit /b 1
)

echo [1/3] Building the launcher...
python -m PyInstaller --noconfirm --onefile --windowed --name "MML Launcher" ^
  --icon "art\mml.ico" --add-data "art;art" --hidden-import frida ^
  --distpath "dist" --workpath "%TEMP%\mml_build" --specpath "%TEMP%\mml_build" ^
  "src\mml_gui.py"
if errorlevel 1 (echo launcher build failed & pause & exit /b 1)

echo [2/3] Assembling the release package...
python "tools\make_patcher.py"
if errorlevel 1 (echo make_patcher failed & pause & exit /b 1)

echo [3/3] Building the installer...
python -m PyInstaller --noconfirm --onefile --windowed --name "MML-Setup" ^
  --icon "art\mml.ico" --add-data "dist\release\game-files;game-files" ^
  --add-data "dist\release\CAPCOM.AVI;." --add-data "art\mml.ico;." --paths "src" ^
  --distpath "dist" --workpath "%TEMP%\mml_isetup" --specpath "%TEMP%\mml_isetup" ^
  "src\mml_installer.py"
if errorlevel 1 (echo installer build failed & pause & exit /b 1)

echo.
echo Done. Updated installer: dist\MML-Setup.exe
pause
