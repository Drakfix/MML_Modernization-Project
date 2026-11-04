@echo off
rem Called by build.bat and build_runner.bat. The Vulkan backend's shaders (shaders\vk\*.vert and
rem *.frag, GLSL) go into build\shaders\vk_*.inc with glslc, and VKDEF gets the compiler options
rem for gfx_vk.c, when glslc and the Vulkan headers are there; otherwise VKDEF stays empty and
rem the build has no Vulkan backend. GLSLC= and VULKAN_INCLUDE= name them; the defaults are the
rem Android NDK's glslc and SDL3's copy of the Khronos headers in mml-deps next to the repo.
set "VKDEF="
if not defined GLSLC set "GLSLC=%~dp0..\..\mml-deps\android-ndk\android-ndk-r27c\shader-tools\windows-x86_64\glslc.exe"
if not defined VULKAN_INCLUDE set "VULKAN_INCLUDE=%~dp0..\..\mml-deps\SDL3-3.4.18\src\video\khronos"
if not exist "%GLSLC%" goto none
if not exist "%VULKAN_INCLUDE%\vulkan\vulkan.h" goto none
if not exist "%~dp0build\shaders" mkdir "%~dp0build\shaders"
for %%s in (game blit ui) do (
  "%GLSLC%" -O --target-env=vulkan1.1 -mfmt=c "%~dp0shaders\vk\%%s.vert" -o "%~dp0build\shaders\vk_%%s_vert.inc" || exit /b 1
  "%GLSLC%" -O --target-env=vulkan1.1 -mfmt=c "%~dp0shaders\vk\%%s.frag" -o "%~dp0build\shaders\vk_%%s_frag.inc" || exit /b 1
)
set VKDEF=/DMML_VULKAN /I"%VULKAN_INCLUDE%"
exit /b 0
:none
echo no glslc or Vulkan headers: built without the Vulkan backend
exit /b 0
