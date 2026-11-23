@echo off
setlocal EnableDelayedExpansion
title Package INFRA VR Release ZIP

set "REPO_DIR=%~dp0"
set "VERSION=%~1"
if "%VERSION%"=="" set "VERSION=v0.1.0"

set "ZIP_NAME=Infra-VR-%VERSION%.zip"
if not exist "%REPO_DIR%dist" mkdir "%REPO_DIR%dist"
set "ZIP_PATH=%REPO_DIR%dist\%ZIP_NAME%"
set "STAGE_DIR=%REPO_DIR%_release_stage"

echo ============================================================
echo  Packaging INFRA VR Release: %ZIP_NAME%
echo ============================================================

if exist "%REPO_DIR%Infra-VR-*.zip" del /F /Q "%REPO_DIR%Infra-VR-*.zip"
if exist "%REPO_DIR%dist\Infra-VR-*.zip" del /F /Q "%REPO_DIR%dist\Infra-VR-*.zip"
if exist "%STAGE_DIR%" rmdir /S /Q "%STAGE_DIR%"
if exist "%ZIP_PATH%" del /F /Q "%ZIP_PATH%"

mkdir "%STAGE_DIR%"
xcopy /E /I /Y /Q "%REPO_DIR%dist\*" "%STAGE_DIR%\" >nul
if exist "%STAGE_DIR%\.infra_path.txt" del /F /Q "%STAGE_DIR%\.infra_path.txt"
if exist "%STAGE_DIR%\*.zip" del /F /Q "%STAGE_DIR%\*.zip"
if exist "%STAGE_DIR%\VR\finger_debug.txt" del /F /Q "%STAGE_DIR%\VR\finger_debug.txt"
if exist "%STAGE_DIR%\VR\profile_*.txt" del /F /Q "%STAGE_DIR%\VR\profile_*.txt"
copy /Y "%REPO_DIR%README.md" "%STAGE_DIR%\README.md" >nul
copy /Y "%REPO_DIR%LICENSE" "%STAGE_DIR%\LICENSE.txt" >nul
copy /Y "%REPO_DIR%THIRD_PARTY_LICENSES.md" "%STAGE_DIR%\THIRD_PARTY_LICENSES.md" >nul
copy /Y "%REPO_DIR%InfraVR\sdk\LICENSE.txt" "%STAGE_DIR%\SOURCE_SDK_LICENSE.txt" >nul
copy /Y "%REPO_DIR%InfraVR\sdk\thirdpartylegalnotices.txt" "%STAGE_DIR%\thirdpartylegalnotices.txt" >nul

powershell -NoProfile -Command "Compress-Archive -Path '%STAGE_DIR%\*' -DestinationPath '%ZIP_PATH%' -Force"
set "ZIP_ERR=%ERRORLEVEL%"

rmdir /S /Q "%STAGE_DIR%"

if "%ZIP_ERR%" NEQ "0" (
    echo [ERROR] Failed to create %ZIP_NAME%.
    exit /b %ZIP_ERR%
)

echo [OK] Created release package: %ZIP_PATH%
echo      Upload this file to your GitHub Release!
