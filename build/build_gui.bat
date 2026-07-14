@echo off
REM Build "MML Launcher.exe" from src\mml_gui.py.
REM Requires: pip install pyinstaller pillow frida
cd /d "%~dp0.."
python -m PyInstaller --noconfirm --onefile --windowed --name "MML Launcher" ^
  --icon "art\mml.ico" ^
  --add-data "art;art" ^
  --hidden-import frida ^
  --distpath "dist" --workpath "%TEMP%\mml_build" --specpath "%TEMP%\mml_build" ^
  "src\mml_gui.py"
if errorlevel 1 (echo BUILD FAILED & pause & exit /b 1)
echo.
echo Done: dist\MML Launcher.exe
pause
