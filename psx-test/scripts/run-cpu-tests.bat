@echo off

if "%VisualStudioVersion%"=="" (
	echo Please run in Visual Studio Native Tools Command Prompt or run vcvarsall.bat to ensure cmake is on the path
	EXIT /B 1
)

REM Set the current directory to the location of the batch script, using the %0 parameter
REM This allows the script to be called from anywhere
pushd "%~dp0"

echo:
echo Building Debug
cmake --build ..\build --config Debug
if errorlevel 1 (
    echo Build failed for Debug configuration
    popd
    EXIT /B 1
)

echo:
echo Building Release
cmake --build ..\build --config Release
if errorlevel 1 (
    echo Build failed for Release configuration
    popd
    EXIT /B 1
)

REM Change to data directory
cd ..\..\data

REM Amidog has drawn final pixel and starts accessing joypad at PC 0x80013a24
setlocal
set SAVE_VRAM_PC=0x80013a24

set TEST_ARGS=--exe test/amidog/psxtest_cpu.exe --amidog-cpu-test --save-vram-pc %SAVE_VRAM_PC%

echo:
echo Running Amidog CPU tests (Debug)
..\psx-test\build\Debug\psx-test.exe %TEST_ARGS%
if errorlevel 1 (
    echo Amidog CPU tests ^(Debug^) FAILED
    popd
    EXIT /B 1
)
echo Amidog CPU tests (Debug) passed.

echo:
echo Running Amidog CPU tests (Release)
..\psx-test\build\Release\psx-test.exe %TEST_ARGS%
if errorlevel 1 (
    echo Amidog CPU tests ^(Release^) FAILED
    popd
    EXIT /B 1
)
echo Amidog CPU tests (Release) passed.

popd

EXIT /B
