@echo off
rem Encode the Modernization intro for mml.dll's own player: MPEG-1 video + MP2 audio
rem in an MPEG program stream (what pl_mpeg decodes). Needs ffmpeg on PATH.
rem   make_intro.bat "master.mov" "C:\Games\Mega Man Legends\Modernization\intro.mpg"
rem Keeps the master's frame rate when MPEG-1 allows it (23.976, 24, 25, 29.97, 30, 50, 59.94, 60).
setlocal
if "%~2"=="" (echo usage: make_intro.bat master.mov out\intro.mpg & exit /b 1)
ffmpeg -y -i "%~1" -vf "scale=1920:-2:flags=lanczos,format=yuv420p" -c:v mpeg1video -b:v 24M -maxrate 24M -bufsize 8M -g 15 -bf 2 -c:a mp2 -b:a 256k -ar 48000 -ac 2 -f mpeg "%~2"
if errorlevel 1 (echo ENCODE FAILED & exit /b 1)
echo Done: %~2
