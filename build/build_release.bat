@echo off
REM One command to produce an updated installer after any code change.
REM Rebuilds mml.dll and the launcher, re-assembles the package, and rebuilds the installer.
REM Set MML_GAME_DIR to your game folder first (it holds the wrapper DLLs and the intro).
setlocal
set "ROOT=%~dp0.."
pushd "%ROOT%"
for %%I in ("%ROOT%") do set "ROOT=%%~fI"

if "%MML_GAME_DIR%"=="" (
  echo Set MML_GAME_DIR to your game folder first, e.g.:
  echo   set "MML_GAME_DIR=C:\Games\Mega Man Legends"
  popd & pause & exit /b 1
)

echo [0/3] Building mml.dll...
call "%ROOT%\native\build.bat"
if errorlevel 1 (echo mml.dll build failed & popd & pause & exit /b 1)

REM the launcher imports patch_manifest.py, so write it before building the launcher
python "%ROOT%\tools\make_patcher.py" --manifest-only
if errorlevel 1 (echo make_patcher failed & popd & pause & exit /b 1)

echo [1/3] Building the launcher...
python -m PyInstaller --noconfirm --onefile --windowed --name "MML Launcher" ^
  --icon "%ROOT%\art\mml.ico" --add-data "%ROOT%\art;art" ^
  --distpath "%ROOT%\dist" --workpath "%TEMP%\mml_build" --specpath "%TEMP%\mml_build" ^
  "%ROOT%\src\mml_gui.py"
if errorlevel 1 (echo launcher build failed & popd & pause & exit /b 1)

echo [2/3] Assembling the release package...
python "%ROOT%\tools\make_patcher.py"
if errorlevel 1 (echo make_patcher failed & popd & pause & exit /b 1)

echo [3/3] Building the installer...
python -m PyInstaller --noconfirm --onefile --windowed --name "MML-Setup" ^
  --icon "%ROOT%\art\mml.ico" --add-data "%ROOT%\dist\release\game-files;game-files" ^
  --add-data "%ROOT%\dist\release\CAPCOM.AVI;." --add-data "%ROOT%\art\mml.ico;." --paths "%ROOT%\src" ^
  --distpath "%ROOT%\dist" --workpath "%TEMP%\mml_isetup" --specpath "%TEMP%\mml_isetup" ^
  "%ROOT%\src\mml_installer.py"
if errorlevel 1 (echo installer build failed & popd & pause & exit /b 1)

echo.
echo Done. Updated installer: %ROOT%\dist\MML-Setup.exe
popd & pause
