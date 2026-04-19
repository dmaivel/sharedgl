@echo off
setlocal

if exist build rmdir /s /q build

mkdir build
cd build
cmake -DCMAKE_GENERATOR_PLATFORM=x64 ..
cmake --build . --target sharedgl-core --config Release
cmake -DCMAKE_GENERATOR_PLATFORM=Win32 ..
cmake --build . --target sharedgl-core --config Release
cd ..

copy /y .\scripts\wininstall.bat .\build\Release\wininstall.bat
