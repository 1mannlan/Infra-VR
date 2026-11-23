@echo off
setlocal EnableDelayedExpansion
title INFRA VR Launcher

set "SCRIPT_DIR=%~dp0"
call "%SCRIPT_DIR%install_vr.bat" --no-pause
if errorlevel 1 (
    echo [ERROR] Automatic VR installation failed.
    pause
    exit /b 1
)

set "INFRA_DIR="
if exist "%SCRIPT_DIR%.infra_path.txt" (
    set /p INFRA_DIR=<"%SCRIPT_DIR%.infra_path.txt"
)
if not defined INFRA_DIR (
    set "INFRA_DIR=C:\Program Files (x86)\Steam\steamapps\common\infra"
)
if not exist "%INFRA_DIR%\infra.exe" (
    echo [ERROR] INFRA installation not found at "%INFRA_DIR%".
    pause
    exit /b 1
)

:: Start SteamVR if vrserver.exe is not yet running
tasklist /FI "IMAGENAME eq vrserver.exe" 2>NUL | find /I /N "vrserver.exe">NUL
if "%ERRORLEVEL%" NEQ "0" (
    echo [1/2] Starting SteamVR...
    start "" "steam://run/250820"
    timeout /t 5 /nobreak >nul
) else (
    echo [1/2] SteamVR is already running.
)

:: Ensure mat_queue_mode starts at 0 so VR eye textures initialize synchronously on the main thread
:: (the mod switches to mat_queue_mode 2 automatically once VR eye textures exist)
powershell -NoProfile -Command "$files = @('%INFRA_DIR%\infra\cfg\video.txt', '%INFRA_DIR%\infra\cfg\videodefaults.txt'); foreach ($f in $files) { if (Test-Path $f) { $c = Get-Content $f -Raw; if ($f -like '*video.txt') { $c = $c -replace '^\s*\"config\"', '\"VideoConfig\"'; }; $c = $c -replace '(\"setting\.fullscreen\"\s+)\"\d+\"', '$1\"0\"'; $c = $c -replace '(\"setting\.nowindowborder\"\s+)\"\d+\"', '$1\"0\"'; $c = $c -replace '(\"setting\.mat_queue_mode\"\s+)\"-?\d+\"', '$1\"0\"'; $c = $c -replace '(\"setting\.defaultres\"\s+)\"\d+\"', '$1\"1280\"'; $c = $c -replace '(\"setting\.defaultresheight\"\s+)\"\d+\"', '$1\"720\"'; Set-Content $f -Value $c -NoNewline } }" >nul 2>&1

:: Read SpatialAudioMode (steam_audio | dsoal | native) from VR\config.txt so Source speaker/DSP cvars stay in sync
set "SPATIAL_AUDIO_MODE=steam_audio"
if exist "%INFRA_DIR%\VR\config.txt" (
    for /f "usebackq tokens=1,2 delims==#" %%A in (`findstr /i /b /c:"SpatialAudioMode" "%INFRA_DIR%\VR\config.txt" 2^>nul`) do (
        for /f "tokens=1" %%V in ("%%~B") do set "SPATIAL_AUDIO_MODE=%%~V"
    )
)
if /I "!SPATIAL_AUDIO_MODE!"=="dsoal" (
    if exist "%INFRA_DIR%\dsound.dll" (
        set "AUDIO_ARGS=+windows_speaker_config 5 +snd_surround_speakers 5 +snd_legacy_surround 1 +dsp_enhance_stereo 0"
    ) else (
        set "AUDIO_ARGS=+windows_speaker_config 0 +snd_surround_speakers 0 +snd_legacy_surround 0 +dsp_enhance_stereo 1"
    )
) else if /I "!SPATIAL_AUDIO_MODE!"=="native" (
    set "AUDIO_ARGS=+windows_speaker_config 0 +snd_surround_speakers 0 +snd_legacy_surround 0 +dsp_enhance_stereo 1"
) else (
    set "AUDIO_ARGS=+windows_speaker_config 0 +snd_surround_speakers 0 +snd_legacy_surround 0 +dsp_enhance_stereo 0"
)

echo [2/2] Launching INFRA in VR Mode (1280x720 Windowed)...
pushd "%INFRA_DIR%"
start "" "%INFRA_DIR%\infra.exe" -steam -game infra -insecure -nojoy -sw -windowed -w 1280 -h 720 -novid +joystick 0 +mat_dynamic_tonemapping 0 +mat_disable_bloom 1 +mat_bloomscale 0 +mat_motion_blur_percent_of_screen_max 0 +mat_queue_mode 0 +r_threaded_particles 1 +r_threaded_renderables 1 +cl_threaded_bone_setup 1 +snd_mix_async 1 %AUDIO_ARGS% +r_queued_decals 0 +r_queued_ropes 0 +mat_vsync 0 +engine_no_focus_sleep 0 +fps_max 0
popd
