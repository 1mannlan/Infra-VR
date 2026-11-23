@echo off
setlocal EnableDelayedExpansion
title INFRA VR - Standalone Installer

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

:: 2. Check if extracted directly into INFRA root or a subfolder of INFRA
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
    echo [INFRA VR] Could not auto-detect INFRA installation folder.
    set /p "INFRA_DIR=Please enter the full path to your INFRA folder (containing infra.exe): "
    if defined INFRA_DIR set "INFRA_DIR=!INFRA_DIR:"=!"
)

if not exist "%INFRA_DIR%\infra.exe" (
    echo [ERROR] infra.exe not found in "%INFRA_DIR%".
    echo Installation aborted.
    if "%NO_PAUSE%"=="0" pause
    exit /b 1
)

echo ============================================================
echo  Installing INFRA VR Mod into:
echo    %INFRA_DIR%
echo ============================================================

:: Stop any running INFRA instances before overwriting DLLs
taskkill /F /IM infra.exe >nul 2>&1
taskkill /F /IM NvRemixBridge.exe >nul 2>&1

:: Backup existing bin\d3d9.dll if present and not already INFRA VR / backed up
if exist "%INFRA_DIR%\bin\d3d9.dll" (
    findstr /C:"MenuOverlayKey" "%INFRA_DIR%\bin\d3d9.dll" >nul 2>&1
    if errorlevel 1 (
        if not exist "%INFRA_DIR%\bin\d3d9.dll.bak" (
            echo  - Backing up existing bin\d3d9.dll to bin\d3d9.dll.bak...
            copy /Y "%INFRA_DIR%\bin\d3d9.dll" "%INFRA_DIR%\bin\d3d9.dll.bak" >nul
        )
    ) else (
        echo  - Existing bin\d3d9.dll is already INFRA VR; keeping existing backup.
    )
)

:: Copy binaries
echo  - Installing bin\d3d9.dll and bin\openvr_api.dll...
copy /Y "%SCRIPT_DIR%bin\d3d9.dll" "%INFRA_DIR%\bin\d3d9.dll" >nul
if errorlevel 1 (
    echo [ERROR] Failed to copy bin\d3d9.dll ^(make sure INFRA is closed^).
    if "%NO_PAUSE%"=="0" pause
    exit /b 1
)
if exist "%INFRA_DIR%\infra\bin" (
    copy /Y "%SCRIPT_DIR%bin\d3d9.dll" "%INFRA_DIR%\infra\bin\d3d9.dll" >nul
)
if not exist "%INFRA_DIR%\VR\bin" mkdir "%INFRA_DIR%\VR\bin"
copy /Y "%SCRIPT_DIR%bin\d3d9.dll" "%INFRA_DIR%\VR\bin\d3d9.dll" >nul
copy /Y "%SCRIPT_DIR%bin\openvr_api.dll" "%INFRA_DIR%\bin\openvr_api.dll" >nul
if exist "%SCRIPT_DIR%bin\phonon.dll" (
    copy /Y "%SCRIPT_DIR%bin\phonon.dll" "%INFRA_DIR%\bin\phonon.dll" >nul
    copy /Y "%SCRIPT_DIR%bin\phonon.dll" "%INFRA_DIR%\phonon.dll" >nul
)

:: Copy VR configuration & OpenVR action manifests
echo  - Installing VR\ action manifests and controller profiles...
if not exist "%INFRA_DIR%\VR\SteamVRActionManifest" mkdir "%INFRA_DIR%\VR\SteamVRActionManifest"
if not exist "%INFRA_DIR%\VR\config.txt" (
    copy /Y "%SCRIPT_DIR%VR\config.txt" "%INFRA_DIR%\VR\config.txt" >nul
) else (
    echo    ^(Keeping existing VR\config.txt^)
)
copy /Y "%SCRIPT_DIR%VR\controller_profiles.txt" "%INFRA_DIR%\VR\controller_profiles.txt" >nul
copy /Y "%SCRIPT_DIR%VR\manifest.vrmanifest" "%INFRA_DIR%\VR\manifest.vrmanifest" >nul
if exist "%SCRIPT_DIR%VR\startup_*.*" xcopy /Y /Q "%SCRIPT_DIR%VR\startup_*.*" "%INFRA_DIR%\VR\" >nul
xcopy /Y /Q "%SCRIPT_DIR%VR\SteamVRActionManifest\*" "%INFRA_DIR%\VR\SteamVRActionManifest\" >nul

:: Copy VGUI laser/beam helper materials
echo  - Installing VGUI helper materials (infra\materials\vgui\)...
if not exist "%INFRA_DIR%\infra\materials\vgui" mkdir "%INFRA_DIR%\infra\materials\vgui"
xcopy /Y /Q "%SCRIPT_DIR%infra\materials\vgui\*" "%INFRA_DIR%\infra\materials\vgui\" >nul

:: Copy VR flashlight lens glow materials
echo  - Installing VR effect materials (infra\materials\vr\)...
if not exist "%INFRA_DIR%\infra\materials\vr" mkdir "%INFRA_DIR%\infra\materials\vr"
xcopy /Y /Q "%SCRIPT_DIR%infra\materials\vr\*" "%INFRA_DIR%\infra\materials\vr\" >nul

:: Copy matte VR hand material override
if exist "%SCRIPT_DIR%infra\materials\models\characters\player_hands.vmt" (
    echo  - Installing matte VR hand material override ^(infra\materials\models\characters\^)...
    if not exist "%INFRA_DIR%\infra\materials\models\characters" mkdir "%INFRA_DIR%\infra\materials\models\characters"
    copy /Y "%SCRIPT_DIR%infra\materials\models\characters\player_hands.vmt" "%INFRA_DIR%\infra\materials\models\characters\player_hands.vmt" >nul
)

:: Copy single-cell VR battery models
if exist "%SCRIPT_DIR%infra\models\vr" (
    echo  - Installing VR inventory models ^(infra\models\vr\^)...
    if not exist "%INFRA_DIR%\infra\models\vr" mkdir "%INFRA_DIR%\infra\models\vr"
    xcopy /Y /Q "%SCRIPT_DIR%infra\models\vr\*" "%INFRA_DIR%\infra\models\vr\" >nul
)

:: Copy camera flash LCD icons & scheme overrides
if exist "%SCRIPT_DIR%infra\vgui\fonts\INFRAicons.vfont" (
    if not exist "%INFRA_DIR%\infra\vgui\fonts" mkdir "%INFRA_DIR%\infra\vgui\fonts"
    copy /Y "%SCRIPT_DIR%infra\vgui\fonts\INFRAicons.vfont" "%INFRA_DIR%\infra\vgui\fonts\INFRAicons.vfont" >nul
    if not exist "%INFRA_DIR%\platform\vgui\fonts" mkdir "%INFRA_DIR%\platform\vgui\fonts"
    if exist "%INFRA_DIR%\platform\vgui\fonts\INFRAicons.vfont" (
        if not exist "%INFRA_DIR%\platform\vgui\fonts\INFRAicons.vfont.vr_bak" (
            copy /Y "%INFRA_DIR%\platform\vgui\fonts\INFRAicons.vfont" "%INFRA_DIR%\platform\vgui\fonts\INFRAicons.vfont.vr_bak" >nul
        )
    )
    copy /Y "%SCRIPT_DIR%infra\vgui\fonts\INFRAicons.vfont" "%INFRA_DIR%\platform\vgui\fonts\INFRAicons.vfont" >nul
)
if exist "%SCRIPT_DIR%infra\resource\CameraScheme.res" (
    if not exist "%INFRA_DIR%\infra\resource" mkdir "%INFRA_DIR%\infra\resource"
    if exist "%INFRA_DIR%\infra\resource\CameraScheme.res" (
        if not exist "%INFRA_DIR%\infra\resource\CameraScheme.res.vr_bak" (
            copy /Y "%INFRA_DIR%\infra\resource\CameraScheme.res" "%INFRA_DIR%\infra\resource\CameraScheme.res.vr_bak" >nul
        )
    )
    copy /Y "%SCRIPT_DIR%infra\resource\CameraScheme.res" "%INFRA_DIR%\infra\resource\CameraScheme.res" >nul
)
if exist "%SCRIPT_DIR%infra\resource\MenuScheme.res" (
    if not exist "%INFRA_DIR%\infra\resource" mkdir "%INFRA_DIR%\infra\resource"
    if exist "%INFRA_DIR%\infra\resource\MenuScheme.res" (
        if not exist "%INFRA_DIR%\infra\resource\MenuScheme.res.vr_bak" (
            copy /Y "%INFRA_DIR%\infra\resource\MenuScheme.res" "%INFRA_DIR%\infra\resource\MenuScheme.res.vr_bak" >nul
        )
    )
    copy /Y "%SCRIPT_DIR%infra\resource\MenuScheme.res" "%INFRA_DIR%\infra\resource\MenuScheme.res" >nul
)

:: Synchronize Spatial Audio mode (steam_audio | dsoal | native) from VR\config.txt
set "SPATIAL_AUDIO_MODE=steam_audio"
if exist "%INFRA_DIR%\VR\config.txt" (
    for /f "usebackq tokens=1,2 delims==#" %%A in (`findstr /i /b /c:"SpatialAudioMode" "%INFRA_DIR%\VR\config.txt" 2^>nul`) do (
        for /f "tokens=1" %%V in ("%%~B") do set "SPATIAL_AUDIO_MODE=%%~V"
    )
)
if /I "!SPATIAL_AUDIO_MODE!"=="dsoal" (
    if exist "%SCRIPT_DIR%dsoal\dsound.dll" (
        copy /Y "%SCRIPT_DIR%dsoal\dsound.dll" "%INFRA_DIR%\dsound.dll" >nul
        copy /Y "%SCRIPT_DIR%dsoal\dsoal-aldrv.dll" "%INFRA_DIR%\dsoal-aldrv.dll" >nul
        copy /Y "%SCRIPT_DIR%dsoal\alsoft.ini" "%INFRA_DIR%\alsoft.ini" >nul
        echo  - Spatial Audio: DSOAL + OpenAL Soft HRTF enabled.
    )
) else (
    if exist "%INFRA_DIR%\dsound.dll" del /F /Q "%INFRA_DIR%\dsound.dll" >nul 2>&1
    if exist "%INFRA_DIR%\dsoal-aldrv.dll" del /F /Q "%INFRA_DIR%\dsoal-aldrv.dll" >nul 2>&1
    if exist "%INFRA_DIR%\alsoft.ini" del /F /Q "%INFRA_DIR%\alsoft.ini" >nul 2>&1
    echo  - Spatial Audio: Steam Audio / Native mode active ^(DSOAL removed^).
)
if exist "%INFRA_DIR%\infra\custom\infra_vr" rmdir /S /Q "%INFRA_DIR%\infra\custom\infra_vr" >nul 2>&1

:: Save detected path for start_infra_vr.bat / uninstall_vr.bat
>"%SCRIPT_DIR%.infra_path.txt" echo !INFRA_DIR!

echo.
echo [OK] INFRA VR Mod installed successfully!
echo      Run start_infra_vr.bat or launch INFRA via Steam with SteamVR active.
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
