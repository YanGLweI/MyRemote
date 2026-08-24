@echo off
REM Build script for MyRemote Control
REM Requires Visual Studio Developer Command Prompt

cd /d %~dp0

if not exist "build" mkdir build
cd build

echo Building MyRemote Control (Debug)...
cmake .. -DCMAKE_BUILD_TYPE=Debug -G "Visual Studio 17 2022" -A x64 || goto :error
cmake --build . --config Debug || goto :error

echo.
echo Building MyRemote Control (Release)...
cmake --build . --config Release || goto :error

cd ..
goto :end

:error
echo Build failed!
exit /b 1

:end
echo Build completed successfully!
pause
