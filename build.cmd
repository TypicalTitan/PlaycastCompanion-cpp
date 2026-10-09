@echo off
rem Build Playcast Companion (native). Usage:
rem   build.cmd            Release build -> build\PlaycastCompanion.exe
rem   build.cmd debug      Debug build
rem   build.cmd check FILE Compile a single src\FILE.cpp to check it (no link)
setlocal
set "VSBT=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools"
if not exist "%VSBT%\VC\Auxiliary\Build\vcvars64.bat" set "VSBT=%ProgramFiles%\Microsoft Visual Studio\2022\Community"
if not exist "%VSBT%\VC\Auxiliary\Build\vcvars64.bat" set "VSBT=%ProgramFiles%\Microsoft Visual Studio\2022\Professional"
if not exist "%VSBT%\VC\Auxiliary\Build\vcvars64.bat" set "VSBT=%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise"
if not exist "%VSBT%\VC\Auxiliary\Build\vcvars64.bat" (
  echo Visual Studio 2022 with the "Desktop development with C++" workload is required.
  exit /b 1
)
call "%VSBT%\VC\Auxiliary\Build\vcvars64.bat" >nul
set "CMAKE_BIN=%VSBT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
set "NINJA_BIN=%VSBT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
set "PATH=%CMAKE_BIN%;%NINJA_BIN%;%PATH%"
cd /d "%~dp0"

if /i "%~1"=="check" goto :check

set "CONFIG=Release"
if /i "%~1"=="debug" set "CONFIG=Debug"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG% || exit /b 1
cmake --build build --config %CONFIG% || exit /b 1
echo.
echo Built build\PlaycastCompanion.exe (%CONFIG%)
exit /b 0

:check
if "%~2"=="" ( echo usage: build.cmd check File.cpp & exit /b 1 )
if not exist "%TEMP%\pc-check" mkdir "%TEMP%\pc-check"
cl /nologo /c /std:c++20 /permissive- /W4 /WX /sdl /EHsc /utf-8 /Zc:__cplusplus /bigobj /await:strict ^
   /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_WIN32_WINNT=0x0A00 ^
   /DPC_VERSION_STRING="\"3.2.3\"" /DPC_VERSION_MAJOR=3 /DPC_VERSION_MINOR=2 /DPC_VERSION_PATCH=3 ^
   /Isrc /Fo"%TEMP%\pc-check\\" "src\%~2"
exit /b %errorlevel%
