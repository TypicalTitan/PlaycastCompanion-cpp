@echo off
rem Builds the Playcast Companion (native) NSIS setup:
rem   1. Release build via ..\build.cmd  -> build\PlaycastCompanion.exe
rem   2. recreates installer\payload with the exe and config.json
rem   3. compiles installer.nsi into PlaycastCompanionSetup-<version>.exe
setlocal
cd /d "%~dp0.."

echo [1/3] Building Release...
call "%~dp0..\build.cmd"
if errorlevel 1 goto :fail
if not exist "build\PlaycastCompanion.exe" goto :missing

echo [2/3] Staging payload...
if exist "installer\payload" rmdir /s /q "installer\payload"
mkdir "installer\payload"
copy /y "build\PlaycastCompanion.exe" "installer\payload\PlaycastCompanion.exe" >nul
if errorlevel 1 goto :fail
copy /y "config.json" "installer\payload\config.json" >nul
if errorlevel 1 goto :fail
if not exist "installer\payload\PlaycastCompanion.exe" goto :missing
if not exist "installer\payload\config.json" goto :missing

echo [3/3] Compiling NSIS installer...
set "MAKENSIS=%ProgramFiles(x86)%\NSIS\makensis.exe"
if not exist "%MAKENSIS%" set "MAKENSIS=%ProgramFiles%\NSIS\makensis.exe"
if exist "%MAKENSIS%" goto :compile
where makensis >nul 2>nul
if errorlevel 1 goto :nonsis
set "MAKENSIS=makensis"

:compile
"%MAKENSIS%" "installer\installer.nsi"
if errorlevel 1 goto :fail

echo.
echo Done: installer\PlaycastCompanionSetup-3.0.0.exe
pause
exit /b 0

:missing
echo.
echo BUILD FAILED: build output is incomplete - build\PlaycastCompanion.exe or
echo config.json is missing (payload in installer\payload could not be staged)
pause
exit /b 1

:nonsis
echo NSIS not found. Install it first:  winget install NSIS.NSIS
pause
exit /b 1

:fail
echo.
echo BUILD FAILED
pause
exit /b 1
