@echo off
rem Builds BorderlessLock.asi (Release) with Visual Studio 2022's bundled CMake + Ninja.
setlocal
set VS=C:\Program Files\Microsoft Visual Studio\2022\Community
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%
cd /d "%~dp0"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build build --target BorderlessLock || exit /b 1
echo Built: %~dp0build\BorderlessLock.asi
