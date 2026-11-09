@echo off
setlocal
where cmake >nul 2>nul
if errorlevel 1 (
  echo CMake est requis. Installez Visual Studio 2022 avec "Desktop development with C++" et CMake.
  pause
  exit /b 1
)
cmake -S "%~dp0" -B "%~dp0build" -G "Visual Studio 17 2022" -A Win32
if errorlevel 1 goto fail
cmake --build "%~dp0build" --config Release
if errorlevel 1 goto fail
echo.
echo DLL creee : "%~dp0build\Release\gen_wavecue.dll"
pause
exit /b 0
:fail
echo La compilation a echoue. Consultez les messages precedents.
pause
exit /b 1
