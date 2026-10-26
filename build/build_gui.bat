@echo off
REM Build "MML Launcher.exe" from src\mml_gui.py.
REM Requires: pip install pyinstaller pillow
setlocal
for %%I in ("%~dp0..") do set "ROOT=%%~fI"
python -m PyInstaller --noconfirm --onefile --windowed --name "MML Launcher" ^
  --icon "%ROOT%\art\mml.ico" --add-data "%ROOT%\art;art" ^
  --distpath "%ROOT%\dist" --workpath "%TEMP%\mml_build" --specpath "%TEMP%\mml_build" ^
  "%ROOT%\src\mml_gui.py"
if errorlevel 1 (echo BUILD FAILED & pause & exit /b 1)
echo.
echo Done: %ROOT%\dist\MML Launcher.exe
pause
