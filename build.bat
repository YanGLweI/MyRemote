@echo off
REM Build script for MyRemote Control (Windows, MSVC).
REM Prereqs: VS2022 (Build Tools ok), Qt6 (MSVC), vcpkg with:
REM   vcpkg install openssl:x64-windows-static openh264:x64-windows-static

cd /d %~dp0

set "VCPKG=C:\vcpkg"
set "QTDIR=C:\Qt\6.7.3\msvc2019_64"

if not exist "build" mkdir build
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
  -DCMAKE_PREFIX_PATH="%QTDIR%" ^
  -DCMAKE_TOOLCHAIN_FILE="%VCPKG%\scripts\buildsystems\vcpkg.cmake" ^
  -DVCPKG_TARGET_TRIPLET=x64-windows-static || goto :error

cmake --build build --config Release || goto :error

echo.
echo Deploying Qt runtime for control_server...
"%QTDIR%\bin\windeployqt.exe" --release build\bin\Release\control_server.exe

echo.
echo Done. Outputs:
echo   build\bin\Release\agent.exe           (single-file agent)
echo   build\bin\Release\control_server.exe  (needs deployed Qt DLLs)
goto :end

:error
echo Build failed!
exit /b 1

:end
