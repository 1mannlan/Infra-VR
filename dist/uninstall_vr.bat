@echo off
setlocal EnableDelayedExpansion
title INFRA VR - Uninstaller

set "SCRIPT_DIR=%~dp0"
set "INFRA_DIR="
set "SILENT=0"
set "NO_PAUSE=0"
if /i "%~1"=="--silent" (
    set "SILENT=1"
    set "NO_PAUSE=1"
)
if /i "%~1"=="--no-pause" set "NO_PAUSE=1"

:: 1. Check cached .infra_path.txt (validates infra.exe still exists there)
if exist "%SCRIPT_DIR%.infra_path.txt" (
    for /f "usebackq delims=" %%P in ("%SCRIPT_DIR%.infra_path.txt") do set "INFRA_DIR=%%~P"
    if defined INFRA_DIR if not exist "!INFRA_DIR!\infra.exe" set "INFRA_DIR="
)

:: 2. Check if located directly inside INFRA root or a subfolder
if not defined INFRA_DIR (
    if exist "%SCRIPT_DIR%infra.exe" (
        set "INFRA_DIR=%SCRIPT_DIR:~0,-1%"
    ) else if exist "%SCRIPT_DIR%..\infra.exe" (
        for %%I in ("%SCRIPT_DIR%..") do set "INFRA_DIR=%%~fI"
    )
)

:: 3. Check default Steam path
if not defined INFRA_DIR (
    if exist "C:\Program Files (x86)\Steam\steamapps\common\infra\infra.exe" (
        set "INFRA_DIR=C:\Program Files (x86)\Steam\steamapps\common\infra"
    )
)

:: 4. Query Steam Registry (HKCU + HKLM) and parse steamapps\libraryfolders.vdf / config\libraryfolders.vdf
if not defined INFRA_DIR call :CheckSteamRegistry "HKCU\Software\Valve\Steam" "SteamPath"
if not defined INFRA_DIR call :CheckSteamRegistry "HKLM\SOFTWARE\WOW6432Node\Valve\Steam" "InstallPath"
if not defined INFRA_DIR call :CheckSteamRegistry "HKLM\SOFTWARE\Valve\Steam" "InstallPath"

:: 5. Ask user if still not found
if not defined INFRA_DIR (
    if "%SILENT%"=="1" (
        echo [ERROR] Could not auto-detect INFRA installation folder.
        exit /b 1
    )
    set /p "INFRA_DIR=Please enter the full path to your INFRA folder (containing infra.exe): "
    if defined INFRA_DIR set "INFRA_DIR=!INFRA_DIR:"=!"
)

if not exist "%INFRA_DIR%\infra.exe" (
    echo [ERROR] infra.exe not found in "%INFRA_DIR%".
    if "%NO_PAUSE%"=="0" pause
    exit /b 1
)

echo ============================================================
echo  Uninstalling INFRA VR Mod from:
echo    %INFRA_DIR%
echo ============================================================

if exist "%INFRA_DIR%\bin\d3d9.dll" (
    if exist "%INFRA_DIR%\bin\d3d9.dll.bak" (
        del /F /Q "%INFRA_DIR%\bin\d3d9.dll"
    ) else (
        findstr /C:"MenuOverlayKey" "%INFRA_DIR%\bin\d3d9.dll" >nul 2>&1
        if not errorlevel 1 (
            del /F /Q "%INFRA_DIR%\bin\d3d9.dll"
        ) else (
            echo  - Keeping non-VR bin\d3d9.dll ^(not installed by INFRA VR^).
        )
    )
)
if exist "%INFRA_DIR%\bin\d3d9.dll.bak" (
    echo  - Restoring previous bin\d3d9.dll.bak...
    move /Y "%INFRA_DIR%\bin\d3d9.dll.bak" "%INFRA_DIR%\bin\d3d9.dll" >nul
)
if exist "%INFRA_DIR%\infra\bin\d3d9.dll" (
    findstr /C:"MenuOverlayKey" "%INFRA_DIR%\infra\bin\d3d9.dll" >nul 2>&1
    if not errorlevel 1 del /F /Q "%INFRA_DIR%\infra\bin\d3d9.dll"
)
if exist "%INFRA_DIR%\VR\bin\d3d9.dll" del /F /Q "%INFRA_DIR%\VR\bin\d3d9.dll"
if exist "%INFRA_DIR%\VR\bin" rd "%INFRA_DIR%\VR\bin" 2>nul
if exist "%INFRA_DIR%\bin\openvr_api.dll" del /F /Q "%INFRA_DIR%\bin\openvr_api.dll"
if exist "%INFRA_DIR%\bin\phonon.dll" del /F /Q "%INFRA_DIR%\bin\phonon.dll"
if exist "%INFRA_DIR%\phonon.dll" del /F /Q "%INFRA_DIR%\phonon.dll"
if exist "%INFRA_DIR%\dsound.dll" del /F /Q "%INFRA_DIR%\dsound.dll"
if exist "%INFRA_DIR%\dsoal-aldrv.dll" del /F /Q "%INFRA_DIR%\dsoal-aldrv.dll"
if exist "%INFRA_DIR%\alsoft.ini" del /F /Q "%INFRA_DIR%\alsoft.ini"
if exist "%INFRA_DIR%\infra\materials\vr" rmdir /S /Q "%INFRA_DIR%\infra\materials\vr"
if exist "%INFRA_DIR%\infra\models\vr" rmdir /S /Q "%INFRA_DIR%\infra\models\vr"
if exist "%INFRA_DIR%\infra\models" rd "%INFRA_DIR%\infra\models" 2>nul
if exist "%INFRA_DIR%\infra\materials\models\characters\player_hands.vmt" (
    del /F /Q "%INFRA_DIR%\infra\materials\models\characters\player_hands.vmt"
    rd "%INFRA_DIR%\infra\materials\models\characters" 2>nul
    rd "%INFRA_DIR%\infra\materials\models" 2>nul
)
if exist "%INFRA_DIR%\infra\resource\CameraScheme.res.vr_bak" (
    move /Y "%INFRA_DIR%\infra\resource\CameraScheme.res.vr_bak" "%INFRA_DIR%\infra\resource\CameraScheme.res" >nul
)
if exist "%INFRA_DIR%\infra\resource\MenuScheme.res.vr_bak" (
    move /Y "%INFRA_DIR%\infra\resource\MenuScheme.res.vr_bak" "%INFRA_DIR%\infra\resource\MenuScheme.res" >nul
)
if exist "%INFRA_DIR%\platform\vgui\fonts\INFRAicons.vfont.vr_bak" (
    move /Y "%INFRA_DIR%\platform\vgui\fonts\INFRAicons.vfont.vr_bak" "%INFRA_DIR%\platform\vgui\fonts\INFRAicons.vfont" >nul
)
if exist "%INFRA_DIR%\infra\vgui\fonts\INFRAicons.vfont" (
    del /F /Q "%INFRA_DIR%\infra\vgui\fonts\INFRAicons.vfont"
    rd "%INFRA_DIR%\infra\vgui\fonts" 2>nul
    rd "%INFRA_DIR%\infra\vgui" 2>nul
)
if exist "%INFRA_DIR%\infra\custom\infra_vr" rmdir /S /Q "%INFRA_DIR%\infra\custom\infra_vr"
if exist "%INFRA_DIR%\infra\custom" rd "%INFRA_DIR%\infra\custom" 2>nul
for %%F in (black.vtf white.vmt white.vtf white_additive.vmt white_additive.vtf) do (
    if exist "%INFRA_DIR%\infra\materials\vgui\%%F" del /F /Q "%INFRA_DIR%\infra\materials\vgui\%%F"
)
if exist "%INFRA_DIR%\infra\materials\vgui" rd "%INFRA_DIR%\infra\materials\vgui" 2>nul
if exist "%INFRA_DIR%\infra\materials" rd "%INFRA_DIR%\infra\materials" 2>nul
if exist "%INFRA_DIR%\infra\vr_vprof.log" del /F /Q "%INFRA_DIR%\infra\vr_vprof.log"
if exist "%INFRA_DIR%\VR\controller_profiles.txt" del /F /Q "%INFRA_DIR%\VR\controller_profiles.txt"
if exist "%INFRA_DIR%\VR\manifest.vrmanifest" del /F /Q "%INFRA_DIR%\VR\manifest.vrmanifest"
if exist "%INFRA_DIR%\VR\startup_*.*" del /F /Q "%INFRA_DIR%\VR\startup_*.*"
if exist "%INFRA_DIR%\VR\finger_debug.txt" del /F /Q "%INFRA_DIR%\VR\finger_debug.txt"
if exist "%INFRA_DIR%\VR\spatial_audio.log" del /F /Q "%INFRA_DIR%\VR\spatial_audio.log"
if exist "%INFRA_DIR%\VR\profile_*.txt" del /F /Q "%INFRA_DIR%\VR\profile_*.txt"
if exist "%INFRA_DIR%\VR\SteamVRActionManifest" rmdir /S /Q "%INFRA_DIR%\VR\SteamVRActionManifest"
if exist "%SCRIPT_DIR%.infra_path.txt" del /F /Q "%SCRIPT_DIR%.infra_path.txt"

echo.
echo [OK] INFRA VR Mod removed. (Your VR\config.txt settings were kept.)
if "%NO_PAUSE%"=="0" pause
goto :EOF

:CheckSteamRegistry
if defined INFRA_DIR goto :EOF
for /f "tokens=2*" %%A in ('reg query "%~1" /v "%~2" 2^>nul') do (
    if not defined INFRA_DIR call :CheckSteamRoot "%%B"
)
goto :EOF

:CheckSteamRoot
if defined INFRA_DIR goto :EOF
set "STEAM_PATH=%~1"
if not defined STEAM_PATH goto :EOF
set "STEAM_PATH=!STEAM_PATH:/=\!"
if "!STEAM_PATH:~-1!"=="\" set "STEAM_PATH=!STEAM_PATH:~0,-1!"
if exist "!STEAM_PATH!\steamapps\common\infra\infra.exe" (
    set "INFRA_DIR=!STEAM_PATH!\steamapps\common\infra"
    goto :EOF
)
if exist "!STEAM_PATH!\steamapps\libraryfolders.vdf" (
    call :ParseVdfFile "!STEAM_PATH!\steamapps\libraryfolders.vdf"
)
if not defined INFRA_DIR if exist "!STEAM_PATH!\config\libraryfolders.vdf" (
    call :ParseVdfFile "!STEAM_PATH!\config\libraryfolders.vdf"
)
goto :EOF

:ParseVdfFile
if defined INFRA_DIR goto :EOF
if not exist "%~1" goto :EOF
for /f "usebackq tokens=1,* delims=	 " %%K in ("%~1") do (
    if /i "%%~K"=="path" (
        set "LIB_PATH=%%~L"
        set "LIB_PATH=!LIB_PATH:\\=\!"
        if "!LIB_PATH:~-1!"=="\" set "LIB_PATH=!LIB_PATH:~0,-1!"
        if not defined INFRA_DIR if exist "!LIB_PATH!\steamapps\common\infra\infra.exe" (
            set "INFRA_DIR=!LIB_PATH!\steamapps\common\infra"
        )
    )
)
goto :EOF
