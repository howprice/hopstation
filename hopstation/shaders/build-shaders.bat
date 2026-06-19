@echo off

REM Adapted from https://github.com/TheSpydog/SDL_gpu_examples

setlocal enabledelayedexpansion

REM Set the current directory to the location of the batch script, using the %0 parameter
REM This allows the script to be called from anywhere
pushd "%~dp0"

set SHADERCROSS=..\..\bin\x64\shadercross.exe
set BIN=..\..\data\shaders

if not exist "%SHADERCROSS%" (
    echo ERROR: shadercross.exe not found at %SHADERCROSS%
    echo Please build it first using scripts\build_SDL_shadercross.bat
    popd
    EXIT /B 1
)

if not exist "%BIN%" mkdir "%BIN%"

REM Clean output directory to avoid loading stale shaders
del /Q "%BIN%\*" 2>nul

REM Infer shader stage from filename

for %%f in (*.vert.hlsl) do (
    if exist "%%f" (
        echo Compiling %%f to %%~nf.dxil
        %SHADERCROSS% "%%f" -s hlsl -d dxil -t vertex -e "main" -o "%BIN%\%%~nf.dxil"  || EXIT /B 1
    )
)

for %%f in (*.frag.hlsl) do (
    if exist "%%f" (
        echo Compiling %%f to %%~nf.dxil
        %SHADERCROSS% "%%f" -s hlsl -d dxil -t fragment -e "main" -o "%BIN%\%%~nf.dxil"  || EXIT /B 1
    )
)

popd

EXIT /B 0
