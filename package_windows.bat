@echo off
setlocal

rem Extract version from CMakeLists.txt
for /f "delims=" %%v in ('powershell -NoProfile -Command ^
  "([regex]::Match((Get-Content CMakeLists.txt -Raw), 'project\s*\([^)]*VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)')).Groups[1].Value"') do set VERSION=%%v

if "%VERSION%"=="" (
    echo ERROR: could not extract version from CMakeLists.txt
    exit /b 1
)

echo Packaging sharedgl v%VERSION%

rem Clean previous build
if exist build rmdir /s /q build

mkdir build
cd build
cmake -DCMAKE_GENERATOR_PLATFORM=x64 ..
if errorlevel 1 exit /b %errorlevel%
cmake --build . --target sharedgl-core --config Release
if errorlevel 1 exit /b %errorlevel%
cmake -DCMAKE_GENERATOR_PLATFORM=Win32 ..
if errorlevel 1 exit /b %errorlevel%
cmake --build . --target sharedgl-core --config Release
if errorlevel 1 exit /b %errorlevel%
cd ..

rem Stage install/uninstall scripts alongside the DLLs
copy /y .\scripts\wininstall.bat   .\build\Release\wininstall.bat
copy /y .\scripts\winuninstall.bat .\build\Release\winuninstall.bat

rem Zip the four artifacts
cd build\Release
powershell -NoProfile -Command ^
  "Compress-Archive -Force -Path wininstall.bat,winuninstall.bat,sharedgl64.dll,sharedgl32.dll -DestinationPath sharedgl-v%VERSION%-windows.zip"
if errorlevel 1 (
    cd ..\..
    exit /b 1
)
cd ..\..

echo Done: build\Release\sharedgl-v%VERSION%-windows.zip
