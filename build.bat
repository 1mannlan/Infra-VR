@echo off
setlocal EnableDelayedExpansion
title Build INFRA VR (d3d9.dll)

set "REPO_DIR=%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "MSBUILD="

if not exist "%VSWHERE%" (
    echo [ERROR] vswhere.exe not found at "%VSWHERE%".
    exit /b 1
)

for /f "usebackq delims=" %%I in (`""!VSWHERE!" -products * -latest -find "MSBuild\**\Bin\MSBuild.exe""`) do (
    if not defined MSBUILD set "MSBUILD=%%I"
)

if not defined MSBUILD (
    echo [ERROR] Could not locate MSBuild.exe via vswhere.exe.
    exit /b 1
)

echo [1/2] Building InfraVR.sln (Release^|x86) using "%MSBUILD%"...
"%MSBUILD%" "%REPO_DIR%InfraVR.sln" /p:Configuration=Release /p:Platform=x86 /m /v:minimal
set "BUILD_ERR=%ERRORLEVEL%"

if "%BUILD_ERR%" NEQ "0" (
    echo [ERROR] Build failed with code %BUILD_ERR%.
    exit /b %BUILD_ERR%
)

if not exist "%REPO_DIR%dist\bin" mkdir "%REPO_DIR%dist\bin"
copy /Y "%REPO_DIR%Release\d3d9.dll" "%REPO_DIR%dist\bin\d3d9.dll" >nul

echo [2/2] Successfully built dist\bin\d3d9.dll
