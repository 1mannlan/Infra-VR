#include "vr.h"
#include "vr_audio.h"
#include "vr_grab.h"
#include "vr_hands.h"
#include <Windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <d3d9.h>
#include <mmsystem.h>
#include <condition_variable>
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#include "sdk.h"
#include "game.h"
#include "hooks.h"
#include "trace.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <string>
#include <filesystem>
#include <thread>
#include <type_traits>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <d3d9_vr.h>

namespace
{
    bool  s_R3WasDown = false;
    float s_R3HoldMs = 0.0f;
    bool  s_R3SawL3 = false;
    bool  s_R3SuppressUntilRelease = false;
    bool  s_SuppressPrimaryAttackUntilRelease = false;
    bool  s_CalWaitRightGripRelease = false;
    bool  s_CalWaitLeftGripRelease  = false;
    float s_CalRightGripHoldMs = 0.0f;
    float s_CalLeftGripHoldMs  = 0.0f;
    bool  s_CalHaveSavedOffsets = false;
    Vector s_CalSavedRightPos{};
    QAngle s_CalSavedRightAng{};
    Vector s_CalSavedLeftPos{};
    QAngle s_CalSavedLeftAng{};

    struct TouchpadSwipeUpState
    {
        bool wasTouching = false;
        bool hasStartY = false;
        bool swipeTriggered = false;
        int  zeroFrames = 0;
        float startY = 0.0f;
        float peakY = 0.0f;
        float prevX = 0.0f;
        float prevY = 0.0f;
        int unchangedFrames = 0;
        std::chrono::steady_clock::time_point lastTouchTime = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point lastTriggerTime = std::chrono::steady_clock::now() - std::chrono::seconds(5);
    };

    TouchpadSwipeUpState s_LeftTpUpState{};
    std::chrono::steady_clock::time_point s_LastLeftSwipeDownTime = std::chrono::steady_clock::now() - std::chrono::seconds(5);
}

VR::VR(Game *game) 
{
    m_Game = game;

    char errorString[MAX_STR_LEN];

    vr::HmdError error = vr::VRInitError_None;
    {
        std::lock_guard<std::mutex> vrLock(GetOpenVRRuntimeMutex());
        m_System = vr::VR_Init(&error, vr::VRApplication_Scene);
    }

    if (error != vr::VRInitError_None) 
    {
        snprintf(errorString, MAX_STR_LEN, "VR_Init failed: %s", vr::VR_GetVRInitErrorAsEnglishDescription(error));
        Game::errorMsg(errorString);
        return;
    }

    vr::EVRInitError peError = vr::VRInitError_None;

    if (!vr::VRCompositor())
    {
        Game::errorMsg("Compositor initialization failed.");
        return;
    }

    m_Input = vr::VRInput();
    m_System = vr::OpenVRInternal_ModuleContext().VRSystem();

    m_AntiAliasing = 4;
    GetConfiguredVRRenderTargetSize(&m_RenderWidth, &m_RenderHeight, &m_RenderScale);
    if (m_RenderWidth == 0 || m_RenderHeight == 0)
        m_System->GetRecommendedRenderTargetSize(&m_RenderWidth, &m_RenderHeight);

    float l_left = 0.0f, l_right = 0.0f, l_top = 0.0f, l_bottom = 0.0f;
    m_System->GetProjectionRaw(vr::EVREye::Eye_Left, &l_left, &l_right, &l_top, &l_bottom);

    float r_left = 0.0f, r_right = 0.0f, r_top = 0.0f, r_bottom = 0.0f;
    m_System->GetProjectionRaw(vr::EVREye::Eye_Right, &r_left, &r_right, &r_top, &r_bottom);

    float tanHalfFov[2];

    tanHalfFov[0] = std::max({ -l_left, l_right, -r_left, r_right });
    tanHalfFov[1] = std::max({ -l_top, l_bottom, -r_top, r_bottom });

    m_TextureBounds[0].uMin = 0.5f + 0.5f * l_left / tanHalfFov[0];
    m_TextureBounds[0].uMax = 0.5f + 0.5f * l_right / tanHalfFov[0];
    m_TextureBounds[0].vMin = 0.5f - 0.5f * l_bottom / tanHalfFov[1];
    m_TextureBounds[0].vMax = 0.5f - 0.5f * l_top / tanHalfFov[1];

    m_TextureBounds[1].uMin = 0.5f + 0.5f * r_left / tanHalfFov[0];
    m_TextureBounds[1].uMax = 0.5f + 0.5f * r_right / tanHalfFov[0];
    m_TextureBounds[1].vMin = 0.5f - 0.5f * r_bottom / tanHalfFov[1];
    m_TextureBounds[1].vMax = 0.5f - 0.5f * r_top / tanHalfFov[1];

    m_TanHalfFovX = tanHalfFov[0];
    m_TanHalfFovY = tanHalfFov[1];
    m_Aspect = tanHalfFov[0] / tanHalfFov[1];
    m_Fov = 2.0f * atan(tanHalfFov[0]) * 360 / (3.14159265358979323846 * 2);

    InstallApplicationManifest("manifest.vrmanifest");
    SetActionManifest("action_manifest.json");

    ParseConfigFile();

    DWORD startWait = GetTickCount();
    while (!g_D3DVR9) 
    {
        if (GetTickCount() - startWait > 120000)
        {
            Game::errorMsg("Timed out waiting for DXVK D3D9VR interface");
            return;
        }
        Sleep(10);
    }

    m_Overlay = vr::VROverlay();
    if (m_Overlay)
    {
        m_Overlay->CreateOverlay("MenuOverlayKey", "MenuOverlay", &m_MainMenuHandle);
        m_Overlay->SetOverlayInputMethod(m_MainMenuHandle, vr::VROverlayInputMethod_None);
        m_Overlay->SetOverlayFlag(m_MainMenuHandle, vr::VROverlayFlags_SendVRDiscreteScrollEvents, false);

        const vr::HmdVector2_t mouseScaleMenu = {(float)m_RenderWidth, (float)m_RenderHeight};
        m_Overlay->SetOverlayCurvature(m_MainMenuHandle, 0.0f);
        m_Overlay->SetOverlayMouseScale(m_MainMenuHandle, &mouseScaleMenu);

        InitStartupSplash();
    }

    // Initial poses + input state (WaitGetPoses itself now runs per frame in RenderThreadPresent)
    UpdatePredictedPoses();
    {
        std::lock_guard<std::mutex> lock(m_RenderPosesMutex);
        memcpy(m_RenderPoses, m_Poses, sizeof(m_RenderPoses));
    }
    UpdateActionsAndControllers();

    m_ConfigThread = std::thread(&VR::WaitForConfigUpdate, this);
    m_IsInitialized = true;
    m_IsVREnabled = true;
}

VR::~VR()
{
    DestroyStartupSplash();

    m_ConfigStopRequested = true;
    if (m_ConfigThread.joinable())
    {
        m_ConfigThread.join();
    }

    UninstallBackgroundFocus();

    if (m_MainMenuHandle != vr::k_ulOverlayHandleInvalid)
    {
        vr::IVROverlay* pOverlay = vr::VROverlay();
        if (pOverlay)
        {
            pOverlay->DestroyOverlay(m_MainMenuHandle);
        }
        m_MainMenuHandle = vr::k_ulOverlayHandleInvalid;
    }

    // Note: m_D9*Surface are non-owning pointers to D3D9Texture2D subresource 0
    // (CreateTexture in d3d9_device.cpp releases its temporary GetSurfaceLevel reference
    // so Source Engine's IMaterialSystem remains sole owner).
    m_D9LeftEyeSurface = nullptr;
    m_D9RightEyeSurface = nullptr;
    m_D9HUDSurface = nullptr;
    m_D9BlankSurface = nullptr;

    if (m_System)
    {
        std::lock_guard<std::mutex> vrLock(GetOpenVRRuntimeMutex());
        vr::VR_Shutdown();
        m_System = nullptr;
    }

    m_IsInitialized = false;
    m_IsVREnabled = false;
}

int VR::SetActionManifest(const char *fileName) 
{
    std::string relPath = std::string("VR\\SteamVRActionManifest\\") + (fileName ? fileName : "");
    std::string path = ResolveGameRelativePath(relPath.c_str());

    if (m_Input->SetActionManifestPath(path.c_str()) != vr::VRInputError_None) 
    {
        Game::errorMsg("SetActionManifestPath failed");
    }

    m_Input->GetActionHandle("/actions/main/in/ActivateVR", &m_ActionActivateVR);
    m_Input->GetActionHandle("/actions/main/in/Jump", &m_ActionJump);
    m_Input->GetActionHandle("/actions/main/in/PrimaryAttack", &m_ActionPrimaryAttack);
    m_Input->GetActionHandle("/actions/main/in/Reload", &m_ActionReload);
    m_Input->GetActionHandle("/actions/main/in/Use", &m_ActionUse);
    m_Input->GetActionHandle("/actions/main/in/Walk", &m_ActionWalk);
    m_Input->GetActionHandle("/actions/main/in/Turn", &m_ActionTurn);
    m_Input->GetActionHandle("/actions/main/in/SecondaryAttack", &m_ActionSecondaryAttack);
    m_Input->GetActionHandle("/actions/main/in/NextItem", &m_ActionNextItem);
    m_Input->GetActionHandle("/actions/main/in/PrevItem", &m_ActionPrevItem);
    m_Input->GetActionHandle("/actions/main/in/ResetPosition", &m_ActionResetPosition);
    m_Input->GetActionHandle("/actions/main/in/Crouch", &m_ActionCrouch);
    m_Input->GetActionHandle("/actions/main/in/Flashlight", &m_ActionFlashlight);
    m_Input->GetActionHandle("/actions/main/in/MenuSelect", &m_MenuSelect);
    m_Input->GetActionHandle("/actions/main/in/MenuBack", &m_MenuBack);
    m_Input->GetActionHandle("/actions/main/in/MenuUp", &m_MenuUp);
    m_Input->GetActionHandle("/actions/main/in/MenuDown", &m_MenuDown);
    m_Input->GetActionHandle("/actions/main/in/MenuLeft", &m_MenuLeft);
    m_Input->GetActionHandle("/actions/main/in/MenuRight", &m_MenuRight);
    m_Input->GetActionHandle("/actions/main/in/Spray", &m_Spray);
    m_Input->GetActionHandle("/actions/main/in/Scoreboard", &m_Scoreboard);
    m_Input->GetActionHandle("/actions/main/in/ShowHUD", &m_ShowHUD);
    m_Input->GetActionHandle("/actions/main/in/Pause", &m_Pause);
    m_Input->GetActionHandle("/actions/main/in/LeftTrackpad", &m_ActionLeftTrackpad);
    m_Input->GetActionHandle("/actions/main/in/RightTrackpad", &m_ActionRightTrackpad);
    m_Input->GetActionHandle("/actions/main/in/ReloadFlashlight", &m_ActionReloadFlashlight);
    m_Input->GetActionHandle("/actions/main/in/ReloadCamera", &m_ActionReloadCamera);
    m_Input->GetActionHandle("/actions/base/in/skeleton_lefthand", &m_ActionSkeletonLeft);
    m_Input->GetActionHandle("/actions/base/in/skeleton_righthand", &m_ActionSkeletonRight);
    m_Input->GetActionHandle("/actions/base/out/vibration_left", &m_ActionVibrationLeft);
    m_Input->GetActionHandle("/actions/base/out/vibration_right", &m_ActionVibrationRight);

    m_Input->GetActionSetHandle("/actions/main", &m_ActionSet);
    m_Input->GetActionSetHandle("/actions/base", &m_BaseActionSet);
    m_ActiveActionSet = {};
    m_ActiveActionSet.ulActionSet = m_ActionSet;
    m_ActiveActionSets[0] = {};
    m_ActiveActionSets[0].ulActionSet = m_ActionSet;
    m_ActiveActionSets[1] = {};
    m_ActiveActionSets[1].ulActionSet = m_BaseActionSet;

    return 0;
}

void VR::InstallApplicationManifest(const char *fileName)
{
    std::string relPath = std::string("VR\\") + (fileName ? fileName : "");
    std::string path = ResolveGameRelativePath(relPath.c_str());

    vr::VRApplications()->AddApplicationManifest(path.c_str());
}

void VR::SetScreenSizeOverride(bool bState) {
    bool isOverriding = m_Game->m_VguiSurface->IsScreenSizeOverrideActive();

    if (bState && !isOverriding || !bState && isOverriding) {
        int iOldWidth, iOldHeight;
        m_Game->m_VguiSurface->GetScreenSize(iOldWidth, iOldHeight);
        m_Game->m_VguiSurface->ForceScreenSizeOverride(bState, m_RenderWidth, m_RenderHeight);
       /*int x = 0, y = 0, w = m_RenderWidth, h = m_RenderHeight;

        if (m_Game->m_ClientMode->GetViewport())
            m_Game->m_ClientMode->AdjustEngineViewport(x, y, w, h);*/

        if (bState) {
            /*IMatRenderContext* renderContext = m_Game->m_MaterialSystem->GetRenderContext();
            renderContext->Viewport(0, 0, m_RenderWidth, m_RenderHeight);
            renderContext->Release();*/
        }

        m_Game->m_VguiSurface->OnScreenSizeChanged(iOldWidth, iOldHeight);
    }
}

// =============================================================================================
// Threading model
// ---------------------------------------------------------------------------------------------
// Single-threaded (mat_queue_mode 0): PresentEx runs on the game's main thread; RenderThreadPresent
// falls back to MainThreadUpdate when dRenderView is not running (loading screens, plain menus).
// Multi-threaded (mat_queue_mode 2): the material system replays draw calls on a separate render thread,
// so PresentEx/RenderThreadPresent run there. Everything that touches engine/VGUI/console state, input
// or ClientCmd runs in MainThreadUpdate (dRenderView, main thread). The render thread only resolves,
// submits to SteamVR (with the exact HMD pose the frame was rendered with) and calls WaitGetPoses.
// The main thread renders with poses predicted to the frame's photon time (it runs one frame ahead).
// =============================================================================================

void VR::PushRenderedFrame()
{
    std::lock_guard<std::mutex> lock(m_FrameMutex);
    m_PendingFrames.push_back(m_CurrentFrame);
    // Every dRenderView produces exactly one Present; cap the queue tightly so a skipped Present cannot leave multi-frame pose lag.
    while (m_PendingFrames.size() > 2)
        m_PendingFrames.pop_front();
}

bool VR::HasPendingFrame()
{
    std::lock_guard<std::mutex> lock(m_FrameMutex);
    return !m_PendingFrames.empty();
}

void VR::ClearPendingFrames()
{
    std::lock_guard<std::mutex> lock(m_FrameMutex);
    m_PendingFrames.clear();
}

bool VR::PeekPendingFramePose(vr::HmdMatrix34_t& outPose)
{
    {
        std::lock_guard<std::mutex> lock(m_FrameMutex);
        if (!m_PendingFrames.empty() && m_PendingFrames.front().hmdPoseValid)
        {
            outPose = m_PendingFrames.front().hmdPose;
            return true;
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_RenderPosesMutex);
        if (m_RenderPoses[vr::k_unTrackedDeviceIndex_Hmd].bPoseIsValid)
        {
            outPose = m_RenderPoses[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking;
            return true;
        }
    }
    return false;
}

void VR::UpdatePredictedPoses()
{
    if (!m_System)
        return;

    // Display timing (queried at most twice per second)
    static float s_FrameDuration = 1.0f / 90.0f;
    static float s_VsyncToPhotons = 0.0f;
    static auto s_LastTimingQuery = std::chrono::steady_clock::time_point {};
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration<float>(now - s_LastTimingQuery).count() > 0.5f)
    {
        s_LastTimingQuery = now;
        float freq = m_System->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_DisplayFrequency_Float);
        if (freq > 20.0f && freq < 1000.0f)
            s_FrameDuration = 1.0f / freq;
        float v2p = m_System->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_SecondsFromVsyncToPhotons_Float);
        if (v2p >= 0.0f && v2p < 0.1f)
            s_VsyncToPhotons = v2p;
    }

    float secondsSinceVsync = 0.0f;
    uint64_t frameCounter = 0;
    float timeToNextVsync = s_FrameDuration;
    if (m_System->GetTimeSinceLastVsync(&secondsSinceVsync, &frameCounter) &&
        std::isfinite(secondsSinceVsync) && secondsSinceVsync >= 0.0f)
    {
        const float phase = std::fmod(secondsSinceVsync, s_FrameDuration);
        timeToNextVsync = s_FrameDuration - phase;
    }

    // The frame rendered now is replayed + submitted by the render thread about one frame later,
    // then shown at the following vsync: predict to (next vsync) + 1 frame + vsync-to-photons.
    float predicted = timeToNextVsync + s_FrameDuration + s_VsyncToPhotons;
    predicted = std::clamp(predicted, 0.0f, 0.05f);

    vr::ETrackingUniverseOrigin origin = vr::TrackingUniverseStanding;
    if (vr::VRCompositor())
        origin = vr::VRCompositor()->GetTrackingSpace();
    m_System->GetDeviceToAbsoluteTrackingPose(origin, predicted, m_Poses, vr::k_unMaxTrackedDeviceCount);
}

void VR::MainThreadUpdate()
{
    if (!m_IsInitialized || !m_Game->m_Initialized)
        return;

    m_MainThreadId = GetCurrentThreadId();
    auto tUpdateStart = VRProfiler::Clock::now();
    m_LastMainUpdateTickMs.store(GetTickCount64(), std::memory_order_relaxed);

    InstallBackgroundFocus();

    if (m_IsVREnabled && g_D3DVR9)
    {
        bool inGame = m_Game->m_EngineClient->IsInGame();

        // Reset cached state when disconnected from a map
        if (!inGame)
        {
            m_Game->m_CachedArmsModel = false;
            ClearPendingFrames();
        }
    }

    // 1. Poses: predicted on the main thread when a separate render thread exists, otherwise the
    //    WaitGetPoses result from the (same-thread) Present right before this frame.
    if (IsQueuedRendering())
    {
        UpdatePredictedPoses();
    }
    else
    {
        std::lock_guard<std::mutex> lock(m_RenderPosesMutex);
        memcpy(m_Poses, m_RenderPoses, sizeof(m_Poses));
    }

    // 2. Input actions, controller state, finger tracking
    UpdateActionsAndControllers();

    // 3. Tracking -> HMD / controller / viewmodel transforms used by this frame's rendering
    UpdateTracking();

    // Remember the HMD pose this frame is rendered with (submitted with Submit_TextureWithPose)
    m_CurrentFrame.hmdPose = m_Poses[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking;
    m_CurrentFrame.hmdPoseValid = m_Poses[vr::k_unTrackedDeviceIndex_Hmd].bPoseIsValid;

    // 4. One-time VR engine settings once the eye render targets exist
    static uint32_t s_LastAppliedAA = 999;
    if (m_CreatedVRTextures && s_LastAppliedAA != m_AntiAliasing)
    {
        s_LastAppliedAA = m_AntiAliasing;
        // Note: Do NOT send mat_antialias or mat_vsync here because changing MaterialSystem
        // video-config ConVars at runtime triggers a full D3D9 device reset & fullscreen switch mid-load!
        // - mat_queue_mode: 2 = multi-threaded material system (draw calls replayed on a render thread; the VR code is
        //   split into MainThreadUpdate / RenderThreadPresent for this). 0 = single-threaded (MultiThreadedRendering=false).
        //   Switched only now, after the VR eye textures exist, so their creation is never deferred to the render thread.
        // - infra_use_selection_outline / glow_outline_effect_enable: the RTX Remix mode writes
        //   infra_use_selection_outline 0 into config.cfg (FCVAR_ARCHIVE); re-enable the interaction outline in VR
        //   so the object picked by the VR hand (dClientFindUseEntity) is clearly highlighted.
        std::string cmd = m_MultiThreadedRendering ? "mat_queue_mode 2; " : "mat_queue_mode 0; ";
        cmd += "infra_use_selection_outline 1; glow_outline_effect_enable 1; developer 0; hideconsole; joystick 0; -jlook; mat_texture_list 0; unbind \"Z AXIS POS\"; unbind \"Z AXIS NEG\"; unbind \";\"; unbind \"\\\"; unbind \"F7\"; unbind \"F8\"; unbind \"p\"; unbind \"F11\"; ";
        if (m_EnableProfilerHotkey)
            cmd += "unbind \"F12\"; ";
        cmd += "mat_postprocess_enable 0; mat_motion_blur_enabled 0; mat_dof_enabled 0; mat_software_aa_strength 0; mat_software_aa_strength_vgui 0; mat_dynamic_tonemapping 0; mat_disable_bloom 1; mat_bloomscale 0; mat_picmip -1; mat_forceaniso 16; r_rootlod 0; r_lod 0; engine_no_focus_sleep 0; fps_max 0; r_threaded_particles 1; r_threaded_renderables 1; cl_threaded_bone_setup 1; snd_mix_async 1; r_queued_decals 0; r_queued_ropes 0";
        m_Game->ClientCmd_Unrestricted(cmd.c_str());
    }

    // Guard against mat_texture_list 1 under mat_queue_mode 2 (skill1.cfg/F7 re-binds can open
    // CTextureListPanel, whose KeyValues::MakeCopy races with the render thread in engine.dll).
    if (m_MultiThreadedRendering && m_Game && m_Game->m_BaseEngine)
    {
        uint8_t* pConVar = reinterpret_cast<uint8_t*>(m_Game->m_BaseEngine + 0x4F3350);
        void* pParent = *reinterpret_cast<void* const*>(pConVar + 0x1C);
        const char* pszName = *reinterpret_cast<const char* const*>(pConVar + 0x0C);
        if (pParent == pConVar && pszName && std::strcmp(pszName, "mat_texture_list") == 0)
        {
            int* pVal = reinterpret_cast<int*>(pConVar + 0x30);
            if (*pVal != 0)
            {
                *pVal = 0;
                m_Game->ClientCmd_Unrestricted("mat_texture_list 0");
            }
        }
    }

    // 5. Snapshot of engine/VGUI state for the render thread (overlay + desktop mirror decisions)
    bool conVisible = m_Game->m_EngineClient->Con_IsVisible();
    bool inActiveGameplay = m_Game->m_EngineClient->IsInGame() &&
                            !m_Game->m_EngineClient->IsLevelMainMenuBackground() &&
                            !m_Game->m_EngineClient->IsPaused();
    // Never allow accidental console popups during unpaused active VR gameplay
    if (inActiveGameplay && conVisible)
    {
        m_Game->ClientCmd_Unrestricted("hideconsole");
    }
    m_SnapConVisible = conVisible;
    m_SnapSubMenuVisible = Is2DSubMenuVisible();
    m_SnapInActiveGameplay = inActiveGameplay;
    {
        int windowWidth = 1920, windowHeight = 1080;
        IMatRenderContext* rndrContext = m_Game->m_MaterialSystem->GetRenderContext();
        rndrContext->GetWindowSize(windowWidth, windowHeight);
        rndrContext->Release();
        m_SnapWindowW = windowWidth > 0 ? windowWidth : 1920;
        m_SnapWindowH = windowHeight > 0 ? windowHeight : 1080;
    }

    // 6. VR controller input -> game / menus
    bool isInGamePausedMenu = m_Game->m_EngineClient->IsInGame() && m_Game->m_EngineClient->IsPaused();
    bool inMenuInput = m_Game->m_VguiSurface->IsCursorVisible() || m_In3DViewControlMenu || isInGamePausedMenu;
    if (inMenuInput != m_WasInMenuInput)
    {
        if (inMenuInput)
        {
            // Entering menu mode: clear gameplay tap/hold state and suppress menu confirm/back
            // if those buttons are still held from gameplay (e.g. Right B opening the phone menu).
            m_CameraBtnHoldMs = 0.0f;
            m_CameraReloadTriggered = false;
            m_FlashlightBtnHoldMs = 0.0f;
            m_FlashlightReloadTriggered = false;
            m_FlashlightTriggerHeld = false;
            m_FlashlightTriggerTurnedOnBeam = false;
            m_FlashlightBoostTriggered = false;
            m_FlashlightTriggerHoldMs = 0.0f;
            s_R3WasDown = false;
            s_R3HoldMs = 0.0f;
            s_R3SawL3 = false;
            s_R3SuppressUntilRelease = PressedDigitalAction(m_ActionActivateVR);
            s_LeftTpUpState.wasTouching = false;
            s_LeftTpUpState.hasStartY = false;
            s_LeftTpUpState.swipeTriggered = false;
            s_LeftTpUpState.zeroFrames = 0;
            m_SuppressMenuSelectUntilRelease = PressedDigitalAction(m_MenuSelect) ||
                                               PressedDigitalAction(m_ActionPrimaryAttack);
            m_SuppressMenuBackUntilRelease = PressedDigitalAction(m_MenuBack) ||
                                             PressedDigitalAction(m_Pause);
            m_SuppressFlashlightTriggerUntilRelease = PressedDigitalAction(m_ActionUse);
            if (isInGamePausedMenu)
                m_PauseOpenRequested = false;
        }
        else
        {
            // Leaving menu mode -> gameplay: cleanly release any held menu click state and
            // suppress shared buttons (Right A = MenuSelect/Camera, Right B = MenuBack/Pause,
            // plus Flashlight/AltFire) until released so menu clicks/back never bleed into gameplay.
            VRHands::ResetPhoneScreenTouch();
            if (m_WasMenuSelectDown)
            {
                m_PendingVGuiMouseRelease = true;
                if (m_Game && m_Game->m_VguiInput)
                    m_Game->m_VguiInput->InternalMouseReleased((ButtonCode_t)0x6B);
                if (m_SentOsMenuMouseDown)
                {
                    if (!IsGameForeground())
                    {
                        LPARAM pos = MAKELPARAM(m_MenuCursorX, m_MenuCursorY);
                        PostMessageA((HWND)GetGameHwndRaw(), WM_LBUTTONUP, 0, pos);
                    }
                    else
                    {
                        INPUT input {};
                        input.type = INPUT_MOUSE;
                        input.mi.dwFlags = MOUSEEVENTF_LEFTUP;
                        SendInput(1, &input, sizeof(INPUT));
                    }
                    m_SentOsMenuMouseDown = false;
                }
                m_WasMenuSelectDown = false;
            }
            m_CameraBtnHoldMs = 0.0f;
            m_CameraReloadTriggered = false;
            m_FlashlightBtnHoldMs = 0.0f;
            m_FlashlightReloadTriggered = false;
            m_FlashlightTriggerHeld = false;
            m_FlashlightTriggerTurnedOnBeam = false;
            m_FlashlightBoostTriggered = false;
            m_FlashlightTriggerHoldMs = 0.0f;
            s_R3WasDown = false;
            s_R3HoldMs = 0.0f;
            s_R3SawL3 = false;
            s_R3SuppressUntilRelease = PressedDigitalAction(m_ActionActivateVR);
            s_LeftTpUpState.wasTouching = false;
            s_LeftTpUpState.hasStartY = false;
            s_LeftTpUpState.swipeTriggered = false;
            s_LeftTpUpState.zeroFrames = 0;
            m_SuppressCameraUntilRelease = PressedDigitalAction(m_ActionNextItem) ||
                                           PressedDigitalAction(m_ActionPrevItem) ||
                                           PressedDigitalAction(m_Spray);
            m_SuppressFlashlightUntilRelease = PressedDigitalAction(m_ActionFlashlight) ||
                                               PressedDigitalAction(m_Scoreboard);
            m_SuppressAltFireUntilRelease = PressedDigitalAction(m_ActionSecondaryAttack) ||
                                            PressedDigitalAction(m_ActionJump);
            m_SuppressPauseUntilRelease = PressedDigitalAction(m_Pause);
            m_SuppressFlashlightTriggerUntilRelease = PressedDigitalAction(m_ActionUse);
        }
        m_WasInMenuInput = inMenuInput;
    }

    if (inMenuInput) {
        ProcessMenuInput();
    } else {
        ProcessInput();
    }

    // One-shot auto-exec helper (e.g. launcher\start_infra_turbine_hall.bat drops VR\auto_exec_once.txt
    // so the player automatically teleports into the target room once the map finishes loading in VR).
    static bool s_CheckedAutoExec = false;
    static unsigned long long s_AutoExecReadyTick = 0;
    if (!s_CheckedAutoExec && m_Game && m_Game->m_EngineClient && m_Game->m_EngineClient->IsInGame())
    {
        if (s_AutoExecReadyTick == 0)
        {
            s_AutoExecReadyTick = GetTickCount64();
        }
        else if (GetTickCount64() - s_AutoExecReadyTick >= 1000ULL)
        {
            s_CheckedAutoExec = true;
            const std::string autoPath = ResolveGameRelativePath("VR\\auto_exec_once.txt");
            std::ifstream af(autoPath);
            if (af)
            {
                std::string line;
                std::getline(af, line);
                af.close();
                DeleteFileA(autoPath.c_str());
                if (!line.empty())
                    m_Game->ClientCmd_Unrestricted(line.c_str());
            }
        }
    }

    m_Profiler.Add(VRProfiler::VRUpdateTotal, VRProfiler::Ms(tUpdateStart, VRProfiler::Clock::now()));
    if (m_EnableProfilerHotkey)
    {
        UpdateProfiler();
    }
}

void VR::RenderThreadPresent()
{
    if (!m_IsInitialized || !m_Game->m_Initialized)
        return;

    m_RenderThreadId = GetCurrentThreadId();

    // Single-threaded fallback: keep input/tracking alive while dRenderView is not running
    // (startup before CreateVRTextures, loading screens, menus without a 3D view).
    uint32_t mainTid = m_MainThreadId.load();
    if (!IsQueuedRendering() && (mainTid == 0 || mainTid == GetCurrentThreadId()) &&
        (GetTickCount64() - m_LastMainUpdateTickMs.load(std::memory_order_relaxed)) > 4ULL)
    {
        MainThreadUpdate();
    }

    VRFrameData frame;
    bool renderedNewFrame = false;
    {
        std::lock_guard<std::mutex> lock(m_FrameMutex);
        if (!m_PendingFrames.empty())
        {
            frame = m_PendingFrames.front();
            m_PendingFrames.pop_front();
            renderedNewFrame = true;
        }
    }

    struct VRSubmitGuard {
        IDirect3DVR9* m_pVR;
        explicit VRSubmitGuard(IDirect3DVR9* pVR) : m_pVR(pVR) {
            if (m_pVR) m_pVR->BeginVRSubmit();
        }
        ~VRSubmitGuard() {
            if (m_pVR) m_pVR->EndVRSubmit();
        }
        VRSubmitGuard(const VRSubmitGuard&) = delete;
        VRSubmitGuard& operator=(const VRSubmitGuard&) = delete;
    };

    {
        auto tSubmit0 = VRProfiler::Clock::now();
        {
            VRSubmitGuard submitGuard(g_D3DVR9);
            SubmitVRTextures(renderedNewFrame, frame);
        }
        m_Profiler.Add(VRProfiler::Submit, VRProfiler::Ms(tSubmit0, VRProfiler::Clock::now()));

        // Frame pacing: WaitGetPoses blocks this thread until SteamVR wants the next frame.
        vr::IVRCompositor* pCompositor = vr::VRCompositor();
        if (pCompositor)
        {
            vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
            auto tWait0 = VRProfiler::Clock::now();
            pCompositor->WaitGetPoses(poses, vr::k_unMaxTrackedDeviceCount, NULL, 0);
            m_Profiler.Add(VRProfiler::WaitGetPoses, VRProfiler::Ms(tWait0, VRProfiler::Clock::now()));
            std::lock_guard<std::mutex> lock(m_RenderPosesMutex);
            memcpy(m_RenderPoses, poses, sizeof(m_RenderPoses));
        }
    }
}

void VR::UpdateProfiler()
{
    // F12 on the desktop keyboard (unbound in-game by the VR startup commands) starts a 10 s CPU capture.
    static bool s_WasF12Down = false;
    bool f12Down = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
    bool f12Pressed = f12Down && !s_WasF12Down;
    s_WasF12Down = f12Down;

    if (f12Pressed && !m_Profiler.Active())
    {
        m_Game->ClientCmd_Unrestricted("con_logfile vr_vprof.log; echo [INFRA VR] CPU profile started (10 s); vprof_reset; vprof_on");
        m_Profiler.Start(10.0);
    }
    else if (m_Profiler.ShouldFinish())
    {
        std::string path = m_Profiler.Finish((int)m_RenderWidth, (int)m_RenderHeight, (int)m_AntiAliasing, IsQueuedRendering());
        // vprof reports are printed via DevMsg: enable developer output only while the report is written.
        std::string cmd = "developer 1; vprof_generate_report; vprof_generate_report_hierarchy; vprof_generate_report_budget; vprof_off; developer 0; echo [INFRA VR] CPU profile written: ";
        cmd += path.empty() ? std::string("(failed)") : path;
        cmd += "; con_logfile \"\"";
        m_Game->ClientCmd_Unrestricted(cmd.c_str());
    }
}

bool VR::Is2DSubMenuVisible() const
{
    if (!m_Game || !m_Game->m_BaseClient || !m_Game->m_VguiSurface || !m_Game->m_VguiSurface->IsCursorVisible())
        return false;
    void* pMainMenu = *(void**)(m_Game->m_BaseClient + 0x6D987C);
    if (!pMainMenu)
        return false;
    void* pSubMenu = *(void**)((uintptr_t)pMainMenu + 0x19C);
    if (!pSubMenu)
        return false;
    auto fnIsVisible = *(bool(__thiscall**)(void*))(*(uintptr_t*)pSubMenu + 0x18);
    return fnIsVisible ? fnIsVisible(pSubMenu) : false;
}

void VR::CreateVRTextures()
{
    int windowWidth, windowHeight;

    IMatRenderContext* rndrContext = m_Game->m_MaterialSystem->GetRenderContext();
    rndrContext->GetWindowSize(windowWidth, windowHeight);
    rndrContext->Release();

    std::cout << "RenderTexture - Width: " << m_RenderWidth << ", Height: " << m_RenderHeight << "\n";

    m_CreatedVRTextures = true;
    m_AntiAliasingLocked = m_AntiAliasing;
    m_HasLockedAntiAliasing = true;

    m_Game->m_MaterialSystem->isGameRunning = false;
    m_Game->m_MaterialSystem->BeginRenderTargetAllocation();
    m_Game->m_MaterialSystem->isGameRunning = true;

    m_CreatingTextureID = Texture_LeftEye;
    m_LeftEyeTexture = m_Game->m_MaterialSystem->CreateNamedRenderTargetTextureEx("leftEye0", m_RenderWidth, m_RenderHeight, RT_SIZE_NO_CHANGE, m_Game->m_MaterialSystem->GetBackBufferFormat(), MATERIAL_RT_DEPTH_SEPARATE, TEXTUREFLAGS_NOMIP);
    
    m_CreatingTextureID = Texture_RightEye;
    m_RightEyeTexture = m_Game->m_MaterialSystem->CreateNamedRenderTargetTextureEx("rightEye0", m_RenderWidth, m_RenderHeight, RT_SIZE_NO_CHANGE, m_Game->m_MaterialSystem->GetBackBufferFormat(), MATERIAL_RT_DEPTH_SEPARATE, TEXTUREFLAGS_NOMIP);

    m_CreatingTextureID = Texture_HUD;
    m_HUDTexture = m_Game->m_MaterialSystem->CreateNamedRenderTargetTextureEx("vrHUD", m_RenderWidth, m_RenderHeight, RT_SIZE_NO_CHANGE, m_Game->m_MaterialSystem->GetBackBufferFormat(), MATERIAL_RT_DEPTH_SHARED, TEXTUREFLAGS_NOMIP);
    
    m_CreatingTextureID = Texture_Blank;
    // Match full VR eye resolution so SteamVR composites overlays (360° intro, logos, loading menu)
    // at native headset sharpness instead of downsampling them into a 512x512 eye buffer.
    m_BlankTexture = m_Game->m_MaterialSystem->CreateNamedRenderTargetTextureEx("blankTexture", m_RenderWidth, m_RenderHeight, RT_SIZE_NO_CHANGE, m_Game->m_MaterialSystem->GetBackBufferFormat(), MATERIAL_RT_DEPTH_SHARED, TEXTUREFLAGS_NOMIP);
    
    m_CreatingTextureID = Texture_None;

    m_Game->m_MaterialSystem->EndRenderTargetAllocation();

    if (m_LeftEyeTexture)
        Hooks::InstallTextureDownloadHook(m_LeftEyeTexture);

    if (g_D3DVR9)
        g_D3DVR9->WaitDeviceIdle();
}

static bool s_MenuRepositionedWithValidPose = false;
static Vector s_LastMenuHmdPos = { 0.0f, 0.0f, 0.0f };

void VR::SubmitVRTextures(bool renderedNewFrame, const VRFrameData& frame)
{
    // Runs on the render thread with mat_queue_mode 2: no engine / VGUI / material system calls here,
    // only the main-thread snapshot (m_Snap*) and render-thread owned data.
    if (!g_D3DVR9)
        return;

    m_RenderedNewFrame = renderedNewFrame;

    bool hasValidEyeTextures =
        m_VKLeftEye.m_VRTexture.handle && m_VKLeftEye.m_VRTexture.eType == vr::TextureType_Vulkan && m_VKLeftEye.m_VulkanData.m_nImage != 0 &&
        m_VKRightEye.m_VRTexture.handle && m_VKRightEye.m_VRTexture.eType == vr::TextureType_Vulkan && m_VKRightEye.m_VulkanData.m_nImage != 0;

    static bool s_HasRenderedFirstVRFrame = false;
    if (renderedNewFrame && hasValidEyeTextures)
        s_HasRenderedFirstVRFrame = true;
    else if (!hasValidEyeTextures)
        s_HasRenderedFirstVRFrame = false;

    const bool inActiveGameplay = m_SnapInActiveGameplay.load();
    const bool conVisible = m_SnapConVisible.load();
    const unsigned long long lastRvMs = m_LastRenderViewTickMs.load();
    const bool noRecent3DView = !renderedNewFrame && (lastRvMs == 0 || (GetTickCount64() - lastRvMs) > 350ULL);
    bool showMenuOverlay = (!s_HasRenderedFirstVRFrame && !renderedNewFrame) ||
                           noRecent3DView ||
                           m_SnapSubMenuVisible.load() ||
                           (!inActiveGameplay && conVisible);
    // Keep the plain 2D menu overlay hidden while the 3D startup splash is actively presenting
    if (m_StartupSplashActive && s_HasRenderedFirstVRFrame && !m_SnapSubMenuVisible.load() && !conVisible)
        showMenuOverlay = false;

    vr::IVROverlay* pOverlay = vr::VROverlay();
    vr::IVRCompositor* pCompositor = vr::VRCompositor();

    if (showMenuOverlay)
    {
        g_D3DVR9->GetBackBufferData(&m_VKBackBuffer);

        vr::TrackedDevicePose_t hmdPose;
        {
            std::lock_guard<std::mutex> lock(m_RenderPosesMutex);
            hmdPose = m_RenderPoses[vr::k_unTrackedDeviceIndex_Hmd];
        }
        bool needReposition = (pOverlay && m_MainMenuHandle != vr::k_ulOverlayHandleInvalid)
            ? (!pOverlay->IsOverlayVisible(m_MainMenuHandle) || (!s_MenuRepositionedWithValidPose && hmdPose.bPoseIsValid))
            : false;

        if (hmdPose.bPoseIsValid)
        {
            Vector curPos = {
                hmdPose.mDeviceToAbsoluteTracking.m[0][3],
                hmdPose.mDeviceToAbsoluteTracking.m[1][3],
                hmdPose.mDeviceToAbsoluteTracking.m[2][3]
            };
            Vector delta = curPos - s_LastMenuHmdPos;
            if (VectorLength(delta) > 0.45f)
                needReposition = true;
        }

        if (needReposition)
            RepositionOverlays(hmdPose);

        int windowWidth = m_SnapWindowW.load();
        int windowHeight = m_SnapWindowH.load();

        vr::VRTextureBounds_t bounds{ 0.0f, 0.0f, 1.0f, 1.0f };
        if (m_RenderWidth > 0 && m_RenderHeight > 0)
        {
            bounds.uMax = std::clamp((float)windowWidth / (float)m_RenderWidth, 0.1f, 1.0f);
            bounds.vMax = std::clamp((float)windowHeight / (float)m_RenderHeight, 0.1f, 1.0f);
        }

        if (m_VKBackBuffer.m_VRTexture.handle && m_VKBackBuffer.m_VRTexture.eType == vr::TextureType_Vulkan && m_VKBackBuffer.m_VulkanData.m_nImage != 0)
        {
            if (pOverlay && m_MainMenuHandle != vr::k_ulOverlayHandleInvalid)
            {
                pOverlay->SetOverlayTexelAspect(m_MainMenuHandle, 1.0f);
                pOverlay->SetOverlayTextureBounds(m_MainMenuHandle, &bounds);
                pOverlay->SetOverlayTexture(m_MainMenuHandle, &m_VKBackBuffer.m_VRTexture);
                pOverlay->ShowOverlay(m_MainMenuHandle);
            }
        }
    }
    else
    {
        if (pOverlay && m_MainMenuHandle != vr::k_ulOverlayHandleInvalid && pOverlay->IsOverlayVisible(m_MainMenuHandle))
            pOverlay->HideOverlay(m_MainMenuHandle);
        s_MenuRepositionedWithValidPose = false;
    }

    if (pCompositor)
    {
        if (!noRecent3DView && (renderedNewFrame || s_HasRenderedFirstVRFrame) && hasValidEyeTextures)
        {
            // With a separate render thread the frame was rendered with a predicted pose on the main thread:
            // hand that exact pose to the compositor so reprojection matches what was rendered.
            if (renderedNewFrame && frame.hmdPoseValid && IsQueuedRendering())
            {
                vr::VRTextureWithPose_t leftTex {};
                vr::VRTextureWithPose_t rightTex {};
                static_cast<vr::Texture_t&>(leftTex) = m_VKLeftEye.m_VRTexture;
                static_cast<vr::Texture_t&>(rightTex) = m_VKRightEye.m_VRTexture;
                leftTex.mDeviceToAbsoluteTracking = frame.hmdPose;
                rightTex.mDeviceToAbsoluteTracking = frame.hmdPose;
                pCompositor->Submit(vr::Eye_Left, &leftTex, &(m_TextureBounds)[0], vr::Submit_TextureWithPose);
                pCompositor->Submit(vr::Eye_Right, &rightTex, &(m_TextureBounds)[1], vr::Submit_TextureWithPose);
            }
            else
            {
                pCompositor->Submit(vr::Eye_Left, &m_VKLeftEye.m_VRTexture, &(m_TextureBounds)[0], vr::Submit_Default);
                pCompositor->Submit(vr::Eye_Right, &m_VKRightEye.m_VRTexture, &(m_TextureBounds)[1], vr::Submit_Default);
            }
            m_RenderedNewFrame = false;
        }
        else if (m_D9BlankSurface && m_VKBlankTexture.m_VRTexture.handle && m_VKBlankTexture.m_VRTexture.eType == vr::TextureType_Vulkan && m_VKBlankTexture.m_VulkanData.m_nImage != 0)
        {
            pCompositor->Submit(vr::Eye_Left, &m_VKBlankTexture.m_VRTexture, &(m_TextureBounds)[0], vr::Submit_Default);
            pCompositor->Submit(vr::Eye_Right, &m_VKBlankTexture.m_VRTexture, &(m_TextureBounds)[1], vr::Submit_Default);
        }
        else
        {
            g_D3DVR9->GetBackBufferData(&m_VKBackBuffer);
            if (m_VKBackBuffer.m_VRTexture.handle && m_VKBackBuffer.m_VRTexture.eType == vr::TextureType_Vulkan && m_VKBackBuffer.m_VulkanData.m_nImage != 0)
            {
                pCompositor->Submit(vr::Eye_Left, &m_VKBackBuffer.m_VRTexture, NULL, vr::Submit_Default);
                pCompositor->Submit(vr::Eye_Right, &m_VKBackBuffer.m_VRTexture, NULL, vr::Submit_Default);
            }
        }
    }
}

void VR::GetPoseData(vr::TrackedDevicePose_t &poseRaw, TrackedDevicePoseData &poseOut)
{
    if (poseRaw.bPoseIsValid) 
    {
        vr::HmdMatrix34_t mat = poseRaw.mDeviceToAbsoluteTracking;
        Vector pos;
        Vector vel;
        QAngle ang;
        QAngle angvel;
        pos.x = -mat.m[2][3];
        pos.y = -mat.m[0][3];
        pos.z = mat.m[1][3];
        ang.x = asin(std::clamp(mat.m[1][2], -1.0f, 1.0f)) * (180.0 / 3.141592654);
        ang.y = atan2f(mat.m[0][2], mat.m[2][2]) * (180.0 / 3.141592654);
        ang.z = atan2f(-mat.m[1][0], mat.m[1][1]) * (180.0 / 3.141592654);
        vel.x = -poseRaw.vVelocity.v[2];
        vel.y = -poseRaw.vVelocity.v[0];
        vel.z = poseRaw.vVelocity.v[1];
        angvel.x = -poseRaw.vAngularVelocity.v[2] * (180.0 / 3.141592654);
        angvel.y = -poseRaw.vAngularVelocity.v[0] * (180.0 / 3.141592654);
        angvel.z = poseRaw.vAngularVelocity.v[1] * (180.0 / 3.141592654);

        poseOut.TrackedDevicePos = pos;
        poseOut.TrackedDeviceVel = vel;
        poseOut.TrackedDeviceAng = ang;
        poseOut.TrackedDeviceAngVel = angvel;
    }
}

void VR::RepositionOverlays(const vr::TrackedDevicePose_t& hmdPose)
{
    vr::HmdMatrix34_t hmdMat = hmdPose.mDeviceToAbsoluteTracking;
    Vector hmdPosition = hmdPose.bPoseIsValid
        ? Vector{ hmdMat.m[0][3], hmdMat.m[1][3], hmdMat.m[2][3] }
        : Vector{ 0.0f, 1.5f, 0.0f };
    Vector hmdForward = hmdPose.bPoseIsValid
        ? Vector{ -hmdMat.m[0][2], 0.0f, -hmdMat.m[2][2] }
        : Vector{ 0.0f, 0.0f, -1.0f };

    if (hmdPose.bPoseIsValid)
    {
        s_MenuRepositionedWithValidPose = true;
        s_LastMenuHmdPos = hmdPosition;
    }

    vr::HmdMatrix34_t menuTransform = 
    {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 1.0f,
        0.0f, 0.0f, 1.0f, 1.0f
    };

    vr::IVRCompositor* pCompositor = vr::VRCompositor();
    if (!pCompositor)
        return;
    vr::ETrackingUniverseOrigin trackingOrigin = pCompositor->GetTrackingSpace();

    hmdForward[1] = 0.0f;
    if (VectorLength(hmdForward) < 0.001f)
        hmdForward = { 0.0f, 0.0f, -1.0f };
    else
        VectorNormalize(hmdForward);

    // Place flat 16:9 menu overlay 1.4m directly in front of the player at eye level
    Vector menuDistance = hmdForward * 1.4f;
    Vector menuNewPos = menuDistance + hmdPosition;

    menuTransform.m[0][3] = menuNewPos.x;
    menuTransform.m[1][3] = menuNewPos.y - 0.15f;
    menuTransform.m[2][3] = menuNewPos.z;

    float hmdRotationRadians = hmdPose.bPoseIsValid
        ? atan2f(hmdMat.m[0][2], hmdMat.m[2][2])
        : 0.0f;

    menuTransform.m[0][0] = cosf(hmdRotationRadians);
    menuTransform.m[0][2] = sinf(hmdRotationRadians);
    menuTransform.m[1][1] = 1.0f;
    menuTransform.m[2][0] = -sinf(hmdRotationRadians);
    menuTransform.m[2][2] = cosf(hmdRotationRadians);

    vr::IVROverlay* pOverlay = vr::VROverlay();
    if (pOverlay && m_MainMenuHandle != vr::k_ulOverlayHandleInvalid)
    {
        pOverlay->SetOverlayTransformAbsolute(m_MainMenuHandle, trackingOrigin, &menuTransform);
        pOverlay->SetOverlayWidthInMeters(m_MainMenuHandle, 1.6f);
    }
}

void VR::GetPoses() 
{
    vr::TrackedDevicePose_t hmdPose = m_Poses[vr::k_unTrackedDeviceIndex_Hmd];
    GetPoseData(hmdPose, m_HmdPose);

    // Keep m_LeftControllerPose / m_RightControllerPose mapped 1:1 to the physical Left / Right controllers.
    // LeftHanded tool assignment is handled explicitly in UpdateTracking() and Hooks so poses are never double-swapped.
    if (m_LeftControllerIndex < vr::k_unMaxTrackedDeviceCount)
        GetPoseData(m_Poses[m_LeftControllerIndex], m_LeftControllerPose);

    if (m_RightControllerIndex < vr::k_unMaxTrackedDeviceCount)
        GetPoseData(m_Poses[m_RightControllerIndex], m_RightControllerPose);
}

void VR::UpdateActionsAndControllers()
{
    // WaitGetPoses now runs in RenderThreadPresent; this part runs on the main thread (MainThreadUpdate).
    uint32_t numSets = (m_BaseActionSet != vr::k_ulInvalidActionSetHandle) ? 2 : 1;
    if (m_Input)
        m_Input->UpdateActionState(m_ActiveActionSets, sizeof(vr::VRActiveActionSet_t), numSets);

    if (!m_System)
        return;

    // Cache controller device indices and controller states once per frame to avoid 6+ redundant OpenVR IPC calls per frame
    m_LeftControllerIndex = m_System->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_LeftHand);
    m_RightControllerIndex = m_System->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand);
    m_HasLeftControllerState = (m_LeftControllerIndex != vr::k_unTrackedDeviceIndexInvalid) &&
                               m_System->GetControllerState(m_LeftControllerIndex, &m_LeftControllerState, sizeof(m_LeftControllerState));
    m_HasRightControllerState = (m_RightControllerIndex != vr::k_unTrackedDeviceIndexInvalid) &&
                                m_System->GetControllerState(m_RightControllerIndex, &m_RightControllerState, sizeof(m_RightControllerState));

    bool partialLeft = false;
    bool partialRight = false;
    VRHands::UpdateFingerInput(
        m_Input, m_ActionSkeletonLeft,
        PressedDigitalAction(m_ActionUse), PressedDigitalAction(m_ActionCrouch),
        m_HasLeftControllerState, m_LeftControllerState,
        m_LeftFingerCurl, m_LeftFingerSplay, &partialLeft);
    VRHands::UpdateFingerInput(
        m_Input, m_ActionSkeletonRight,
        PressedDigitalAction(m_ActionPrimaryAttack), PressedDigitalAction(m_ActionReload),
        m_HasRightControllerState, m_RightControllerState,
        m_RightFingerCurl, m_RightFingerSplay, &partialRight);
    m_SkeletalTrackingStateLeft.store(partialLeft ? 1 : 0, std::memory_order_release);
    m_SkeletalTrackingStateRight.store(partialRight ? 1 : 0, std::memory_order_release);
}

void VR::GetViewParameters() 
{
    vr::HmdMatrix34_t eyeToHeadLeft = m_System->GetEyeToHeadTransform(vr::Eye_Left);
    vr::HmdMatrix34_t eyeToHeadRight = m_System->GetEyeToHeadTransform(vr::Eye_Right);
    m_EyeToHeadTransformPosLeft.x = eyeToHeadLeft.m[0][3];
    m_EyeToHeadTransformPosLeft.y = eyeToHeadLeft.m[1][3];
    m_EyeToHeadTransformPosLeft.z = eyeToHeadLeft.m[2][3];

    m_EyeToHeadTransformPosRight.x = eyeToHeadRight.m[0][3];
    m_EyeToHeadTransformPosRight.y = eyeToHeadRight.m[1][3];
    m_EyeToHeadTransformPosRight.z = eyeToHeadRight.m[2][3];
}

bool VR::PressedDigitalAction(vr::VRActionHandle_t &actionHandle, bool checkIfActionChanged)
{
    vr::InputDigitalActionData_t digitalActionData;
    vr::EVRInputError result = m_Input->GetDigitalActionData(actionHandle, &digitalActionData, sizeof(digitalActionData), vr::k_ulInvalidInputValueHandle);
    
    if (result == vr::VRInputError_None)
    {
        if (checkIfActionChanged)
            return digitalActionData.bState && digitalActionData.bChanged;
        else
            return digitalActionData.bState;
    }

    return false;
}

bool VR::GetAnalogActionData(vr::VRActionHandle_t &actionHandle, vr::InputAnalogActionData_t &analogDataOut)
{
    vr::EVRInputError result = m_Input->GetAnalogActionData(actionHandle, &analogDataOut, sizeof(analogDataOut), vr::k_ulInvalidInputValueHandle);

    if (result == vr::VRInputError_None)
        return true;

    return false;
}

void VR::ProcessMenuInput()
{
    auto sendKeyTap = [this](WORD vk) {
        if (!IsGameForeground())
        {
            // Game window not focused: deliver directly to INFRA instead of the foreground app
            HWND h = (HWND)GetGameHwndRaw();
            UINT sc = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
            PostMessageA(h, WM_KEYDOWN, vk, 1 | (sc << 16));
            PostMessageA(h, WM_KEYUP, vk, 1 | (sc << 16) | (1u << 30) | (1u << 31));
            return;
        }
        INPUT input {};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = vk;
        SendInput(1, &input, sizeof(INPUT));
        input.ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(1, &input, sizeof(INPUT));
    };

    auto sendMouseClick = [this](bool down) {
        if (!IsGameForeground())
        {
            LPARAM pos = MAKELPARAM(m_MenuCursorX, m_MenuCursorY);
            PostMessageA((HWND)GetGameHwndRaw(), down ? WM_LBUTTONDOWN : WM_LBUTTONUP, down ? MK_LBUTTON : 0, pos);
            return;
        }
        INPUT input {};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        SendInput(1, &input, sizeof(INPUT));
    };

    static auto s_PrevMenuTime = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    float dtSec = std::clamp(std::chrono::duration<float>(now - s_PrevMenuTime).count(), 0.001f, 0.1f);
    s_PrevMenuTime = now;

    // Choose the thumbstick with the stronger deflection (prevents summing stick drift from both sticks)
    vr::InputAnalogActionData_t walkData {}, turnData {};
    float stickX = 0.0f, stickY = 0.0f;
    bool gotWalk = GetAnalogActionData(m_ActionWalk, walkData);
    bool gotTurn = GetAnalogActionData(m_ActionTurn, turnData);
    float walkMag = gotWalk ? std::hypot(walkData.x, walkData.y) : 0.0f;
    float turnMag = gotTurn ? std::hypot(turnData.x, turnData.y) : 0.0f;
    if (walkMag >= turnMag && gotWalk)
    {
        stickX = std::clamp(walkData.x, -1.0f, 1.0f);
        stickY = std::clamp(walkData.y, -1.0f, 1.0f);
    }
    else if (gotTurn)
    {
        stickX = std::clamp(turnData.x, -1.0f, 1.0f);
        stickY = std::clamp(turnData.y, -1.0f, 1.0f);
    }

    static auto s_LastStickNavTime = std::chrono::steady_clock::now();
    static bool s_StickNeutral = true;
    float msSinceLastNav = std::chrono::duration<float, std::milli>(now - s_LastStickNavTime).count();

    bool stickUp = false, stickDown = false, stickLeft = false, stickRight = false;
    if (std::fabs(stickX) < 0.30f && std::fabs(stickY) < 0.30f)
    {
        s_StickNeutral = true;
    }
    else if (s_StickNeutral || msSinceLastNav > 260.0f)
    {
        if (stickY > 0.65f)       { stickUp = true;    s_StickNeutral = false; s_LastStickNavTime = now; }
        else if (stickY < -0.65f) { stickDown = true;  s_StickNeutral = false; s_LastStickNavTime = now; }
        else if (stickX < -0.65f) { stickLeft = true;  s_StickNeutral = false; s_LastStickNavTime = now; }
        else if (stickX > 0.65f)  { stickRight = true; s_StickNeutral = false; s_LastStickNavTime = now; }
    }

    bool navUp    = stickUp;
    bool navDown  = stickDown;
    bool navLeft  = stickLeft;
    bool navRight = stickRight;

    bool rawSelectDown = PressedDigitalAction(m_MenuSelect) ||
                         PressedDigitalAction(m_ActionPrimaryAttack);
    if (m_SuppressMenuSelectUntilRelease && !rawSelectDown)
        m_SuppressMenuSelectUntilRelease = false;

    bool selectDown = !m_SuppressMenuSelectUntilRelease && rawSelectDown;

    bool navSelect = !m_SuppressMenuSelectUntilRelease &&
                     (PressedDigitalAction(m_MenuSelect, true) ||
                      PressedDigitalAction(m_ActionPrimaryAttack, true));

    bool rawBackDown = PressedDigitalAction(m_MenuBack) ||
                       PressedDigitalAction(m_Pause);
    if (m_SuppressMenuBackUntilRelease && !rawBackDown)
        m_SuppressMenuBackUntilRelease = false;

    bool navBack   = !m_SuppressMenuBackUntilRelease &&
                     (PressedDigitalAction(m_MenuBack, true) ||
                      PressedDigitalAction(m_Pause, true));

    // When the in-game 3D cell phone (infra_phone) is open in the player's Right Hand
    // (and no 2D Options/LoadGame sub-menu is open), CINFRAPhoneScreen is a C_VGuiScreen (450x750)
    // driven by 2D cursor coordinates in dVGuiScreenGetCursorPos + MOUSE_LEFT (+0xAD4/+0xADC).
    // Move the menu cursor strictly via the Thumbsticks (never from finger tracking, controller rotation, or SteamVR overlay scroll).
    bool isInGamePhoneMenu = m_Game->m_EngineClient->IsInGame() && !m_In3DViewControlMenu && !Is2DSubMenuVisible();

    int screenW = 1920, screenH = 1080;
    m_Game->m_VguiSurface->GetScreenSize(screenW, screenH);
    if (screenW <= 0) screenW = 1920;
    if (screenH <= 0) screenH = 1080;

    static float s_CursorFloatX = -1.0f;
    static float s_CursorFloatY = -1.0f;
    if (!m_MenuCursorInitialized || s_CursorFloatX < 0.0f || s_CursorFloatY < 0.0f)
    {
        if (m_MenuCursorX <= 0 || m_MenuCursorY <= 0)
        {
            m_MenuCursorX = screenW / 2;
            m_MenuCursorY = isInGamePhoneMenu ? (int)(screenH * 0.28f) : (screenH / 2);
        }
        s_CursorFloatX = (float)m_MenuCursorX;
        s_CursorFloatY = (float)m_MenuCursorY;
        m_MenuCursorInitialized = true;
    }

    bool cursorMoved = false;

    // 1. Move menu cursor smoothly ONLY with Left or Right Thumbstick (finger tracking & hand rotation never move/scroll menus)
    const float cursorSpeed = isInGamePhoneMenu ? 750.0f : 900.0f;
    if (std::fabs(stickX) > 0.18f)
    {
        s_CursorFloatX = std::clamp(s_CursorFloatX + stickX * cursorSpeed * dtSec, 0.0f, (float)(screenW - 1));
        cursorMoved = true;
    }
    if (std::fabs(stickY) > 0.18f)
    {
        s_CursorFloatY = std::clamp(s_CursorFloatY - stickY * cursorSpeed * dtSec, 0.0f, (float)(screenH - 1));
        cursorMoved = true;
    }

    // 2. Track physical desktop mouse deltas (only outside handheld phone menu)
    static POINT s_LastOsPt = { -1, -1 };
    POINT osPt {};
    if (!isInGamePhoneMenu && IsGameForeground() && GetCursorPos(&osPt))
    {
        if (s_LastOsPt.x != -1 && (osPt.x != s_LastOsPt.x || osPt.y != s_LastOsPt.y))
        {
            int dOsX = osPt.x - s_LastOsPt.x;
            int dOsY = osPt.y - s_LastOsPt.y;
            s_CursorFloatX = std::clamp(s_CursorFloatX + dOsX * 2.0f, 0.0f, (float)(screenW - 1));
            s_CursorFloatY = std::clamp(s_CursorFloatY + dOsY * 2.0f, 0.0f, (float)(screenH - 1));
            cursorMoved = true;
        }
        s_LastOsPt = osPt;
    }

    VRHands::PhoneScreenTouchResult phoneTouch{};
    if (isInGamePhoneMenu && m_PhoneEquipped)
    {
        phoneTouch = VRHands::PollPhoneScreenTouch(this, m_SetupOrigin);
        if (phoneTouch.hovering &&
            ((std::fabs(stickX) <= 0.18f && std::fabs(stickY) <= 0.18f) ||
             phoneTouch.pressedEdge || phoneTouch.releasedEdge))
        {
            s_CursorFloatX = phoneTouch.u * (float)(screenW - 1);
            s_CursorFloatY = phoneTouch.v * (float)(screenH - 1);
            cursorMoved = true;
        }
    }

    if (cursorMoved)
    {
        m_MenuCursorX = std::clamp((int)s_CursorFloatX, 0, screenW - 1);
        m_MenuCursorY = std::clamp((int)s_CursorFloatY, 0, screenH - 1);
        m_Game->m_VguiInput->SetCursorPos(m_MenuCursorX, m_MenuCursorY);
    }

    // Send real Left Mouse Clicks on Right Trigger / Right A button press & release so 3D VGuiScreen (laptop/binder/projector/phone) and 2D GameUI buttons click reliably!
    if (selectDown != m_WasMenuSelectDown)
    {
        m_Game->m_VguiInput->SetCursorPos(m_MenuCursorX, m_MenuCursorY);
        if (selectDown)
        {
            m_PendingVGuiMousePress = true;
            m_Game->m_VguiInput->InternalMousePressed((ButtonCode_t)0x6B);
            if (!isInGamePhoneMenu)
            {
                sendMouseClick(true);
                m_SentOsMenuMouseDown = true;
            }
        }
        else
        {
            m_PendingVGuiMouseRelease = true;
            m_Game->m_VguiInput->InternalMouseReleased((ButtonCode_t)0x6B);
            if (m_SentOsMenuMouseDown)
            {
                sendMouseClick(false);
                m_SentOsMenuMouseDown = false;
            }
        }
        m_WasMenuSelectDown = selectDown;
    }

    if (phoneTouch.pressedEdge)
    {
        m_Game->m_VguiInput->SetCursorPos(m_MenuCursorX, m_MenuCursorY);
        m_PendingVGuiMousePress = true;
        m_Game->m_VguiInput->InternalMousePressed((ButtonCode_t)0x6B);
    }
    if (phoneTouch.releasedEdge)
    {
        m_Game->m_VguiInput->SetCursorPos(m_MenuCursorX, m_MenuCursorY);
        m_PendingVGuiMouseRelease = true;
        m_Game->m_VguiInput->InternalMouseReleased((ButtonCode_t)0x6B);
    }

    vr::IVROverlay* pOverlay = vr::VROverlay();
    if (pOverlay && m_MainMenuHandle != vr::k_ulOverlayHandleInvalid)
        pOverlay->SetOverlayFlag(m_MainMenuHandle, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, false);

    if (isInGamePhoneMenu)
    {
        if (navBack)
        {
            ClosePhonePauseMenu();
        }
        return;
    }

    if (m_In3DViewControlMenu)
    {
        if (navBack)
            sendKeyTap(VK_ESCAPE);
        if (navUp)
            sendKeyTap(VK_UP);
        if (navDown)
            sendKeyTap(VK_DOWN);
        if (navLeft)
            sendKeyTap(VK_LEFT);
        if (navRight)
            sendKeyTap(VK_RIGHT);
        if (navSelect)
            sendKeyTap(VK_RETURN);
        return;
    }

    // 2D Options / LoadGame Sub-Menus
    if (navSelect)
        sendKeyTap(VK_RETURN);
    if (navBack)
    {
        if (m_Game->m_EngineClient->IsInGame() && !Is2DSubMenuVisible())
        {
            ClosePhonePauseMenu();
        }
        else
            sendKeyTap(VK_ESCAPE);
    }
    if (navUp)
        sendKeyTap(VK_UP);
    if (navDown)
        sendKeyTap(VK_DOWN);
    if (navLeft)
        sendKeyTap(VK_LEFT);
    if (navRight)
        sendKeyTap(VK_RIGHT);
}

void VR::ProcessInput()
{
    if (!m_IsVREnabled)
        return;

    if (m_MeasureHeightRequested.load(std::memory_order_acquire))
    {
        MeasureStandingPlayerHeight();
    }

    typedef std::chrono::duration<float, std::milli> duration;
    auto currentTime = std::chrono::steady_clock::now();
    duration elapsed = currentTime - m_PrevFrameTime;
    float deltaTime = std::clamp(elapsed.count(), 0.1f, 100.0f);
    m_PrevFrameTime = currentTime;
    float dtSec = deltaTime * 0.001f;

    // -------------------------------------------------------------------------
    // 1. Two-Stick Click Chord:
    //    - Hold both Left + Right Stick Clicks (L3 + R3) for 0.8s -> Toggle Live Calibration Mode
    //    - Quick tap both Left + Right Stick Clicks (< 0.4s) -> ResetPosition()
    // -------------------------------------------------------------------------
    bool leftStickClick = PressedDigitalAction(m_ActionResetPosition);
    bool rightStickClick = PressedDigitalAction(m_ActionActivateVR);
    bool leftStickJustPressed = PressedDigitalAction(m_ActionResetPosition, true);

    if (leftStickClick && rightStickClick)
    {
        m_StickChordHoldMs += deltaTime;
        if (!m_StickChordHandled && m_StickChordHoldMs >= 800.0f)
        {
            m_StickChordHandled = true;
            m_CalibrationMode = !m_CalibrationMode;

            // Haptic feedback pulse on both controllers
            TriggerHapticPulseOnHand(true, 3500, 0.85f);
            TriggerHapticPulseOnHand(false, 3500, 0.85f);

            if (m_CalibrationMode)
            {
                {
                    std::lock_guard<std::mutex> lock(m_ConfigMutex);
                    s_CalSavedRightPos = m_RightHandPosOffset;
                    s_CalSavedRightAng = m_RightHandAngOffset;
                    s_CalSavedLeftPos  = m_LeftHandPosOffset;
                    s_CalSavedLeftAng  = m_LeftHandAngOffset;
                    s_CalHaveSavedOffsets = true;
                }
                s_CalWaitRightGripRelease = true;
                s_CalWaitLeftGripRelease  = true;
                s_CalRightGripHoldMs = 0.0f;
                s_CalLeftGripHoldMs  = 0.0f;
            }
            else
            {
                m_CalibratingRight = false;
                m_CalibratingLeft = false;
                m_CalRightPoseLocked = false;
                m_CalLeftPoseLocked = false;
                s_CalWaitRightGripRelease = false;
                s_CalWaitLeftGripRelease  = false;
                s_CalRightGripHoldMs = 0.0f;
                s_CalLeftGripHoldMs  = 0.0f;
                s_CalHaveSavedOffsets = false;
                if (m_CalibrationDirty)
                {
                    SaveConfigFile();
                    m_CalibrationDirty = false;
                }
            }
        }
    }
    else
    {
        if (m_StickChordHoldMs > 30.0f && m_StickChordHoldMs < 450.0f && !m_StickChordHandled)
        {
            ResetPosition();
        }
        m_StickChordHoldMs = 0.0f;
        m_StickChordHandled = false;
    }

    // R3 standalone tap (< 450 ms, without L3 pressed) -> Cycle Camera Flash mode (Off -> Auto -> On -> Off)
    {
        const bool chordActiveOrHandled = m_StickChordHandled || (m_StickChordHoldMs > 0.0f) || m_CalibrationMode;
        if (s_R3SuppressUntilRelease && !rightStickClick)
            s_R3SuppressUntilRelease = false;

        if (m_CalibrationMode)
        {
            s_R3WasDown = false;
            s_R3HoldMs = 0.0f;
            s_R3SawL3 = true;
            if (rightStickClick)
                s_R3SuppressUntilRelease = true;
        }
        else if (!s_R3SuppressUntilRelease)
        {
            if (rightStickClick)
            {
                if (!s_R3WasDown)
                {
                    s_R3WasDown = true;
                    s_R3HoldMs = 0.0f;
                    s_R3SawL3 = leftStickClick || chordActiveOrHandled;
                }
                s_R3HoldMs += deltaTime;
                if (leftStickClick || chordActiveOrHandled)
                    s_R3SawL3 = true;
            }
            else if (s_R3WasDown)
            {
                if (!s_R3SawL3 && !leftStickClick && !chordActiveOrHandled &&
                    s_R3HoldMs > 0.0f && s_R3HoldMs < 450.0f &&
                    !m_RightHandEmpty && !m_PhoneEquipped)
                {
                    switch (m_CameraFlashMode)
                    {
                    case CameraFlashMode::Off:  m_CameraFlashMode = CameraFlashMode::Auto; break;
                    case CameraFlashMode::Auto: m_CameraFlashMode = CameraFlashMode::On;   break;
                    case CameraFlashMode::On:
                    default:                    m_CameraFlashMode = CameraFlashMode::Off;  break;
                    }

                    float flashHapticAmp = 0.55f;
                    if (m_CameraFlashMode == CameraFlashMode::Off)
                        flashHapticAmp = 0.25f;
                    else if (m_CameraFlashMode == CameraFlashMode::On)
                        flashHapticAmp = 0.85f;
                    TriggerHapticPulseOnHand(false, 800, flashHapticAmp);

                    if (m_Game && m_Game->m_EngineClient &&
                        m_Game->m_EngineClient->IsInGame() &&
                        !m_Game->m_EngineClient->IsLevelMainMenuBackground() &&
                        !m_Game->m_EngineClient->IsPaused())
                    {
                        m_Game->m_EngineClient->ClientCmd_Unrestricted("play items/camera/focus.wav");
                    }
                }
                s_R3WasDown = false;
                s_R3HoldMs = 0.0f;
                s_R3SawL3 = false;
            }
        }
    }

    // -------------------------------------------------------------------------
    // 2. Real-Space "Move Hand & Release" Controller Offset Calibration Mode
    //    - Only active after holding L3 + R3 for 0.8s.
    //    - Hold Right Grip (or Left Grip): Freezes the hand/tool stationary in world space
    //      while you move & rotate your physical hand into your natural grip.
    //    - Release Grip: Locks new 6DOF offset, permanently saves to VR\config.txt,
    //      and automatically exits Calibration Mode so normal gameplay grips never trigger it!
    // -------------------------------------------------------------------------
    m_IsCalibrating = false;
    if (m_CalibrationMode)
    {
        // Check Trigger FIRST: measure direct vertical eye-to-controller distance for waist belt height
        // (immune to uncalibrated SteamVR floor height) and restore pre-calibration hand offsets.
        const bool rightTrig = PressedDigitalAction(m_ActionPrimaryAttack, true);
        const bool leftTrig  = PressedDigitalAction(m_ActionUse, true);
        if (rightTrig || leftTrig)
        {
            if (s_CalHaveSavedOffsets)
            {
                std::lock_guard<std::mutex> lock(m_ConfigMutex);
                m_RightHandPosOffset = s_CalSavedRightPos;
                m_RightHandAngOffset = s_CalSavedRightAng;
                m_LeftHandPosOffset  = s_CalSavedLeftPos;
                m_LeftHandAngOffset  = s_CalSavedLeftAng;
            }
            if (CalibrateBeltHeightFromController(leftTrig && !rightTrig))
            {
                s_SuppressPrimaryAttackUntilRelease = PressedDigitalAction(m_ActionPrimaryAttack);
                m_CalibrationMode = false;
                m_CalibratingRight = false;
                m_CalibratingLeft = false;
                m_CalRightPoseLocked = false;
                m_CalLeftPoseLocked = false;
                m_CalibrationDirty = false;
                s_CalHaveSavedOffsets = false;
                s_CalWaitRightGripRelease = false;
                s_CalWaitLeftGripRelease  = false;
                s_CalRightGripHoldMs = 0.0f;
                s_CalLeftGripHoldMs  = 0.0f;
                return;
            }
        }

        bool calRightGrip = PressedDigitalAction(m_ActionReload);
        bool calLeftGrip  = PressedDigitalAction(m_ActionCrouch);

        if (!calRightGrip)
        {
            s_CalWaitRightGripRelease = false;
            s_CalRightGripHoldMs = 0.0f;
        }
        else if (!s_CalWaitRightGripRelease)
        {
            s_CalRightGripHoldMs += deltaTime;
        }

        if (!calLeftGrip)
        {
            s_CalWaitLeftGripRelease = false;
            s_CalLeftGripHoldMs = 0.0f;
        }
        else if (!s_CalWaitLeftGripRelease)
        {
            s_CalLeftGripHoldMs += deltaTime;
        }

        const bool wantCalRight = calRightGrip && !s_CalWaitRightGripRelease && (s_CalRightGripHoldMs >= 250.0f);
        const bool wantCalLeft  = calLeftGrip  && !s_CalWaitLeftGripRelease  && (s_CalLeftGripHoldMs  >= 250.0f);

        bool prevCalRight = m_CalibratingRight;
        bool prevCalLeft  = m_CalibratingLeft;

        m_CalibratingRight = wantCalRight;
        m_CalibratingLeft  = wantCalLeft;

        if (m_CalibratingRight)
            m_CalibrationDirty = true;
        if (m_CalibratingLeft)
            m_CalibrationDirty = true;

        // Holding grip freezes the hand in world space; move your real hand, release to save
        if (m_CalibratingRight || m_CalibratingLeft)
        {
            m_IsCalibrating = true;
            return;
        }

        // Released Right or Left grip -> Save to VR\config.txt, haptic confirmation, and auto-exit Calibration Mode!
        if ((prevCalRight && !m_CalibratingRight) || (prevCalLeft && !m_CalibratingLeft))
        {
            m_CalibrationMode = false;
            m_CalRightPoseLocked = false;
            m_CalLeftPoseLocked = false;
            m_CalibrationDirty = false;
            s_CalHaveSavedOffsets = false;
            SaveConfigFile();

            if (prevCalRight && !m_CalibratingRight)
            {
                TriggerHapticPulseOnHand(false, 2500, 0.75f);
            }
            if (prevCalLeft && !m_CalibratingLeft)
            {
                TriggerHapticPulseOnHand(true, 2500, 0.75f);
            }
        }
        return;
    }
    else
    {
        m_CalibratingRight = false;
        m_CalibratingLeft = false;
        m_CalRightPoseLocked = false;
        m_CalLeftPoseLocked = false;
        s_CalWaitRightGripRelease = false;
        s_CalWaitLeftGripRelease  = false;
        s_CalRightGripHoldMs = 0.0f;
        s_CalLeftGripHoldMs  = 0.0f;
        s_CalHaveSavedOffsets = false;
    }

    // -------------------------------------------------------------------------
    // 3. HL2:VR / INFRA Controller Mapping for Gameplay
    // -------------------------------------------------------------------------
    vr::InputAnalogActionData_t analogActionData {};
    bool stickJump = false;
    bool stickDownNow = false;

    if (GetAnalogActionData(m_ActionTurn, analogActionData))
    {
        // Right Stick Left/Right = Turn
        float yawDelta = 0.0f;
        if (m_SnapTurning)
        {
            if (!m_PressedTurn && analogActionData.x > 0.55f)
            {
                yawDelta = -m_SnapTurnAngle;
                m_PressedTurn = true;
            }
            else if (!m_PressedTurn && analogActionData.x < -0.55f)
            {
                yawDelta = m_SnapTurnAngle;
                m_PressedTurn = true;
            }
            else if (analogActionData.x < 0.3f && analogActionData.x > -0.3f)
            {
                m_PressedTurn = false;
            }
        }
        else
        {
            float deadzone = 0.22f;
            if (std::fabs(analogActionData.x) > deadzone)
            {
                float xNormalized = (std::fabs(analogActionData.x) - deadzone) / (1.0f - deadzone);
                yawDelta = ((analogActionData.x > deadzone) ? -1.0f : 1.0f) * m_TurnSpeed * deltaTime * xNormalized;
            }
        }

        if (yawDelta != 0.0f)
        {
            // Pivot strictly around the live HMD XY position ("auf der Stelle drehen")
            // so turning with 6DOF enabled never orbits around m_Center when off-center.
            if (m_6DOF && !m_In3DViewControlMenu)
            {
                const Vector hmdPosLocal = m_HmdPose.TrackedDevicePos;
                Vector offset = hmdPosLocal - m_Center;
                offset.z = 0.0f;
                VectorPivotXY(offset, { 0, 0, 0 }, -yawDelta);
                const float newCenterX = hmdPosLocal.x - offset.x;
                const float newCenterY = hmdPosLocal.y - offset.y;
                m_HmdPosRelativeRawPrev.x -= (newCenterX - m_Center.x);
                m_HmdPosRelativeRawPrev.y -= (newCenterY - m_Center.y);
                m_Center.x = newCenterX;
                m_Center.y = newCenterY;
            }
            m_RotationOffset.y += yawDelta;
            m_RotationOffset.y -= 360.0f * std::floor(m_RotationOffset.y / 360.0f);
            UpdateTracking();
        }
        else
        {
            m_RotationOffset.y -= 360.0f * std::floor(m_RotationOffset.y / 360.0f);
        }

        // HL2:VR Right Stick Up = Jump, Right Stick Down = Crouch Toggle
        if (analogActionData.y > 0.60f)
            stickJump = true;
        else if (analogActionData.y < -0.60f)
            stickDownNow = true;
    }

    m_StickJumpActive = stickJump;

    // Crouch as Toggle on Right Stick Down rising edge
    static bool s_WasStickDown = false;
    if (stickDownNow && !s_WasStickDown)
    {
        m_StickCrouchActive = !m_StickCrouchActive;
    }
    s_WasStickDown = stickDownNow;

    // Sprint as Toggle on Left Stick Click rising edge (auto-stops when Left Stick returns to neutral)
    if (leftStickJustPressed && !rightStickClick)
    {
        m_SprintActive = !m_SprintActive;
    }
    vr::InputAnalogActionData_t walkData {};
    if (GetAnalogActionData(m_ActionWalk, walkData))
    {
        if (std::fabs(walkData.x) < 0.20f && std::fabs(walkData.y) < 0.20f)
        {
            m_SprintActive = false;
        }
    }

    // Helper to send +cmd / -cmd ONLY on state transitions (never spam every frame!)
    auto updateHoldCommand = [&](bool isDown, bool &wasDown, const char *downCmd, const char *upCmd) {
        if (isDown != wasDown)
        {
            m_Game->ClientCmd_Unrestricted(isDown ? downCmd : upCmd);
            wasDown = isDown;
        }
    };

    static bool s_WasSprinting = false;
    updateHoldCommand(m_SprintActive, s_WasSprinting, "+speed", "-speed");

    // Right Trigger = Take Photo / Primary Attack (+attack & IN_ATTACK)
    bool rawPrimaryAttackDown = PressedDigitalAction(m_ActionPrimaryAttack);
    if (s_SuppressPrimaryAttackUntilRelease && !rawPrimaryAttackDown)
        s_SuppressPrimaryAttackUntilRelease = false;
    bool primaryAttackDown = !s_SuppressPrimaryAttackUntilRelease && rawPrimaryAttackDown;
    static bool s_WasPrimaryAttack = false;
    updateHoldCommand(primaryAttackDown, s_WasPrimaryAttack, "+attack", "-attack");

    // Left A / X Button:
    // - When Camera is equipped: Single-button Camera LCD Zoom cycle (1.0x Wide -> 1.6x -> 2.5x -> 1.0x)
    // - Otherwise: Secondary Action (+attack2 & IN_ATTACK2)
    const bool cameraActiveForZoom = (!m_RightHandEmpty && !m_PhoneEquipped);
    if (!cameraActiveForZoom)
    {
        m_CameraZoomTarget = 1.0f;
        m_CameraZoomFactor = 1.0f;
    }

    bool rawAltFireDown = PressedDigitalAction(m_ActionSecondaryAttack) || PressedDigitalAction(m_ActionJump);
    if (m_SuppressAltFireUntilRelease && !rawAltFireDown)
        m_SuppressAltFireUntilRelease = false;
    bool altFireDown = !m_SuppressAltFireUntilRelease && rawAltFireDown;
    static bool s_WasAltFireRaw = false;
    static bool s_WasAltFireCmd = false;
    if (cameraActiveForZoom)
    {
        if (altFireDown && !s_WasAltFireRaw)
        {
            if (m_CameraZoomTarget < 1.35f)
                m_CameraZoomTarget = 1.6f;
            else if (m_CameraZoomTarget < 2.10f)
                m_CameraZoomTarget = 2.5f;
            else
                m_CameraZoomTarget = 1.0f;

            const float tickAmp = (m_CameraZoomTarget > 2.0f) ? 0.65f : ((m_CameraZoomTarget > 1.2f) ? 0.48f : 0.32f);
            TriggerHapticPulseOnHand(false, 650, tickAmp);
        }
        updateHoldCommand(false, s_WasAltFireCmd, "+attack2", "-attack2");
    }
    else
    {
        updateHoldCommand(altFireDown, s_WasAltFireCmd, "+attack2", "-attack2");
    }
    s_WasAltFireRaw = altFireDown;

    // Right Stick Up = Jump (+jump & IN_JUMP)
    static bool s_WasJump = false;
    updateHoldCommand(stickJump, s_WasJump, "+jump", "-jump");

    // Right Stick Down = Crouch Toggle (+duck & IN_DUCK)
    static bool s_WasCrouch = false;
    updateHoldCommand(m_StickCrouchActive, s_WasCrouch, "+duck", "-duck");

    // Left B / Y Button:
    // - Short tap (< 0.45s): Equip / Holster Flashlight (initially OFF when equipped)
    // - Hold (>= 0.45s): Reload Flashlight Battery
    bool rawFlashlightDown = (PressedDigitalAction(m_ActionFlashlight) || PressedDigitalAction(m_Scoreboard)) && !leftStickClick;
    if (m_SuppressFlashlightUntilRelease && !rawFlashlightDown)
        m_SuppressFlashlightUntilRelease = false;
    bool flashlightBtnDown = !m_SuppressFlashlightUntilRelease && rawFlashlightDown;
    if (flashlightBtnDown)
    {
        m_FlashlightBtnHoldMs += deltaTime;
        if ((m_AllowQuickReload || m_BatteryInventoryMode == BatteryInventoryMode::Off) && !m_FlashlightReloadTriggered && m_FlashlightBtnHoldMs >= 450.0f)
        {
            m_FlashlightReloadTriggered = true;
            m_PendingFlashlightReload = true;
            TriggerHapticPulseOnHand(true, 2500, 0.7f);
        }
    }
    else
    {
        if (m_FlashlightBtnHoldMs > 10.0f && !m_FlashlightReloadTriggered)
        {
            m_FlashlightEquipped = !m_FlashlightEquipped;
            m_FlashlightBeamOn = false;
        }
        m_FlashlightBtnHoldMs = 0.0f;
        m_FlashlightReloadTriggered = false;
    }

    // Left Trigger:
    // - Short tap: Toggle Flashlight Beam ON / OFF (when Flashlight is equipped)
    // - Hold (>= 1.0s with SuperFlashlight equipped): Activate Flashlight Boost (5s high beam, 10s cooldown)
    const bool rawLeftTrigDown = PressedDigitalAction(m_ActionUse);
    if (m_SuppressFlashlightTriggerUntilRelease && !rawLeftTrigDown)
        m_SuppressFlashlightTriggerUntilRelease = false;
    const bool leftTrigDown = !m_SuppressFlashlightTriggerUntilRelease && rawLeftTrigDown;

    if (!m_FlashlightEquipped)
    {
        m_FlashlightTriggerHeld = false;
        m_FlashlightTriggerTurnedOnBeam = false;
        m_FlashlightBoostTriggered = false;
        m_FlashlightTriggerHoldMs = 0.0f;
    }
    else if (!m_HasSuperFlashlight)
    {
        if (leftTrigDown && !m_FlashlightTriggerHeld)
        {
            m_FlashlightBeamOn = !m_FlashlightBeamOn;
        }
        m_FlashlightTriggerHeld = leftTrigDown;
        m_FlashlightTriggerTurnedOnBeam = false;
        m_FlashlightBoostTriggered = false;
        m_FlashlightTriggerHoldMs = 0.0f;
    }
    else
    {
        if (leftTrigDown)
        {
            if (!m_FlashlightTriggerHeld)
            {
                m_FlashlightTriggerHeld = true;
                m_FlashlightTriggerHoldMs = 0.0f;
                m_FlashlightBoostTriggered = false;
                if (!m_FlashlightBeamOn)
                {
                    m_FlashlightBeamOn = true;
                    m_FlashlightTriggerTurnedOnBeam = true;
                }
                else
                {
                    m_FlashlightTriggerTurnedOnBeam = false;
                }
            }
            m_FlashlightTriggerHoldMs += deltaTime;
            if (!m_FlashlightBoostTriggered && m_FlashlightTriggerHoldMs >= 1000.0f)
            {
                m_FlashlightBoostTriggered = true;
                m_PendingFlashlightBoost = true;
            }
        }
        else if (m_FlashlightTriggerHeld)
        {
            if (!m_FlashlightBoostTriggered && !m_FlashlightTriggerTurnedOnBeam)
            {
                m_FlashlightBeamOn = false;
            }
            m_FlashlightTriggerHeld = false;
            m_FlashlightTriggerTurnedOnBeam = false;
            m_FlashlightBoostTriggered = false;
            m_FlashlightTriggerHoldMs = 0.0f;
        }
    }

    // Both Left Grip and Right Grip = Interact / Use / Grab (+use & IN_USE)
    // Hold-to-Grab: holding Grip grabs physics objects; releasing Grip immediately drops them in dUpdateObject / dProcessUsercmds.
    bool leftGripDown  = PressedDigitalAction(m_ActionCrouch);
    bool rightGripDown = PressedDigitalAction(m_ActionReload);
    if (!m_IsHoldingObject)
    {
        if (leftGripDown && !rightGripDown)
            m_UseWithLeftHand = !m_LeftHanded;
        else if (rightGripDown && !leftGripDown)
            m_UseWithLeftHand = m_LeftHanded;
    }

    const bool debouncedUseDown = VRGrab::IsDebouncedUseGripDown(this);
    static bool s_WasUse = false;
    updateHoldCommand(debouncedUseDown, s_WasUse, "+use", "-use");

    // Touchpad Swipe Down (or Lower-Half Click) to Reload Battery:
    // - Right Touchpad Swipe Down -> Camera Battery
    // - Left Touchpad Swipe Down  -> Flashlight Battery
    // - Left Touchpad Swipe Up    -> Toggle Flashlight Grip Style (Overhand <-> Underhand)
    struct TouchpadAxisFilter
    {
        float prevX = 0.0f;
        float prevY = 0.0f;
        int unchangedFrames = 0;
    };

    struct TouchpadSwipeState
    {
        bool wasTouching = false;
        bool swipeTriggered = false;
        bool wasPadPressed = false;
        float peakY = 0.0f;
        float minY = 0.0f;
        TouchpadAxisFilter axisFilter{};
        std::chrono::steady_clock::time_point lastTouchTime = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point lastTriggerTime = std::chrono::steady_clock::now();
    };

    auto readTouchpadAxis = [&](vr::ETrackedControllerRole role, vr::VRActionHandle_t tpAction,
                                TouchpadAxisFilter &filter,
                                float &outX, float &outY, bool &outTouching, bool &outPadPressed, bool &outNonZero)
    {
        outX = 0.0f;
        outY = 0.0f;
        outTouching = false;
        outPadPressed = false;
        outNonZero = false;

        bool hasCtrlState = (role == vr::TrackedControllerRole_LeftHand) ? m_HasLeftControllerState : m_HasRightControllerState;
        const vr::VRControllerState_t &ctrlState = (role == vr::TrackedControllerRole_LeftHand) ? m_LeftControllerState : m_RightControllerState;
        bool hwPadTouched = false;
        if (hasCtrlState)
        {
            const uint64_t padMask = vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Touchpad);
            outPadPressed = ((ctrlState.ulButtonPressed & padMask) != 0);
            hwPadTouched  = ((ctrlState.ulButtonTouched & padMask) != 0);
        }

        bool actionActive = false;
        if (tpAction != vr::k_ulInvalidActionHandle)
        {
            vr::InputAnalogActionData_t tpData {};
            if (GetAnalogActionData(tpAction, tpData) && tpData.bActive)
            {
                actionActive = true;
                if (std::fabs(tpData.x) > 0.001f || std::fabs(tpData.y) > 0.001f)
                {
                    outX = tpData.x;
                    outY = tpData.y;
                    outNonZero = true;
                    if (std::fabs(outX - filter.prevX) < 0.0002f && std::fabs(outY - filter.prevY) < 0.0002f)
                        filter.unchangedFrames++;
                    else
                        filter.unchangedFrames = 0;
                    filter.prevX = outX;
                    filter.prevY = outY;

                    // Treat as active touch if hardware touch bit is set OR coordinates are actively moving
                    if (hwPadTouched || filter.unchangedFrames <= 10)
                        outTouching = true;
                }
                else
                {
                    filter.unchangedFrames = 0;
                    if (hwPadTouched)
                    {
                        outX = tpData.x;
                        outY = tpData.y;
                        outTouching = true;
                    }
                }
            }
            else
            {
                filter.unchangedFrames = 0;
            }
        }

        if (!outTouching && !actionActive && hwPadTouched && hasCtrlState)
        {
            // Note: On Index Controllers (knuckles), rAxis[0] is the thumbstick and rAxis[1] or rAxis[0] may be used depending on legacy binding.
            // Only read rAxis[0] when hwPadTouched is explicitly true and tpAction didn't provide an active stream.
            outX = ctrlState.rAxis[0].x;
            outY = ctrlState.rAxis[0].y;
            outNonZero = (std::fabs(outX) > 0.001f || std::fabs(outY) > 0.001f);
            outTouching = true;
        }
    };

    auto checkTouchpadSwipeDown = [&](vr::ETrackedControllerRole role, vr::VRActionHandle_t tpAction, vr::VRActionHandle_t discreteAction,
                                      TouchpadSwipeState &st) -> bool
    {
        if (discreteAction != vr::k_ulInvalidActionHandle && PressedDigitalAction(discreteAction, true))
            return true;

        float tpX = 0.0f, tpY = 0.0f;
        bool touching = false;
        bool padPressed = false;
        bool nonZero = false;
        readTouchpadAxis(role, tpAction, st.axisFilter, tpX, tpY, touching, padPressed, nonZero);

        // Pressing/clicking the lower half of the trackpad also triggers reload immediately
        bool padJustPressed = padPressed && !st.wasPadPressed;
        st.wasPadPressed = padPressed;
        if (padJustPressed && tpY <= 0.20f)
        {
            st.swipeTriggered = true;
            st.minY = tpY;
            st.lastTriggerTime = currentTime;
            TriggerHapticPulseOnHand(role == vr::TrackedControllerRole_LeftHand, 2200, 0.65f);
            return true;
        }

        if (touching)
        {
            st.lastTouchTime = currentTime;
            if (!st.wasTouching)
            {
                st.wasTouching = true;
                st.peakY = tpY;
                st.minY = tpY;
                st.swipeTriggered = false;
            }
            else
            {
                if (st.swipeTriggered)
                {
                    if (tpY < st.minY)
                        st.minY = tpY;
                    float msSinceTrigger = std::chrono::duration<float, std::milli>(currentTime - st.lastTriggerTime).count();
                    // Re-arm as soon as the thumb moves back up by >= 0.06f or starts a new touch after 300ms
                    if ((tpY - st.minY) >= 0.06f || (msSinceTrigger > 300.0f && tpY > st.peakY))
                    {
                        st.swipeTriggered = false;
                        st.peakY = tpY;
                        st.minY = tpY;
                    }
                }
                else
                {
                    if (tpY > st.peakY)
                        st.peakY = tpY;
                    if ((st.peakY - tpY) >= 0.12f)
                    {
                        st.swipeTriggered = true;
                        st.minY = tpY;
                        st.lastTriggerTime = currentTime;
                        TriggerHapticPulseOnHand(role == vr::TrackedControllerRole_LeftHand, 2200, 0.65f);
                        return true;
                    }
                }
            }
        }
        else if (st.wasTouching)
        {
            // Keep touch state alive briefly (180ms) across Y=0.0 deadzone, then reset cleanly
            float msSinceTouch = std::chrono::duration<float, std::milli>(currentTime - st.lastTouchTime).count();
            if (msSinceTouch > 180.0f)
            {
                st.wasTouching = false;
                st.swipeTriggered = false;
            }
        }
        return false;
    };

    const bool quickReloadActive = (m_AllowQuickReload || m_BatteryInventoryMode == BatteryInventoryMode::Off);
    static TouchpadSwipeState s_RightTpState {};
    if (quickReloadActive && checkTouchpadSwipeDown(vr::TrackedControllerRole_RightHand, m_ActionRightTrackpad, m_ActionReloadCamera, s_RightTpState))
    {
        m_PendingCameraReload = true;
        m_CameraReloadTicks = 10;
    }

    // Right Trackpad Up / Down -> Smooth Camera LCD Zoom (1.0x Wide .. 3.0x Tele) when Camera is equipped
    {
        struct RightTpZoomState
        {
            TouchpadAxisFilter axisFilter{};
            bool wasTouching = false;
            bool hasPrevY = false;
            float prevY = 0.0f;
            int lastDetent = 4; // floor(zoom * 4)
            std::chrono::steady_clock::time_point lastTouchTime{};
        };
        static RightTpZoomState s_RightTpZoom{};

        if (cameraActiveForZoom && !quickReloadActive)
        {
            float tpX = 0.0f, tpY = 0.0f;
            bool touching = false;
            bool padPressed = false;
            bool nonZero = false;
            readTouchpadAxis(vr::TrackedControllerRole_RightHand, m_ActionRightTrackpad, s_RightTpZoom.axisFilter, tpX, tpY, touching, padPressed, nonZero);

            const float dtSec = std::clamp(deltaTime * 0.001f, 0.0f, 0.05f);
            float prevZoom = m_CameraZoomTarget;

            if (touching)
            {
                s_RightTpZoom.lastTouchTime = currentTime;
                if (!s_RightTpZoom.wasTouching)
                {
                    s_RightTpZoom.wasTouching = true;
                    s_RightTpZoom.hasPrevY = nonZero;
                    s_RightTpZoom.prevY = tpY;
                    s_RightTpZoom.lastDetent = static_cast<int>(floorf(m_CameraZoomTarget * 4.0f + 0.001f));
                }
                else if (!s_RightTpZoom.hasPrevY)
                {
                    if (nonZero)
                    {
                        s_RightTpZoom.hasPrevY = true;
                        s_RightTpZoom.prevY = tpY;
                    }
                }
                else
                {
                    if (padPressed && std::fabs(tpY) > 0.25f)
                    {
                        // Holding upper / lower trackpad click acts like a smooth DSLR zoom rocker
                        const float dir = (tpY > 0.0f) ? 1.0f : -1.0f;
                        m_CameraZoomTarget = std::clamp(m_CameraZoomTarget + dir * 1.45f * dtSec, 1.0f, 3.0f);
                        s_RightTpZoom.prevY = tpY;
                    }
                    else
                    {
                        // Sliding thumb up / down smoothly zooms in / out
                        const float dy = tpY - s_RightTpZoom.prevY;
                        if (std::fabs(dy) >= 0.004f && std::fabs(dy) < 0.65f)
                        {
                            m_CameraZoomTarget = std::clamp(m_CameraZoomTarget + dy * 1.65f, 1.0f, 3.0f);
                            s_RightTpZoom.prevY = tpY;
                        }
                        else if (std::fabs(dy) >= 0.65f)
                        {
                            s_RightTpZoom.prevY = tpY;
                        }
                    }
                }
            }
            else if (s_RightTpZoom.wasTouching)
            {
                const float msSinceTouch = std::chrono::duration<float, std::milli>(currentTime - s_RightTpZoom.lastTouchTime).count();
                if (msSinceTouch > 180.0f)
                {
                    s_RightTpZoom.wasTouching = false;
                    s_RightTpZoom.hasPrevY = false;
                }
            }

            if (std::fabs(m_CameraZoomTarget - prevZoom) > 1e-4f)
            {
                const int detent = static_cast<int>(floorf(m_CameraZoomTarget * 4.0f + 0.001f));
                if (detent != s_RightTpZoom.lastDetent)
                {
                    s_RightTpZoom.lastDetent = detent;
                    TriggerHapticPulseOnHand(false, 420, 0.28f);
                }
            }
        }
        else
        {
            s_RightTpZoom.wasTouching = false;
            s_RightTpZoom.hasPrevY = false;
        }

        // Smoothly interpolate active zoom factor toward target (silky on button steps, snappy on trackpad slide)
        if (cameraActiveForZoom)
        {
            const float dtSec = std::clamp(deltaTime * 0.001f, 0.0f, 0.05f);
            const float blend = 1.0f - expf(-16.0f * dtSec);
            m_CameraZoomFactor += (m_CameraZoomTarget - m_CameraZoomFactor) * blend;
            if (std::fabs(m_CameraZoomTarget - m_CameraZoomFactor) < 0.002f)
                m_CameraZoomFactor = m_CameraZoomTarget;
        }
    }

    static TouchpadSwipeState s_LeftTpState {};
    if (quickReloadActive && checkTouchpadSwipeDown(vr::TrackedControllerRole_LeftHand, m_ActionLeftTrackpad, m_ActionReloadFlashlight, s_LeftTpState))
    {
        m_PendingFlashlightReload = true;
        s_LastLeftSwipeDownTime = currentTime;
    }

    // Left Trackpad Swipe-Up -> Toggle Flashlight Grip Style (Overhand <-> Underhand)
    {
        float tpX = 0.0f, tpY = 0.0f;
        bool touching = false;
        bool padPressed = false;
        bool nonZero = false;
        TouchpadAxisFilter upFilter{ s_LeftTpUpState.prevX, s_LeftTpUpState.prevY, s_LeftTpUpState.unchangedFrames };
        readTouchpadAxis(vr::TrackedControllerRole_LeftHand, m_ActionLeftTrackpad, upFilter, tpX, tpY, touching, padPressed, nonZero);
        s_LeftTpUpState.prevX = upFilter.prevX;
        s_LeftTpUpState.prevY = upFilter.prevY;
        s_LeftTpUpState.unchangedFrames = upFilter.unchangedFrames;

        if (touching)
        {
            s_LeftTpUpState.lastTouchTime = currentTime;
            if (!s_LeftTpUpState.wasTouching)
            {
                s_LeftTpUpState.wasTouching = true;
                s_LeftTpUpState.swipeTriggered = false;
                s_LeftTpUpState.hasStartY = nonZero;
                s_LeftTpUpState.zeroFrames = nonZero ? 0 : 1;
                s_LeftTpUpState.startY = tpY;
                s_LeftTpUpState.peakY = tpY;
            }
            else if (!s_LeftTpUpState.hasStartY)
            {
                if (nonZero || ++s_LeftTpUpState.zeroFrames >= 2)
                {
                    s_LeftTpUpState.hasStartY = true;
                    s_LeftTpUpState.startY = tpY;
                    s_LeftTpUpState.peakY = tpY;
                }
            }
            else
            {
                if (tpY > s_LeftTpUpState.peakY)
                    s_LeftTpUpState.peakY = tpY;

                const float msSinceUp   = std::chrono::duration<float, std::milli>(currentTime - s_LeftTpUpState.lastTriggerTime).count();
                const float msSinceDown = std::chrono::duration<float, std::milli>(currentTime - s_LastLeftSwipeDownTime).count();
                if (!s_LeftTpUpState.swipeTriggered)
                {
                    if (m_FlashlightEquipped && (tpY - s_LeftTpUpState.startY) > 0.45f &&
                        msSinceUp >= 350.0f && msSinceDown >= 350.0f)
                    {
                        m_FlashlightOverhandGrip = !m_FlashlightOverhandGrip;
                        TriggerHapticPulseOnHand(true, 800, 0.55f);
                        s_LeftTpUpState.swipeTriggered = true;
                        s_LeftTpUpState.lastTriggerTime = currentTime;
                    }
                }
                else if (msSinceUp >= 350.0f && (s_LeftTpUpState.peakY - tpY) >= 0.35f)
                {
                    s_LeftTpUpState.swipeTriggered = false;
                    s_LeftTpUpState.startY = tpY;
                    s_LeftTpUpState.peakY = tpY;
                }
            }
        }
        else if (s_LeftTpUpState.wasTouching)
        {
            float msSinceTouch = std::chrono::duration<float, std::milli>(currentTime - s_LeftTpUpState.lastTouchTime).count();
            if (msSinceTouch > 180.0f)
            {
                s_LeftTpUpState.wasTouching = false;
                s_LeftTpUpState.hasStartY = false;
                s_LeftTpUpState.swipeTriggered = false;
                s_LeftTpUpState.zeroFrames = 0;
            }
        }
    }

    // Right A Button (gameplay; also MenuSelect in menus):
    // - Short tap (< 0.45s): Equip / Holster Camera (toggle between infra_camera and infra_hands)
    // - Hold (>= 0.45s): Reload Camera Battery
    bool rawCameraBtnDown = PressedDigitalAction(m_ActionNextItem) ||
                            PressedDigitalAction(m_ActionPrevItem) ||
                            PressedDigitalAction(m_Spray);
    if (m_SuppressCameraUntilRelease && !rawCameraBtnDown)
        m_SuppressCameraUntilRelease = false;
    bool cameraBtnDown = !m_SuppressCameraUntilRelease && rawCameraBtnDown;
    if (cameraBtnDown)
    {
        m_CameraBtnHoldMs += deltaTime;
        if ((m_AllowQuickReload || m_BatteryInventoryMode == BatteryInventoryMode::Off) && !m_CameraReloadTriggered && m_CameraBtnHoldMs >= 450.0f)
        {
            m_CameraReloadTriggered = true;
            m_PendingCameraReload = true;
            m_CameraReloadTicks = 10;
            TriggerHapticPulseOnHand(false, 2500, 0.7f);
        }
    }
    else
    {
        if (m_CameraBtnHoldMs > 10.0f && !m_CameraReloadTriggered)
        {
            if (m_RightHandEmpty)
                m_Game->ClientCmd_Unrestricted("use infra_camera");
            else
                m_Game->ClientCmd_Unrestricted("use infra_hands");
        }
        m_CameraBtnHoldMs = 0.0f;
        m_CameraReloadTriggered = false;
    }

    m_RenderedHud = false;

    // Right B Button (gameplay; also MenuBack in menus) = Open / Close In-Game 3D Phone Pause Menu
    // (gameui_activate triggers CINFRAMainMenu::OnTick -> "use infra_phone").
    // Toggle: if the phone pause menu is already open (engine paused by CINFRAPhone, or opened
    // within the last 2s while the phone draw animation still plays), Right B closes it instead.
    bool rawPauseDown = PressedDigitalAction(m_Pause);
    if (m_SuppressPauseUntilRelease && !rawPauseDown)
        m_SuppressPauseUntilRelease = false;
    if (!m_SuppressPauseUntilRelease && PressedDigitalAction(m_Pause, true))
    {
        float sinceOpenSec = std::chrono::duration<float>(std::chrono::steady_clock::now() - m_PauseOpenedAt).count();
        bool menuOpen = m_Game->m_EngineClient->IsPaused() || (m_PauseOpenRequested && sinceOpenSec < 2.0f);
        if (menuOpen)
        {
            ClosePhonePauseMenu();
        }
        else
        {
            OpenPhonePauseMenu();
        }
    }

    // Allow direct fingertip tapping on an unpaused equipped 3D cell phone (incoming call / SMS)
    if (m_PhoneEquipped && !m_In3DViewControlMenu && !Is2DSubMenuVisible() &&
        m_Game && m_Game->m_VguiInput && m_Game->m_VguiSurface)
    {
        const VRHands::PhoneScreenTouchResult phoneTouch = VRHands::PollPhoneScreenTouch(this, m_SetupOrigin);
        if (phoneTouch.hovering || phoneTouch.pressedEdge || phoneTouch.releasedEdge)
        {
            int sw = 1920, sh = 1080;
            m_Game->m_VguiSurface->GetScreenSize(sw, sh);
            if (sw <= 0) sw = 1920;
            if (sh <= 0) sh = 1080;
            if (phoneTouch.hovering)
            {
                m_MenuCursorX = std::clamp((int)(phoneTouch.u * (float)(sw - 1)), 0, sw - 1);
                m_MenuCursorY = std::clamp((int)(phoneTouch.v * (float)(sh - 1)), 0, sh - 1);
            }
            m_Game->m_VguiInput->SetCursorPos(m_MenuCursorX, m_MenuCursorY);
            if (phoneTouch.pressedEdge)
            {
                m_PendingVGuiMousePress = true;
                m_Game->m_VguiInput->InternalMousePressed((ButtonCode_t)0x6B);
            }
            if (phoneTouch.releasedEdge)
            {
                m_PendingVGuiMouseRelease = true;
                m_Game->m_VguiInput->InternalMouseReleased((ButtonCode_t)0x6B);
            }
        }
    }
}

void VR::OpenPhonePauseMenu()
{
    if (!m_Game || !m_Game->m_EngineClient || !m_Game->m_VguiSurface)
        return;

    int sw = 1920, sh = 1080;
    m_Game->m_VguiSurface->GetScreenSize(sw, sh);
    if (sw <= 0) sw = 1920;
    if (sh <= 0) sh = 1080;
    m_MenuCursorX = sw / 2;
    m_MenuCursorY = (int)(sh * 0.28f); // Start directly on the top button of the phone menu
    m_MenuCursorInitialized = false;
    m_Game->ClientCmd_Unrestricted("gameui_activate");
    RepositionOverlays(m_Poses[vr::k_unTrackedDeviceIndex_Hmd]);
    m_PauseOpenRequested = true;
    m_PauseOpenedAt = std::chrono::steady_clock::now();
}

void VR::ClosePhonePauseMenu()
{
    m_PauseOpenRequested = false;
    if (m_Game)
        m_Game->ClientCmd_Unrestricted("unpause; gameui_hide; use infra_hands");
}

VMatrix VR::VMatrixFromHmdMatrix(const vr::HmdMatrix34_t &hmdMat)
{
    // VMatrix has a different implicit coordinate system than HmdMatrix34_t, but this function does not convert between them
    VMatrix vMat(
        hmdMat.m[0][0], hmdMat.m[1][0], hmdMat.m[2][0], 0.0f,
        hmdMat.m[0][1], hmdMat.m[1][1], hmdMat.m[2][1], 0.0f,
        hmdMat.m[0][2], hmdMat.m[1][2], hmdMat.m[2][2], 0.0f,
        hmdMat.m[0][3], hmdMat.m[1][3], hmdMat.m[2][3], 1.0f
    );

    return vMat;
}

vr::HmdMatrix34_t VR::VMatrixToHmdMatrix(const VMatrix &vMat)
{
    vr::HmdMatrix34_t hmdMat = {0};

    hmdMat.m[0][0] = vMat.m[0][0];
    hmdMat.m[1][0] = vMat.m[0][1];
    hmdMat.m[2][0] = vMat.m[0][2];

    hmdMat.m[0][1] = vMat.m[1][0];
    hmdMat.m[1][1] = vMat.m[1][1];
    hmdMat.m[2][1] = vMat.m[1][2];

    hmdMat.m[0][2] = vMat.m[2][0];
    hmdMat.m[1][2] = vMat.m[2][1];
    hmdMat.m[2][2] = vMat.m[2][2];

    hmdMat.m[0][3] = vMat.m[3][0];
    hmdMat.m[1][3] = vMat.m[3][1];
    hmdMat.m[2][3] = vMat.m[3][2];

    return hmdMat;
}

vr::HmdMatrix34_t VR::GetControllerTipMatrix(vr::ETrackedControllerRole controllerRole)
{
    vr::VRInputValueHandle_t inputValue = vr::k_ulInvalidInputValueHandle;

    if (controllerRole == vr::TrackedControllerRole_RightHand)
    {
        m_Input->GetInputSourceHandle("/user/hand/right", &inputValue);
    }
    else if (controllerRole == vr::TrackedControllerRole_LeftHand)
    {
        m_Input->GetInputSourceHandle("/user/hand/left", &inputValue);
    }

    if (inputValue != vr::k_ulInvalidInputValueHandle)
    {
        char buffer[vr::k_unMaxPropertyStringSize];

        m_System->GetStringTrackedDeviceProperty(vr::VRSystem()->GetTrackedDeviceIndexForControllerRole(controllerRole), vr::Prop_RenderModelName_String, 
                                                 buffer, vr::k_unMaxPropertyStringSize);

        vr::RenderModel_ControllerMode_State_t controllerState = {0};
        vr::RenderModel_ComponentState_t componentState = {0};

        if (vr::VRRenderModels()->GetComponentStateForDevicePath(buffer, vr::k_pch_Controller_Component_Tip, inputValue, &controllerState, &componentState))
        {
            return componentState.mTrackingToComponentLocal;
        }
    }

    // Not a hand controller role or tip lookup failed, return identity
    const vr::HmdMatrix34_t identity = 
    {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f
    };

    return identity;
}

bool VR::CheckOverlayIntersectionForController(vr::VROverlayHandle_t overlayHandle, vr::ETrackedControllerRole controllerRole)
{
    vr::TrackedDeviceIndex_t deviceIndex = m_System->GetTrackedDeviceIndexForControllerRole(controllerRole);

    if (deviceIndex == vr::k_unTrackedDeviceIndexInvalid)
        return false;

    vr::TrackedDevicePose_t &controllerPose = m_Poses[deviceIndex];

    if (!controllerPose.bPoseIsValid)
        return false;

    VMatrix controllerVMatrix = VMatrixFromHmdMatrix(controllerPose.mDeviceToAbsoluteTracking);
    VMatrix tipVMatrix        = VMatrixFromHmdMatrix(GetControllerTipMatrix(controllerRole));
    tipVMatrix.MatrixMul(controllerVMatrix, controllerVMatrix);

    vr::VROverlayIntersectionParams_t  params  = {0};
    vr::VROverlayIntersectionResults_t results = {0};

    vr::IVRCompositor* pCompositor = vr::VRCompositor();
    if (!pCompositor)
        return false;
    params.eOrigin    = pCompositor->GetTrackingSpace();
    params.vSource    = { controllerVMatrix.m[3][0],  controllerVMatrix.m[3][1],  controllerVMatrix.m[3][2]};
    params.vDirection = {-controllerVMatrix.m[2][0], -controllerVMatrix.m[2][1], -controllerVMatrix.m[2][2]};

    vr::IVROverlay* pOverlay = vr::VROverlay();
    if (!pOverlay)
        return false;
    return pOverlay->ComputeOverlayIntersection(overlayHandle, &params, &results);
}

QAngle VR::GetLeftControllerAbsAngle()
{
    return m_LeftControllerAngAbs;
}

QAngle& VR::GetLeftControllerAbsAngleConst()
{
    return m_LeftControllerAngAbs;
}

Vector VR::GetLeftControllerAbsPos(Vector eyePosition)
{
    Vector position = m_SetupOrigin + m_LeftControllerPosRel;

    if (m_6DOF)
        position += m_HmdPosRelative;

    return position;
}

QAngle VR::GetRightControllerAbsAngle()
{
    return m_RightControllerAngAbs;
}

QAngle& VR::GetRightControllerAbsAngleConst()
{
    return m_RightControllerAngAbs;
}

Vector VR::GetRightControllerAbsPos(Vector eyePosition)
{
    Vector position = m_SetupOrigin + m_RightControllerPosRel;

    if (m_6DOF)
        position += m_HmdPosRelative;

    return position;
}

Vector VR::GetLeftPointerAbsPos()
{
    Vector position = m_SetupOrigin + m_LeftPointerPosRel;
    if (m_6DOF)
        position += m_HmdPosRelative;
    return position;
}

Vector VR::GetRightPointerAbsPos()
{
    Vector position = m_SetupOrigin + m_RightPointerPosRel;
    if (m_6DOF)
        position += m_HmdPosRelative;
    return position;
}

Vector VR::GetRecommendedViewmodelAbsPos(Vector eyePosition)
{
    Vector viewmodelPos = GetRightControllerAbsPos(eyePosition);
    viewmodelPos -= m_ViewmodelForward * m_ViewmodelPosOffset.x;
    viewmodelPos -= m_ViewmodelRight * m_ViewmodelPosOffset.y;
    viewmodelPos -= m_ViewmodelUp * m_ViewmodelPosOffset.z;

    return viewmodelPos;
}

QAngle VR::GetRecommendedViewmodelAbsAngle()
{
    QAngle result{};

    QAngle::VectorAngles(m_ViewmodelForward, m_ViewmodelUp, result);

    return result;
}

void VR::UpdateHMDAngles() {
    QAngle hmdAngLocal = m_HmdPose.TrackedDeviceAng;

    //hmdAngLocal += m_RotationOffset;
    hmdAngLocal.x += m_RotationOffset.x;
    hmdAngLocal.y += m_RotationOffset.y;
    hmdAngLocal.z += m_RotationOffset.z;

    //hmdAngLocal.Normalize();

    QAngle::AngleVectors(hmdAngLocal, &m_HmdForward, &m_HmdRight, &m_HmdUp);

    //hmdAngLocal.x = (hmdAngLocal.x > 180 ? 180)
    hmdAngLocal.Normalize();

    m_HmdAngAbs = hmdAngLocal;
}

void VR::ResetPosition()
{
    m_Center = m_HmdPose.TrackedDevicePos;
    m_RoomscaleCmdActive = false;
    if (m_In3DViewControlMenu)
    {
        m_RotationOffset.y = m_MenuAnchorYaw - m_HmdPose.TrackedDeviceAng.y;
        m_RotationOffset.y -= 360.0f * std::floor(m_RotationOffset.y / 360.0f);
    }
}

void AngleMatrix(const QAngle& angles, matrix3x4_t& matrix);
void MatrixAngles(const matrix3x4_t& matrix, float* angles);

void VR::UpdateTracking()
{
    GetPoses();

    auto computeControllerSpeedMps = [&](vr::TrackedDeviceIndex_t devIdx, int slot) -> float
    {
        struct SpeedFallbackState
        {
            bool hasPrev = false;
            Vector prevPosMeters{ 0.0f, 0.0f, 0.0f };
            std::chrono::steady_clock::time_point prevTime{};
            float cachedFallbackSpeed = 0.0f;
        };
        static SpeedFallbackState s_Fallback[2]{};
        SpeedFallbackState& st = s_Fallback[slot];

        if (devIdx >= vr::k_unMaxTrackedDeviceCount)
        {
            st.hasPrev = false;
            st.cachedFallbackSpeed = 0.0f;
            return 0.0f;
        }

        const vr::TrackedDevicePose_t& rawPose = m_Poses[devIdx];
        if (!rawPose.bPoseIsValid)
        {
            st.hasPrev = false;
            st.cachedFallbackSpeed = 0.0f;
            return 0.0f;
        }

        const auto now = std::chrono::steady_clock::now();
        const Vector curPosMeters(
            rawPose.mDeviceToAbsoluteTracking.m[0][3],
            rawPose.mDeviceToAbsoluteTracking.m[1][3],
            rawPose.mDeviceToAbsoluteTracking.m[2][3]);

        float fallbackSpeed = st.cachedFallbackSpeed;
        if (st.hasPrev)
        {
            const float dt = std::chrono::duration<float>(now - st.prevTime).count();
            if (dt >= 0.002f)
            {
                if (dt <= 0.25f &&
                    std::isfinite(curPosMeters.x) && std::isfinite(curPosMeters.y) && std::isfinite(curPosMeters.z))
                {
                    const float dx = curPosMeters.x - st.prevPosMeters.x;
                    const float dy = curPosMeters.y - st.prevPosMeters.y;
                    const float dz = curPosMeters.z - st.prevPosMeters.z;
                    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                    fallbackSpeed = (dist <= 1.0f) ? (dist / dt) : 0.0f;
                }
                else
                {
                    fallbackSpeed = 0.0f;
                }
                st.prevPosMeters = curPosMeters;
                st.prevTime = now;
                st.cachedFallbackSpeed = fallbackSpeed;
            }
        }
        else if (std::isfinite(curPosMeters.x) && std::isfinite(curPosMeters.y) && std::isfinite(curPosMeters.z))
        {
            st.prevPosMeters = curPosMeters;
            st.prevTime = now;
            st.cachedFallbackSpeed = 0.0f;
            st.hasPrev = true;
            fallbackSpeed = 0.0f;
        }

        const float vx = rawPose.vVelocity.v[0];
        const float vy = rawPose.vVelocity.v[1];
        const float vz = rawPose.vVelocity.v[2];
        if (std::isfinite(vx) && std::isfinite(vy) && std::isfinite(vz))
        {
            const float velSpeed = std::sqrt(vx * vx + vy * vy + vz * vz);
            if (velSpeed > 1e-4f)
                return std::clamp(velSpeed, 0.0f, 25.0f);
        }
        return std::clamp(fallbackSpeed, 0.0f, 25.0f);
    };

    m_LeftHandSpeedMps  = computeControllerSpeedMps(m_LeftControllerIndex, 0);
    m_RightHandSpeedMps = computeControllerSpeedMps(m_RightControllerIndex, 1);

    // HMD tracking
    Vector hmdPosLocal = m_HmdPose.TrackedDevicePos;
    if (!m_6DOF && !m_In3DViewControlMenu)
    {
        m_Center.x = hmdPosLocal.x;
        m_Center.y = hmdPosLocal.y;
    }
    Vector hmdPosCentered = hmdPosLocal - m_Center;

    m_HmdPosRelativeRaw = hmdPosCentered;

    Vector hmdPosCorrected = hmdPosCentered;
    VectorPivotXY(hmdPosCorrected, { 0, 0, 0 }, m_RotationOffset.y);
    
    UpdateHMDAngles();

    m_HmdPosRelative = hmdPosCorrected * m_VRScale;

    static bool s_HasViewParams = false;
    static unsigned long long s_LastViewParamsMs = 0ULL;
    const unsigned long long viewParamsNowMs = GetTickCount64();
    if (!s_HasViewParams || (viewParamsNowMs - s_LastViewParamsMs) >= 250ULL)
    {
        GetViewParameters();
        s_HasViewParams = true;
        s_LastViewParamsMs = viewParamsNowMs;
    }
    m_Ipd = m_EyeToHeadTransformPosRight.x * 2;
    m_EyeZ = m_EyeToHeadTransformPosRight.z;

    // Hand tracking
    Vector leftControllerPosLocal = m_LeftControllerPose.TrackedDevicePos;
    QAngle leftControllerAngLocal = m_LeftControllerPose.TrackedDeviceAng;

    Vector rightControllerPosLocal = m_RightControllerPose.TrackedDevicePos;
    QAngle rightControllerAngLocal = m_RightControllerPose.TrackedDeviceAng;

    Vector hmdToLeftController = leftControllerPosLocal - hmdPosLocal;
    VectorPivotXY(hmdToLeftController, { 0, 0, 0 }, m_RotationOffset.y);
    Vector rawLeftPosRel = hmdToLeftController * m_VRScale;

    Vector hmdToController = rightControllerPosLocal - hmdPosLocal;
    VectorPivotXY(hmdToController, { 0, 0, 0 }, m_RotationOffset.y);
    Vector rawRightPosRel = hmdToController * m_VRScale;

    leftControllerAngLocal.x += m_RotationOffset.x;
    leftControllerAngLocal.y += m_RotationOffset.y;
    leftControllerAngLocal.z += m_RotationOffset.z;

    rightControllerAngLocal.x += m_RotationOffset.x;
    rightControllerAngLocal.y += m_RotationOffset.y;
    rightControllerAngLocal.z += m_RotationOffset.z;

    QAngle::AngleVectors(leftControllerAngLocal, &m_LeftControllerForward, &m_LeftControllerRight, &m_LeftControllerUp);
    QAngle::AngleVectors(rightControllerAngLocal, &m_RightControllerForward, &m_RightControllerRight, &m_RightControllerUp);

    const float basePitchOffset = -30.0f;

    // Base controller ergonomic orientation (before custom per-hand tilt)
    Vector baseLeftFwd   = VectorRotate(m_LeftControllerForward, m_LeftControllerRight, basePitchOffset);
    Vector baseLeftUp    = VectorRotate(m_LeftControllerUp, m_LeftControllerRight, basePitchOffset);
    Vector baseLeftRight = m_LeftControllerRight;

    Vector baseRightFwd   = VectorRotate(m_RightControllerForward, m_RightControllerRight, basePitchOffset);
    Vector baseRightUp    = VectorRotate(m_RightControllerUp, m_RightControllerRight, basePitchOffset);
    Vector baseRightRight = m_RightControllerRight;

    // Physical pointer pose for hand-based use selection: the real controller position with the ergonomic
    // pointing direction, deliberately WITHOUT the camera/flashlight grip calibration offsets
    // (e.g. RightHandAngOffset 76/-52/-39 deg, LeftHandAngOffset 69/-154/-162 deg) which rotate the
    // viewmodel frame far away from where the real index finger points.
    m_LeftPointerPosRel  = rawLeftPosRel;
    m_RightPointerPosRel = rawRightPosRel;
    m_LeftPointerFwd     = baseLeftFwd;
    m_LeftPointerRight   = baseLeftRight;
    m_LeftPointerUp      = baseLeftUp;
    m_RightPointerFwd    = baseRightFwd;
    m_RightPointerRight  = baseRightRight;
    m_RightPointerUp     = baseRightUp;

    const float cmToUnits = 0.01f * m_VRScale;

    // Helper: Apply 6DOF PosOffset (cm) & AngOffset (deg) in (baseFwd, -baseRight, baseUp) orthonormal basis
    auto applyHandOffset = [&](const Vector& rawPosRel, const Vector& bFwd, const Vector& bRight, const Vector& bUp,
                               const Vector& posOffsetCm, const QAngle& angOffsetDeg,
                               Vector& outPosRel, Vector& outFwd, Vector& outRight, Vector& outUp)
    {
        outPosRel = rawPosRel + (bFwd   * (posOffsetCm.x * cmToUnits))
                              + (bRight * (posOffsetCm.y * cmToUnits))
                              + (bUp    * (posOffsetCm.z * cmToUnits));

        matrix3x4_t rot;
        AngleMatrix(angOffsetDeg, rot);

        Vector bLeft = bRight * -1.0f;
        outFwd        = (bFwd * rot[0][0]) + (bLeft * rot[1][0]) + (bUp * rot[2][0]);
        Vector outLeft= (bFwd * rot[0][1]) + (bLeft * rot[1][1]) + (bUp * rot[2][1]);
        outRight      = outLeft * -1.0f;
        outUp         = (bFwd * rot[0][2]) + (bLeft * rot[1][2]) + (bUp * rot[2][2]);
    };

    // Helper: Extract 6DOF PosOffset (cm) & AngOffset (deg) from locked world-relative pose
    auto extractHandOffset = [&](const Vector& rawPosRel, const Vector& bFwd, const Vector& bRight, const Vector& bUp,
                                 const Vector& lockedPosRel, const Vector& lockedFwd, const Vector& lockedRight, const Vector& lockedUp,
                                 Vector& outPosOffsetCm, QAngle& outAngOffsetDeg)
    {
        Vector deltaPos = lockedPosRel - rawPosRel;
        outPosOffsetCm.x = DotProduct(deltaPos, bFwd)   / cmToUnits;
        outPosOffsetCm.y = DotProduct(deltaPos, bRight) / cmToUnits;
        outPosOffsetCm.z = DotProduct(deltaPos, bUp)    / cmToUnits;

        Vector bLeft      = bRight * -1.0f;
        Vector lockedLeft = lockedRight * -1.0f;

        matrix3x4_t rot{};
        rot[0][0] = DotProduct(lockedFwd,  bFwd);
        rot[1][0] = DotProduct(lockedFwd,  bLeft);
        rot[2][0] = DotProduct(lockedFwd,  bUp);

        rot[0][1] = DotProduct(lockedLeft, bFwd);
        rot[1][1] = DotProduct(lockedLeft, bLeft);
        rot[2][1] = DotProduct(lockedLeft, bUp);

        rot[0][2] = DotProduct(lockedUp,   bFwd);
        rot[1][2] = DotProduct(lockedUp,   bLeft);
        rot[2][2] = DotProduct(lockedUp,   bUp);

        MatrixAngles(rot, &outAngOffsetDeg.x);
    };

    {
        std::lock_guard<std::mutex> configLock(m_ConfigMutex);

        Vector& leftToolPosOff  = m_LeftHanded ? m_RightHandPosOffset : m_LeftHandPosOffset;
        QAngle& leftToolAngOff  = m_LeftHanded ? m_RightHandAngOffset : m_LeftHandAngOffset;
        Vector& rightToolPosOff = m_LeftHanded ? m_LeftHandPosOffset  : m_RightHandPosOffset;
        QAngle& rightToolAngOff = m_LeftHanded ? m_LeftHandAngOffset  : m_RightHandAngOffset;

        // Physical Left Hand (Flashlight by default, Camera when LeftHanded=true)
        if (m_CalibratingLeft)
        {
            if (!m_CalLeftPoseLocked)
            {
                applyHandOffset(rawLeftPosRel, baseLeftFwd, baseLeftRight, baseLeftUp,
                                leftToolPosOff, leftToolAngOff,
                                m_CalLeftLockedPosRel, m_CalLeftLockedFwd, m_CalLeftLockedRight, m_CalLeftLockedUp);
                m_CalLeftPoseLocked = true;
            }
            extractHandOffset(rawLeftPosRel, baseLeftFwd, baseLeftRight, baseLeftUp,
                              m_CalLeftLockedPosRel, m_CalLeftLockedFwd, m_CalLeftLockedRight, m_CalLeftLockedUp,
                              leftToolPosOff, leftToolAngOff);
            m_LeftControllerPosRel  = m_CalLeftLockedPosRel;
            m_LeftControllerForward = m_CalLeftLockedFwd;
            m_LeftControllerRight   = m_CalLeftLockedRight;
            m_LeftControllerUp      = m_CalLeftLockedUp;
        }
        else
        {
            m_CalLeftPoseLocked = false;
            applyHandOffset(rawLeftPosRel, baseLeftFwd, baseLeftRight, baseLeftUp,
                            leftToolPosOff, leftToolAngOff,
                            m_LeftControllerPosRel, m_LeftControllerForward, m_LeftControllerRight, m_LeftControllerUp);
        }

        // Physical Right Hand (Camera by default, Flashlight when LeftHanded=true)
        if (m_CalibratingRight)
        {
            if (!m_CalRightPoseLocked)
            {
                applyHandOffset(rawRightPosRel, baseRightFwd, baseRightRight, baseRightUp,
                                rightToolPosOff, rightToolAngOff,
                                m_CalRightLockedPosRel, m_CalRightLockedFwd, m_CalRightLockedRight, m_CalRightLockedUp);
                m_CalRightPoseLocked = true;
            }
            extractHandOffset(rawRightPosRel, baseRightFwd, baseRightRight, baseRightUp,
                              m_CalRightLockedPosRel, m_CalRightLockedFwd, m_CalRightLockedRight, m_CalRightLockedUp,
                              rightToolPosOff, rightToolAngOff);
            m_RightControllerPosRel  = m_CalRightLockedPosRel;
            m_RightControllerForward = m_CalRightLockedFwd;
            m_RightControllerRight   = m_CalRightLockedRight;
            m_RightControllerUp      = m_CalRightLockedUp;
        }
        else
        {
            m_CalRightPoseLocked = false;
            applyHandOffset(rawRightPosRel, baseRightFwd, baseRightRight, baseRightUp,
                            rightToolPosOff, rightToolAngOff,
                            m_RightControllerPosRel, m_RightControllerForward, m_RightControllerRight, m_RightControllerUp);
        }

        // Final controller angles in world space
        QAngle::VectorAngles(m_LeftControllerForward, m_LeftControllerUp, m_LeftControllerAngAbs);
        m_LeftControllerAngAbs.Normalize();
        QAngle::VectorAngles(m_RightControllerForward, m_RightControllerUp, m_RightControllerAngAbs);
        m_RightControllerAngAbs.Normalize();

        m_LeftControllerPosAbs = GetLeftControllerAbsPos();

        PositionAngle viewmodelOffset = PositionAngle{ {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f} };

        // Apply both hardcoded and custom (from config) viewmodel offsets here:
        m_ViewmodelPosOffset = viewmodelOffset.position + m_ViewmodelPosCustomOffset;
        m_ViewmodelAngOffset = viewmodelOffset.angle + m_ViewmodelAngCustomOffset;
    }

    m_ViewmodelForward = m_RightControllerForward;
    m_ViewmodelUp = m_RightControllerUp;
    m_ViewmodelRight = m_RightControllerRight;

    // Viewmodel yaw offset
    m_ViewmodelForward = VectorRotate(m_ViewmodelForward, m_ViewmodelUp, m_ViewmodelAngOffset.y);
    m_ViewmodelRight = VectorRotate(m_ViewmodelRight, m_ViewmodelUp, m_ViewmodelAngOffset.y);

    // Viewmodel pitch offset
    m_ViewmodelForward = VectorRotate(m_ViewmodelForward, m_ViewmodelRight, m_ViewmodelAngOffset.x);
    m_ViewmodelUp = VectorRotate(m_ViewmodelUp, m_ViewmodelRight, m_ViewmodelAngOffset.x);

    // Viewmodel roll offset
    m_ViewmodelRight = VectorRotate(m_ViewmodelRight, m_ViewmodelForward, m_ViewmodelAngOffset.z);
    m_ViewmodelUp = VectorRotate(m_ViewmodelUp, m_ViewmodelForward, m_ViewmodelAngOffset.z);

    // Record 90-144 Hz OpenVR controller linear + wrist-flick velocities for physics throwing on release.
    VRGrab::RecordHandTrackingVelocity(this);
}

Vector VR::GetViewAngle()
{
    return Vector( m_HmdAngAbs.x, m_HmdAngAbs.y, m_HmdAngAbs.z );
}

Vector VR::GetViewOrigin(Vector setupOrigin)
{
    Vector center = setupOrigin;

    if (m_6DOF)
        center += m_HmdPosRelative;

    return center + (m_HmdForward * -(m_EyeZ * m_VRScale));
}

Vector VR::GetViewOriginLeft(Vector setupOrigin)
{
    Vector viewOriginLeft = GetViewOrigin(setupOrigin);
    viewOriginLeft -= m_HmdRight * ((m_Ipd * m_IpdScale * m_VRScale) / 2);

    return viewOriginLeft;
}

Vector VR::GetViewOriginRight(Vector setupOrigin)
{
    Vector viewOriginRight = GetViewOrigin(setupOrigin);
    viewOriginRight += m_HmdRight * ((m_Ipd * m_IpdScale * m_VRScale) / 2);

    return viewOriginRight;
}

Vector VR::Trace(uint32_t* localPlayer) {
    Vector vecStart = GetRightControllerAbsPos();
    Vector vecEnd = vecStart + m_RightControllerForward * MAX_TRACE_LENGTH;

    CGameTrace trace;
    Ray_t ray;
    CTraceFilterSkipNPCsAndPlayers tracefilter((IHandleEntity*)localPlayer, 0);

    ray.Init(vecStart, vecEnd);

    m_Game->m_EngineTrace->TraceRay(ray, MASK_SHOT | MASK_SHOT_HULL, &tracefilter, &trace);

    return trace.endpos;
}

void AngleMatrix(const QAngle& angles, matrix3x4_t& matrix)
{
    float sr, sp, sy, cr, cp, cy;

    SinCos(DEG2RAD(angles[YAW]), &sy, &cy);
    SinCos(DEG2RAD(angles[PITCH]), &sp, &cp);
    SinCos(DEG2RAD(angles[ROLL]), &sr, &cr);

    // matrix = (YAW * PITCH) * ROLL
    matrix[0][0] = cp * cy;
    matrix[1][0] = cp * sy;
    matrix[2][0] = -sp;

    // NOTE: Do not optimize this to reduce multiplies! optimizer bug will screw this up.
    matrix[0][1] = sr * sp * cy + cr * -sy;
    matrix[1][1] = sr * sp * sy + cr * cy;
    matrix[2][1] = sr * cp;
    matrix[0][2] = (cr * sp * cy + -sr * -sy);
    matrix[1][2] = (cr * sp * sy + -sr * cy);
    matrix[2][2] = cr * cp;

    matrix[0][3] = 0.0f;
    matrix[1][3] = 0.0f;
    matrix[2][3] = 0.0f;
}

void MatrixCopy(const matrix3x4_t& in, matrix3x4_t& out)
{
    memcpy(out.Base(), in.Base(), sizeof(float) * 3 * 4);
}


/*
================
R_ConcatTransforms
================
*/

void ConcatTransforms(const matrix3x4_t& in1, const matrix3x4_t& in2, matrix3x4_t& out)
{
    if (&in1 == &out)
    {
        matrix3x4_t in1b;
        MatrixCopy(in1, in1b);
        ConcatTransforms(in1b, in2, out);
        return;
    }
    if (&in2 == &out)
    {
        matrix3x4_t in2b;
        MatrixCopy(in2, in2b);
        ConcatTransforms(in1, in2b, out);
        return;
    }
    out[0][0] = in1[0][0] * in2[0][0] + in1[0][1] * in2[1][0] +
        in1[0][2] * in2[2][0];
    out[0][1] = in1[0][0] * in2[0][1] + in1[0][1] * in2[1][1] +
        in1[0][2] * in2[2][1];
    out[0][2] = in1[0][0] * in2[0][2] + in1[0][1] * in2[1][2] +
        in1[0][2] * in2[2][2];
    out[0][3] = in1[0][0] * in2[0][3] + in1[0][1] * in2[1][3] +
        in1[0][2] * in2[2][3] + in1[0][3];
    out[1][0] = in1[1][0] * in2[0][0] + in1[1][1] * in2[1][0] +
        in1[1][2] * in2[2][0];
    out[1][1] = in1[1][0] * in2[0][1] + in1[1][1] * in2[1][1] +
        in1[1][2] * in2[2][1];
    out[1][2] = in1[1][0] * in2[0][2] + in1[1][1] * in2[1][2] +
        in1[1][2] * in2[2][2];
    out[1][3] = in1[1][0] * in2[0][3] + in1[1][1] * in2[1][3] +
        in1[1][2] * in2[2][3] + in1[1][3];
    out[2][0] = in1[2][0] * in2[0][0] + in1[2][1] * in2[1][0] +
        in1[2][2] * in2[2][0];
    out[2][1] = in1[2][0] * in2[0][1] + in1[2][1] * in2[1][1] +
        in1[2][2] * in2[2][1];
    out[2][2] = in1[2][0] * in2[0][2] + in1[2][1] * in2[1][2] +
        in1[2][2] * in2[2][2];
    out[2][3] = in1[2][0] * in2[0][3] + in1[2][1] * in2[1][3] +
        in1[2][2] * in2[2][3] + in1[2][3];
}

void MatrixAngles(const matrix3x4_t& matrix, float* angles)
{
    float forward[3];
    float left[3];
    float up[3];

    forward[0] = matrix[0][0];
    forward[1] = matrix[1][0];
    forward[2] = matrix[2][0];
    left[0] = matrix[0][1];
    left[1] = matrix[1][1];
    left[2] = matrix[2][1];
    up[2] = matrix[2][2];

    float xyDist = sqrtf(forward[0] * forward[0] + forward[1] * forward[1]);

    if (xyDist > 0.001f)
    {
        angles[1] = RAD2DEG(atan2f(forward[1], forward[0]));
        angles[0] = RAD2DEG(atan2f(-forward[2], xyDist));
        angles[2] = RAD2DEG(atan2f(left[2], up[2]));
    }
    else
    {
        angles[1] = RAD2DEG(atan2f(-left[0], left[1]));
        angles[0] = RAD2DEG(atan2f(-forward[2], xyDist));
        angles[2] = 0;
    }
}

inline void MatrixAngles(const matrix3x4_t& matrix, QAngle& angles)
{
    MatrixAngles(matrix, &angles.x);
}

QAngle TransformAnglesToWorldSpace(const QAngle& angles, const matrix3x4_t& parentMatrix)
{
    matrix3x4_t angToParent, angToWorld;
    AngleMatrix(angles, angToParent);
    ConcatTransforms(parentMatrix, angToParent, angToWorld);
    QAngle out;
    MatrixAngles(angToWorld, out);
    return out;
}

QAngle TransformAnglesToLocalSpace(const QAngle& worldAngles, const matrix3x4_t& parentMatrix)
{
    matrix3x4_t worldToParent{};
    worldToParent[0][0] = parentMatrix[0][0];
    worldToParent[0][1] = parentMatrix[1][0];
    worldToParent[0][2] = parentMatrix[2][0];
    worldToParent[0][3] = 0.0f;
    worldToParent[1][0] = parentMatrix[0][1];
    worldToParent[1][1] = parentMatrix[1][1];
    worldToParent[1][2] = parentMatrix[2][1];
    worldToParent[1][3] = 0.0f;
    worldToParent[2][0] = parentMatrix[0][2];
    worldToParent[2][1] = parentMatrix[1][2];
    worldToParent[2][2] = parentMatrix[2][2];
    worldToParent[2][3] = 0.0f;

    matrix3x4_t angToWorld{}, angToParent{};
    AngleMatrix(worldAngles, angToWorld);
    ConcatTransforms(worldToParent, angToWorld, angToParent);
    QAngle out{};
    MatrixAngles(angToParent, out);
    return out;
}

Vector VR::TraceEye(uint32_t* localPlayer, Vector cameraPos, Vector eyePos, QAngle& eyeAngle) {
    return eyePos;
}

template <typename... Ts>
static void concatErrorMsg(Game& game, const Ts&... args)
{
    std::ostringstream oss;
    (oss << ... << args);
    std::cout << oss.str() << "\n";
}

template <typename T>
static T parseConfigEntry(
    const std::unordered_map<std::string, std::string>& userConfig, Game& game,
    const char* key, const T& defaultValue)
try
{
    const auto itr = userConfig.find(key);

    if (itr == userConfig.end())
    {
        return defaultValue;
    }

    std::string configValue = itr->second;
    while (!configValue.empty() && (configValue.back() == ' ' || configValue.back() == '\t' || configValue.back() == '\r'))
        configValue.pop_back();
    while (!configValue.empty() && (configValue.front() == ' ' || configValue.front() == '\t'))
        configValue.erase(configValue.begin());

    if constexpr (std::is_same_v<T, bool>)
    {
        return configValue == "true" || configValue == "1";
    }
    else if constexpr(std::is_floating_point_v<T>)
    {
        return std::stof(configValue);
    }
    else if constexpr(std::is_integral_v<T>)
    {
        return std::stol(configValue);
    }
    else if constexpr(std::is_same_v<T, std::string>)
    {
        return configValue;
    }
    else
    {
        struct invalid_type;
        return invalid_type{};
    }
}
catch (const std::logic_error& e)
{
    concatErrorMsg(game, "Error parsing config entry with key '", key,
        "' -- reverting to default value of '", defaultValue, "' -- error: (", e.what(), ")");

    return defaultValue;
}

static std::string TrimConfigString(const std::string& s)
{
    size_t start = 0;
    while (start < s.size() && (s[start] == ' ' || s[start] == '\t' || s[start] == '\r' || s[start] == '\n'))
        ++start;
    size_t end = s.size();
    while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r' || s[end - 1] == '\n'))
        --end;
    return s.substr(start, end - start);
}

static float ParseLengthToCm(const std::string& raw, float defaultCm, bool allowBareMeters)
{
    std::string s = TrimConfigString(raw);
    for (char& ch : s)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    for (char& ch : s)
        if (ch == ',') ch = '.';

    if (s.empty() || s == "auto" || s == "default")
        return 0.0f;

    auto parseFloatStrict = [](const std::string& str, float& outVal) -> bool {
        std::string t = TrimConfigString(str);
        if (t.empty())
            return false;
        try
        {
            size_t pos = 0;
            float v = std::stof(t, &pos);
            if (pos != t.size() || !std::isfinite(v))
                return false;
            outVal = v;
            return true;
        }
        catch (...)
        {
            return false;
        }
    };

    // Feet + inches formats: e.g. 5'10", 5'10.5, 5' 10, 5ft10in, 5ft 10, 5ft
    size_t ftSep = s.find('\'');
    size_t ftSepLen = 1;
    if (ftSep == std::string::npos)
    {
        ftSep = s.find("ft");
        ftSepLen = 2;
    }
    if (ftSep != std::string::npos)
    {
        float feet = 0.0f;
        if (!parseFloatStrict(s.substr(0, ftSep), feet) || feet < 0.0f)
            return defaultCm;
        std::string rest = TrimConfigString(s.substr(ftSep + ftSepLen));
        auto stripRestSuffix = [&](const char* suf) {
            const size_t sl = std::strlen(suf);
            if (rest.size() >= sl && rest.compare(rest.size() - sl, sl, suf) == 0)
            {
                rest = TrimConfigString(rest.substr(0, rest.size() - sl));
                return true;
            }
            return false;
        };
        if (!stripRestSuffix("inches") && !stripRestSuffix("inch") && !stripRestSuffix("in"))
            stripRestSuffix("\"");
        float inches = 0.0f;
        if (!rest.empty() && (!parseFloatStrict(rest, inches) || inches < 0.0f))
            return defaultCm;
        return (feet * 12.0f + inches) * 2.54f;
    }

    auto stripUnit = [&](const char* suf) {
        const size_t sl = std::strlen(suf);
        if (s.size() >= sl && s.compare(s.size() - sl, sl, suf) == 0)
        {
            s = TrimConfigString(s.substr(0, s.size() - sl));
            return true;
        }
        return false;
    };

    float multiplier = 1.0f;
    bool hasExplicitUnit = false;
    if (stripUnit("inches") || stripUnit("inch") || stripUnit("in") || stripUnit("\""))
    {
        multiplier = 2.54f;
        hasExplicitUnit = true;
    }
    else if (stripUnit("cm"))
    {
        multiplier = 1.0f;
        hasExplicitUnit = true;
    }
    else if (stripUnit("mm"))
    {
        multiplier = 0.1f;
        hasExplicitUnit = true;
    }
    else if (stripUnit("m"))
    {
        multiplier = 100.0f;
        hasExplicitUnit = true;
    }

    float val = 0.0f;
    if (!parseFloatStrict(s, val) || val < 0.0f)
        return defaultCm;
    if (val == 0.0f)
        return 0.0f;
    if (!hasExplicitUnit && allowBareMeters && val > 0.5f && val < 3.0f)
        multiplier = 100.0f;
    return val * multiplier;
}

static std::string FormatCmCompact(float cm)
{
    char buf[32] = {};
    const float roundedHalf = std::round(cm * 10.0f) * 0.1f;
    if (std::fabs(roundedHalf - std::round(roundedHalf)) < 0.05f)
        snprintf(buf, sizeof(buf), "%.0f", std::round(roundedHalf));
    else
        snprintf(buf, sizeof(buf), "%.1f", roundedHalf);
    return std::string(buf);
}

static std::string FormatInchesCompact(float cm)
{
    char buf[32] = {};
    snprintf(buf, sizeof(buf), "%.1f", cm / 2.54f);
    return std::string(buf);
}

void VR::RecomputeEffectiveBeltHeight()
{
    constexpr float kDefaultBeltDropCm   = 82.0f;
    constexpr float kEyeHeightRatio      = 0.936f;
    constexpr float kBeltAboveInseamCm   = 1.5f;
    constexpr float kBeltDropHeightRatio = 0.480f;

    float eff = kDefaultBeltDropCm;
    if (!m_BeltHeightOffsetAuto && m_BeltHeightOffsetCm > 0.0f)
    {
        eff = m_BeltHeightOffsetCm;
    }
    else if (m_PlayerHeightCm > 0.0f && m_PlayerInseamCm > 0.0f)
    {
        const float eyeCm = (m_MeasuredEyeHeightCm > 80.0f &&
                             std::fabs((m_MeasuredEyeHeightCm / kEyeHeightRatio) - m_PlayerHeightCm) < 1.5f)
            ? m_MeasuredEyeHeightCm
            : (m_PlayerHeightCm * kEyeHeightRatio);
        eff = eyeCm - (m_PlayerInseamCm + kBeltAboveInseamCm);
    }
    else if (m_PlayerHeightCm > 0.0f)
    {
        eff = m_PlayerHeightCm * kBeltDropHeightRatio;
    }
    else if (m_PlayerInseamCm > 0.0f)
    {
        eff = m_PlayerInseamCm;
    }

    eff = std::clamp(eff, 10.0f, 130.0f);
    m_EffectiveBeltHeightOffsetCm.store(eff, std::memory_order_release);
}

bool VR::MeasureStandingPlayerHeight()
{
    vr::TrackedDevicePose_t standingPose = m_Poses[vr::k_unTrackedDeviceIndex_Hmd];
    if (m_System)
    {
        vr::TrackedDevicePose_t queried{};
        m_System->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0.0f, &queried, 1);
        if (queried.bPoseIsValid)
            standingPose = queried;
    }
    if (!standingPose.bPoseIsValid)
        return false;

    const float eyeHeightM = standingPose.mDeviceToAbsoluteTracking.m[1][3];
    if (!std::isfinite(eyeHeightM) || eyeHeightM < 0.90f || eyeHeightM > 2.30f)
        return false;

    const float measuredEyeCm = eyeHeightM * 100.0f;
    const float estimatedHeightCm = std::clamp(std::round((measuredEyeCm / 0.936f) * 2.0f) * 0.5f, 100.0f, 240.0f);

    {
        std::lock_guard<std::mutex> lock(m_ConfigMutex);
        m_MeasuredEyeHeightCm = measuredEyeCm;
        m_PlayerHeightCm = estimatedHeightCm;
        m_BeltHeightOffsetAuto = true;
        m_BeltHeightOffsetCm = 0.0f;
        RecomputeEffectiveBeltHeight();
    }

    m_MeasureHeightRequested.store(false, std::memory_order_release);
    SaveConfigFile();
    TriggerHapticPulseOnHand(true, 3000, 0.85f);
    TriggerHapticPulseOnHand(false, 3000, 0.85f);
    return true;
}

bool VR::CalibrateBeltHeightFromController(bool useLeftController)
{
    vr::TrackedDevicePose_t standingPoses[vr::k_unMaxTrackedDeviceCount]{};
    bool haveStandingQuery = false;
    if (m_System)
    {
        m_System->GetDeviceToAbsoluteTrackingPose(
            vr::TrackingUniverseStanding, 0.0f, standingPoses, vr::k_unMaxTrackedDeviceCount);
        haveStandingQuery = standingPoses[vr::k_unTrackedDeviceIndex_Hmd].bPoseIsValid;
    }

    const vr::TrackedDevicePose_t& hmdPose = haveStandingQuery
        ? standingPoses[vr::k_unTrackedDeviceIndex_Hmd]
        : m_Poses[vr::k_unTrackedDeviceIndex_Hmd];

    bool haveDropCm = false;
    float eyeToHandDropCm = 0.0f;
    const float cmUnits = 0.01f * m_VRScale;
    if (cmUnits > 1e-4f)
    {
        VRHandGripPose gripPose{};
        if (VRHands::ComputeHandGripPose(this, useLeftController, m_SetupOrigin, gripPose, false) && gripPose.valid)
        {
            const float hmdWorldZ = m_SetupOrigin.z + (m_6DOF ? m_HmdPosRelative.z : 0.0f);
            const float palmDropCm = (hmdWorldZ - gripPose.gripWorldPos.z) / cmUnits;
            if (std::isfinite(palmDropCm) && palmDropCm >= 35.0f && palmDropCm <= 125.0f)
            {
                eyeToHandDropCm = palmDropCm;
                haveDropCm = true;
            }
        }
    }

    const float hmdY = hmdPose.bPoseIsValid ? hmdPose.mDeviceToAbsoluteTracking.m[1][3] : 0.0f;
    if (!haveDropCm && hmdPose.bPoseIsValid)
    {
        vr::TrackedDeviceIndex_t ctrlIdx = useLeftController ? m_LeftControllerIndex : m_RightControllerIndex;
        if (m_System)
        {
            const vr::TrackedDeviceIndex_t roleIdx = m_System->GetTrackedDeviceIndexForControllerRole(
                useLeftController ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand);
            if (roleIdx < vr::k_unMaxTrackedDeviceCount)
                ctrlIdx = roleIdx;
        }
        if (ctrlIdx < vr::k_unMaxTrackedDeviceCount)
        {
            const vr::TrackedDevicePose_t& ctrlPose = haveStandingQuery
                ? standingPoses[ctrlIdx]
                : m_Poses[ctrlIdx];
            if (ctrlPose.bPoseIsValid)
            {
                const float rawDropCm = (hmdY - ctrlPose.mDeviceToAbsoluteTracking.m[1][3]) * 100.0f;
                if (std::isfinite(rawDropCm) && rawDropCm >= 35.0f && rawDropCm <= 125.0f)
                {
                    eyeToHandDropCm = rawDropCm;
                    haveDropCm = true;
                }
            }
        }
    }

    if (haveDropCm)
    {
        constexpr float kBeltAboveCalibratedPosCm = 1.5f;
        const float beltDropCm = std::clamp(
            std::round((eyeToHandDropCm - kBeltAboveCalibratedPosCm) * 2.0f) * 0.5f,
            30.0f, 125.0f);
        {
            std::lock_guard<std::mutex> lock(m_ConfigMutex);
            if (hmdPose.bPoseIsValid && std::isfinite(hmdY) && hmdY >= 0.90f && hmdY <= 2.30f)
                m_MeasuredEyeHeightCm = hmdY * 100.0f;
            m_BeltHeightOffsetAuto = false;
            m_BeltHeightOffsetCm = beltDropCm;
            RecomputeEffectiveBeltHeight();
        }
        m_MeasureHeightRequested.store(false, std::memory_order_release);
        SaveConfigFile();
        TriggerHapticPulseOnHand(true, 3000, 0.85f);
        TriggerHapticPulseOnHand(false, 3000, 0.85f);
        return true;
    }

    return MeasureStandingPlayerHeight();
}

void VR::ParseConfigFile()
{
    std::lock_guard<std::mutex> lock(m_ConfigMutex);

    const std::string configPath = ResolveGameRelativePath("VR\\config.txt");
    std::ifstream configStream(configPath);
    std::unordered_map<std::string, std::string> userConfig;

    std::string line;
    while (std::getline(configStream, line))
    {
        size_t hashPos = line.find('#');
        if (hashPos != std::string::npos)
            line.erase(hashPos);
        size_t eqPos = line.find('=');
        if (eqPos == std::string::npos)
            continue;
        std::string key = line.substr(0, eqPos);
        std::string value = line.substr(eqPos + 1);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t' || key.back() == '\r'))
            key.pop_back();
        while (!key.empty() && (key.front() == ' ' || key.front() == '\t'))
            key.erase(key.begin());
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r'))
            value.pop_back();
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
            value.erase(value.begin());
        if (!key.empty())
            userConfig[key] = value;
    }

    // Controller profile defaults (VR\controller_profiles.txt, sections like [knuckles], [default]).
    // Values only apply for keys NOT present in VR\config.txt (user calibration always wins).
    {
        std::string controllerType = "default";
        if (m_System)
        {
            vr::TrackedDeviceIndex_t idx = m_System->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand);
            if (idx == vr::k_unTrackedDeviceIndexInvalid)
                idx = m_System->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_LeftHand);
            if (idx != vr::k_unTrackedDeviceIndexInvalid)
            {
                char buf[128] = {};
                m_System->GetStringTrackedDeviceProperty(idx, vr::Prop_ControllerType_String, buf, sizeof(buf));
                if (buf[0])
                    controllerType = buf;
            }
        }
#ifdef _DEBUG
        std::cout << "Controller type: " << controllerType << "\n";
#endif

        std::unordered_map<std::string, std::unordered_map<std::string, std::string>> profiles;
        std::ifstream profStream(ResolveGameRelativePath("VR\\controller_profiles.txt"));
        std::string section = "default";
        while (std::getline(profStream, line))
        {
            size_t c = line.find('#');
            if (c != std::string::npos) line.erase(c);
            while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) line.pop_back();
            size_t s = line.find_first_not_of(" \t");
            if (s == std::string::npos) continue;
            line = line.substr(s);
            if (line.front() == '[' && line.back() == ']') { section = line.substr(1, line.size() - 2); continue; }
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = line.substr(0, eq), v = line.substr(eq + 1);
            while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
            profiles[section][k] = v;
        }
        auto applyProfile = [&](const std::string& name) {
            auto it = profiles.find(name);
            if (it == profiles.end()) return false;
            for (auto& kv : it->second)
                userConfig.emplace(kv.first, kv.second);
            return true;
        };
        applyProfile(controllerType);
        applyProfile("default");
    }
    if (userConfig.empty())
        return;

    const auto parseOrDefault = [&](const char* key, auto& target,
                                    const auto& defaultValue) 
    { 
        target = parseConfigEntry(userConfig, *m_Game, key, defaultValue);
#ifdef _DEBUG
        std::cout << "Setting '" << key << "' to '" << target << "'\n";
#endif
    };

    const auto parseXYZOrDefault = [&](std::string keyPrefix, auto& target, float defX, float defY, float defZ)
    {
        parseOrDefault((keyPrefix + "X").c_str(), target.x, defX);
        parseOrDefault((keyPrefix + "Y").c_str(), target.y, defY);
        parseOrDefault((keyPrefix + "Z").c_str(), target.z, defZ);
    };

    parseOrDefault("SnapTurning", m_SnapTurning, false);
    parseOrDefault("SnapTurnAngle", m_SnapTurnAngle, 45.0f);
    parseOrDefault("TurnSpeed", m_TurnSpeed, 0.15f);
    parseOrDefault("LeftHanded", m_LeftHanded, false);
    parseOrDefault("VRScale", m_VRScale, 43.2f);
    if (m_VRScale < 5.0f) m_VRScale = 5.0f;
    else if (m_VRScale > 200.0f) m_VRScale = 200.0f;
    parseOrDefault("IPDScale", m_IpdScale, 1.0f);
    parseOrDefault("6DOF", m_6DOF, true);
    parseOrDefault("AimMode", m_AimMode, 2);
    const bool explicitAA = (userConfig.find("AntiAliasing") != userConfig.end());
    uint32_t parsedAA = 4u;
    parseOrDefault("AntiAliasing", parsedAA, 4u);
    if (parsedAA != 0 && parsedAA != 2 && parsedAA != 4 && parsedAA != 8)
        parsedAA = 4;
    m_AntiAliasing = parsedAA;
    m_AntiAliasingConfig = m_AntiAliasing;
    if (!(explicitAA && parsedAA == 0))
    {
        std::ifstream vidIn(ResolveGameRelativePath("infra\\cfg\\video.txt"));
        if (vidIn.is_open())
        {
            std::string vLine;
            while (std::getline(vidIn, vLine))
            {
                if (vLine.find("setting.mat_antialias") != std::string::npos)
                {
                    size_t q2 = vLine.rfind('"');
                    size_t q1 = (q2 != std::string::npos && q2 > 0) ? vLine.rfind('"', q2 - 1) : std::string::npos;
                    if (q1 != std::string::npos && q2 != std::string::npos && q2 > q1 + 1)
                    {
                        try
                        {
                            uint32_t vidAA = (uint32_t)std::stoul(vLine.substr(q1 + 1, q2 - q1 - 1));
                            if (vidAA == 2 || vidAA == 4 || vidAA == 8)
                                m_AntiAliasing = std::max(m_AntiAliasing, vidAA);
                        }
                        catch (...) {}
                    }
                }
            }
        }
    }
    // Once the eye render targets exist, MSAA can only change after a restart: switching live
    // (config edit or in-game video options -> video.txt) mixed 4x and 8x eye/depth surfaces and crashed.
    if (m_CreatedVRTextures && m_HasLockedAntiAliasing)
        m_AntiAliasing = m_AntiAliasingLocked;
    parseOrDefault("RenderScale", m_RenderScale, 1.0f);
    parseOrDefault("RenderWindow", m_RenderWindow, 0);
    parseOrDefault("ShowHands", m_ShowHands, true);
    parseOrDefault("UseTipDistance", m_UseTipDistanceCm, 9.0f);
    parseOrDefault("UseTouchRadius", m_UseTouchRadiusCm, 15.0f);
    parseOrDefault("MenuSeatHeight", m_MenuSeatHeightCm, 23.5f);
    parseOrDefault("MenuSeatForward", m_MenuSeatForwardCm, 0.0f);
    parseOrDefault("IntroSeatHeight", m_IntroSeatHeightCm, 5.0f);
    parseOrDefault("CarSeatHeight", m_CarSeatHeightCm, 15.0f);
    parseOrDefault("MultiThreadedRendering", m_MultiThreadedRendering, true);
    parseOrDefault("KeepActiveInBackground", m_KeepActiveInBackground, true);
    parseOrDefault("VRAMOffload", m_VRAMOffload, true);
    parseOrDefault("DebugFingerLog", m_DebugFingerLog, false);
    VRHands::SetFingerDebugLogEnabled(m_DebugFingerLog);
    parseOrDefault("EnableProfilerHotkey", m_EnableProfilerHotkey, false);
    parseOrDefault("SpatialAudioMode", m_SpatialAudioMode, std::string("steam_audio"));
    for (char& ch : m_SpatialAudioMode)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (m_SpatialAudioMode != "steam_audio" && m_SpatialAudioMode != "dsoal" && m_SpatialAudioMode != "native")
        m_SpatialAudioMode = "steam_audio";
    parseOrDefault("SpatialAudioOcclusion", m_SpatialAudioOcclusion, true);
    parseOrDefault("SpatialAudioRoomSend", m_SpatialAudioRoomSend, 1.0f);
    m_SpatialAudioRoomSend = std::clamp(m_SpatialAudioRoomSend, 0.0f, 1.5f);
    VRAudio::SetConfig(m_SpatialAudioMode, m_SpatialAudioOcclusion, m_SpatialAudioRoomSend);

    std::string batteryInventoryModeStr = "belt";
    parseOrDefault("BatteryInventoryMode", batteryInventoryModeStr, std::string("belt"));
    m_BatteryInventoryMode = ParseBatteryInventoryMode(batteryInventoryModeStr);
    parseOrDefault("BeltHolsterEnabled", m_BeltHolsterEnabled, true);
    std::string playerHeightStr = "0";
    parseOrDefault("PlayerHeight", playerHeightStr, std::string("0"));
    m_PlayerHeightCm = ParseLengthToCm(playerHeightStr, 0.0f, true);
    if (m_PlayerHeightCm > 0.0f)
        m_PlayerHeightCm = std::clamp(m_PlayerHeightCm, 100.0f, 240.0f);

    std::string playerInseamStr = "0";
    parseOrDefault("PlayerInseam", playerInseamStr, std::string("0"));
    m_PlayerInseamCm = ParseLengthToCm(playerInseamStr, 0.0f, false);
    if (m_PlayerInseamCm > 0.0f)
        m_PlayerInseamCm = std::clamp(m_PlayerInseamCm, 45.0f, 130.0f);

    bool measurePlayerHeight = false;
    parseOrDefault("MeasurePlayerHeight", measurePlayerHeight, false);
    if (measurePlayerHeight)
        m_MeasureHeightRequested.store(true, std::memory_order_release);

    std::string beltOffsetStr = "auto";
    parseOrDefault("BeltHeightOffsetCm", beltOffsetStr, std::string("auto"));
    const float parsedBeltOffsetCm = ParseLengthToCm(beltOffsetStr, 0.0f, false);
    if (parsedBeltOffsetCm <= 0.0f)
    {
        m_BeltHeightOffsetAuto = true;
        m_BeltHeightOffsetCm = 82.0f;
    }
    else
    {
        m_BeltHeightOffsetAuto = false;
        m_BeltHeightOffsetCm = std::clamp(parsedBeltOffsetCm, 10.0f, 130.0f);
    }
    RecomputeEffectiveBeltHeight();
    parseOrDefault("ShowBatteryCounts", m_ShowBatteryCounts, true);
    parseOrDefault("AllowQuickReload", m_AllowQuickReload, false);
    parseOrDefault("ShowFlashlightBatteryRing", m_ShowFlashlightBatteryRing, true);
    parseOrDefault("CameraFlashBlindingStrength", m_CameraFlashBlindingStrength, 1.0f);
    if (!std::isfinite(m_CameraFlashBlindingStrength))
        m_CameraFlashBlindingStrength = 1.0f;
    m_CameraFlashBlindingStrength = std::clamp(m_CameraFlashBlindingStrength, 0.0f, 2.0f);

    parseOrDefault("DesktopWindowWidth", m_DesktopWindowWidth, 1280);
    parseOrDefault("DesktopWindowHeight", m_DesktopWindowHeight, 720);

    parseXYZOrDefault("RightHandPosOffset", m_RightHandPosOffset, 0.0f, 0.0f, 0.0f);
    parseXYZOrDefault("RightHandAngOffset", m_RightHandAngOffset, 0.0f, 0.0f, 0.0f);
    parseXYZOrDefault("LeftHandPosOffset",  m_LeftHandPosOffset,  0.0f, 0.0f, 0.0f);
    parseXYZOrDefault("LeftHandAngOffset",  m_LeftHandAngOffset,  0.0f, 0.0f, 0.0f);

    parseXYZOrDefault("ViewmodelPosCustomOffset", m_ViewmodelPosCustomOffset, 0.0f, 0.0f, 0.0f);
    parseXYZOrDefault("ViewmodelAngCustomOffset", m_ViewmodelAngCustomOffset, 0.0f, 0.0f, 0.0f);
}

static std::filesystem::file_time_type s_LastSavedConfigTime {};

void VR::SaveConfigFile()
{
    std::lock_guard<std::mutex> lock(m_ConfigMutex);

    const std::string configPath = ResolveGameRelativePath("VR\\config.txt");
    const std::string tmpPath = configPath + ".tmp";
    std::ofstream out(tmpPath, std::ios::trunc);
    if (!out.is_open())
        return;

    auto isZeroVec = [](const auto& v) {
        return fabsf(v.x) < 1e-4f && fabsf(v.y) < 1e-4f && fabsf(v.z) < 1e-4f;
    };

    out << "TurnSpeed=" << m_TurnSpeed << "\n";
    out << "SnapTurning=" << (m_SnapTurning ? "true" : "false") << "\n";
    out << "SnapTurnAngle=" << m_SnapTurnAngle << "\n";
    out << "LeftHanded=" << (m_LeftHanded ? "true" : "false") << "\n";
    out << "VRScale=" << m_VRScale << " # Real world units to source units scale\n";
    out << "IPDScale=" << m_IpdScale << " # Scale of interpupillary distance\n";
    out << "6DOF=" << (m_6DOF ? "true" : "false") << "\n";
    out << "AimMode=" << m_AimMode << " # 0 = None, 1 = Crosshair, 2 = Context/Menu cursor\n";
    out << "AntiAliasing=" << m_AntiAliasingConfig << " # 0, 2, 4, 8 (applies after restart)\n";
    out << "RenderScale=" << m_RenderScale << " # VR eye resolution multiplier (0.5..2.0, default 1.0)\n";
    out << "RenderWindow=" << m_RenderWindow << " # 0 = no desktop mirror (faster), 1 = mirror left eye to desktop window\n";
    out << "ShowHands=" << (m_ShowHands ? "true" : "false") << " # VR hands gripping camera and flashlight\n";
    out << "UseTipDistance=" << m_UseTipDistanceCm << " # Virtual fingertip distance in front of the controller for selecting switches (cm)\n";
    out << "UseTouchRadius=" << m_UseTouchRadiusCm << " # Max distance fingertip -> object for close-range selection (cm)\n";
    out << "MenuSeatHeight=" << m_MenuSeatHeightCm << " # Main menu eye height above the laptop camera (cm)\n";
    out << "MenuSeatForward=" << m_MenuSeatForwardCm << " # Main menu seat distance moved towards the laptop (cm)\n";
    out << "IntroSeatHeight=" << m_IntroSeatHeightCm << " # Chapter 1 intro chair eye height offset (cm)\n";
    out << "CarSeatHeight=" << m_CarSeatHeightCm << " # Chapter 2 intro car eye height offset (cm)\n";
    out << "MultiThreadedRendering=" << (m_MultiThreadedRendering ? "true" : "false") << " # Use mat_queue_mode 2 multi-threaded rendering for lower CPU frametimes\n";
    out << "KeepActiveInBackground=" << (m_KeepActiveInBackground ? "true" : "false") << " # No stutter when another window has focus\n";
    out << "VRAMOffload=" << (m_VRAMOffload ? "true" : "false") << " # Offload textures from 32-bit RAM to GPU VRAM\n";
    out << "DebugFingerLog=" << (m_DebugFingerLog ? "true" : "false") << " # Diagnostics only (writes VR\\finger_debug.txt when enabled)\n";
    out << "EnableProfilerHotkey=" << (m_EnableProfilerHotkey ? "true" : "false") << " # F12 CPU frame profiler hotkey\n";
    out << "SpatialAudioMode=" << m_SpatialAudioMode << " # steam_audio (3D HRTF + height + occlusion), dsoal (OpenAL Soft 5.1 HRTF, restart required), native (Source headphones)\n";
    out << "SpatialAudioOcclusion=" << (m_SpatialAudioOcclusion ? "true" : "false") << " # Raycast wall occlusion for steam_audio mode\n";
    out << "SpatialAudioRoomSend=" << m_SpatialAudioRoomSend << " # Source room reverb send multiplier (0.0..1.5) in steam_audio mode\n";
    out << "BatteryInventoryMode=" << BatteryInventoryModeToString(m_BatteryInventoryMode) << " # belt, wrist, both, off\n";
    out << "BeltHolsterEnabled=" << (m_BeltHolsterEnabled ? "true" : "false") << " # Holster/draw flashlight & camera on belt\n";
    if (m_PlayerHeightCm > 0.0f)
    {
        out << "PlayerHeight=" << FormatCmCompact(m_PlayerHeightCm) << "cm # ("
            << FormatInchesCompact(m_PlayerHeightCm) << "in) Standing body height";
        if (m_MeasuredEyeHeightCm > 0.0f)
        {
            out << " | Measured standing eye height: " << FormatCmCompact(m_MeasuredEyeHeightCm)
                << "cm (" << FormatInchesCompact(m_MeasuredEyeHeightCm) << "in)";
        }
        out << "\n";
    }
    else
    {
        out << "PlayerHeight=0 # Standing height (e.g. 175cm, 69in, 5'9\") or hold controller at belt + Trigger in Calibration Mode (L3+R3 0.8s)\n";
    }
    if (m_PlayerInseamCm > 0.0f)
    {
        out << "PlayerInseam=" << FormatCmCompact(m_PlayerInseamCm) << "cm # ("
            << FormatInchesCompact(m_PlayerInseamCm) << "in) Optional inner leg length / inseam\n";
    }
    else
    {
        out << "PlayerInseam=0 # Optional inner leg length / inseam (e.g. 78cm or 30.7in)\n";
    }
    out << "MeasurePlayerHeight=false # Set true while standing upright to measure your height from SteamVR\n";
    if (m_BeltHeightOffsetAuto)
    {
        const float effCm = GetEffectiveBeltHeightOffsetCm();
        out << "BeltHeightOffsetCm=auto # Effective: " << FormatCmCompact(effCm) << "cm ("
            << FormatInchesCompact(effCm) << "in) below eyes (default 82cm; or set explicit e.g. 84cm / 33.1in)\n";
    }
    else
    {
        out << "BeltHeightOffsetCm=" << FormatCmCompact(m_BeltHeightOffsetCm)
            << " # Manual override in cm (+1.5cm above Trigger-calibrated hand; or set 'auto')\n";
    }
    out << "ShowBatteryCounts=" << (m_ShowBatteryCounts ? "true" : "false") << " # Show reserve battery counts at belt/wrist slots\n";
    out << "AllowQuickReload=" << (m_AllowQuickReload ? "true" : "false") << " # Allow touchpad swipe-down / button-hold quick reload\n";
    out << "ShowFlashlightBatteryRing=" << (m_ShowFlashlightBatteryRing ? "true" : "false") << " # LED battery ring on flashlight tail cap\n";
    out << "CameraFlashBlindingStrength=" << m_CameraFlashBlindingStrength << " # Camera flash self-blinding glare multiplier (0.0..2.0)\n";
    out << "DesktopWindowWidth=" << m_DesktopWindowWidth << " # Desktop mirror window size (640..3840 x 480..2160)\n";
    out << "DesktopWindowHeight=" << m_DesktopWindowHeight << "\n";
    out << "CalibrationMode=false # Hold both stick clicks 0.8s in-game to toggle\n";
    if (!isZeroVec(m_RightHandPosOffset))
    {
        out << "RightHandPosOffsetX=" << m_RightHandPosOffset.x << " # Camera Forward(+) / Back(-) in cm\n";
        out << "RightHandPosOffsetY=" << m_RightHandPosOffset.y << " # Camera Right(+) / Left(-) in cm\n";
        out << "RightHandPosOffsetZ=" << m_RightHandPosOffset.z << " # Camera Up(+) / Down(-) in cm\n";
    }
    if (!isZeroVec(m_RightHandAngOffset))
    {
        out << "RightHandAngOffsetX=" << m_RightHandAngOffset.x << " # Camera Pitch Forward(+) / Back(-) in deg\n";
        out << "RightHandAngOffsetY=" << m_RightHandAngOffset.y << " # Camera Yaw in deg\n";
        out << "RightHandAngOffsetZ=" << m_RightHandAngOffset.z << " # Camera Roll in deg\n";
    }
    if (!isZeroVec(m_LeftHandPosOffset))
    {
        out << "LeftHandPosOffsetX=" << m_LeftHandPosOffset.x << " # Flashlight Forward(+) / Back(-) in cm\n";
        out << "LeftHandPosOffsetY=" << m_LeftHandPosOffset.y << " # Flashlight Right(+) / Left(-) in cm\n";
        out << "LeftHandPosOffsetZ=" << m_LeftHandPosOffset.z << " # Flashlight Up(+) / Down(-) in cm\n";
    }
    if (!isZeroVec(m_LeftHandAngOffset))
    {
        out << "LeftHandAngOffsetX=" << m_LeftHandAngOffset.x << " # Flashlight Pitch Forward(+) / Back(-) in deg\n";
        out << "LeftHandAngOffsetY=" << m_LeftHandAngOffset.y << " # Flashlight Yaw in deg\n";
        out << "LeftHandAngOffsetZ=" << m_LeftHandAngOffset.z << " # Flashlight Roll in deg\n";
    }
    if (!isZeroVec(m_ViewmodelPosCustomOffset))
    {
        out << "ViewmodelPosCustomOffsetX=" << m_ViewmodelPosCustomOffset.x << "\n";
        out << "ViewmodelPosCustomOffsetY=" << m_ViewmodelPosCustomOffset.y << "\n";
        out << "ViewmodelPosCustomOffsetZ=" << m_ViewmodelPosCustomOffset.z << "\n";
    }
    if (!isZeroVec(m_ViewmodelAngCustomOffset))
    {
        out << "ViewmodelAngCustomOffsetX=" << m_ViewmodelAngCustomOffset.x << "\n";
        out << "ViewmodelAngCustomOffsetY=" << m_ViewmodelAngCustomOffset.y << "\n";
        out << "ViewmodelAngCustomOffsetZ=" << m_ViewmodelAngCustomOffset.z << "\n";
    }
    out.close();

    MoveFileExA(tmpPath.c_str(), configPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);

    try
    {
        s_LastSavedConfigTime = std::filesystem::last_write_time(configPath);
    }
    catch (...) {}
}

void VR::WaitForConfigUpdate()
{
    const std::string configDir = ResolveGameRelativePath("VR\\");
    const std::string configPath = ResolveGameRelativePath("VR\\config.txt");
    HANDLE fileChangeHandle = FindFirstChangeNotificationA(configDir.c_str(), false, FILE_NOTIFY_CHANGE_LAST_WRITE);
    if (fileChangeHandle == INVALID_HANDLE_VALUE)
        return;

    std::filesystem::file_time_type configLastModified{};
    while (!m_ConfigStopRequested)
    {
        DWORD waitRes = WaitForSingleObject(fileChangeHandle, 250);
        if (m_ConfigStopRequested)
            break;

        if (waitRes == WAIT_OBJECT_0)
        {
            try 
            {
                if (std::filesystem::exists(configPath))
                {
                    int retryCount = 0;
                    while (retryCount < 5 && std::filesystem::file_size(configPath) < 50)
                    {
                        if (m_ConfigStopRequested) break;
                        Sleep(50);
                        retryCount++;
                    }

                    auto configModifiedTime = std::filesystem::last_write_time(configPath);
                    std::filesystem::file_time_type savedTime{};
                    {
                        std::lock_guard<std::mutex> lock(m_ConfigMutex);
                        savedTime = s_LastSavedConfigTime;
                    }
                    if (configModifiedTime != configLastModified && configModifiedTime != savedTime)
                    {
                        configLastModified = configModifiedTime;
                        ParseConfigFile();
                        
#ifdef _DEBUG
                        std::cout << "Successfully reloaded 'config.txt'\n";
#endif
                    }
                }
            }
            catch (const std::invalid_argument &e)
            {
                concatErrorMsg(
                    *m_Game, "Failed to parse 'config.txt' (", e.what(), ")");
            }
            catch (const std::filesystem::filesystem_error &)
            {
                // File locked or inaccessible temporarily
            }
            
            FindNextChangeNotification(fileChangeHandle);
        }
    }

    FindCloseChangeNotification(fileChangeHandle);
}

// ---------------------------------------------------------------------------
// Background play: keep INFRA "active" when another window has focus, so the
// engine does not throttle/stutter, while not stealing the desktop mouse.
// ---------------------------------------------------------------------------
static HWND    s_GameHwnd = nullptr;
static WNDPROC s_OrigGameWndProc = nullptr;
typedef BOOL(WINAPI* tSetCursorPos)(int, int);
typedef BOOL(WINAPI* tClipCursor)(const RECT*);
static tSetCursorPos s_OrigSetCursorPos = nullptr;
static tClipCursor   s_OrigClipCursor = nullptr;
static LPVOID        s_TargetSetCursorPos = nullptr;
static LPVOID        s_TargetClipCursor = nullptr;
static LPVOID        s_TargetVPanelIsVisible = nullptr;

static bool GameIsForeground()
{
    return s_GameHwnd && GetForegroundWindow() == s_GameHwnd;
}

static BOOL WINAPI HookedSetCursorPos(int x, int y)
{
    if (s_GameHwnd && !GameIsForeground())
        return TRUE; // don't warp the desktop mouse while you're using another window
    return s_OrigSetCursorPos(x, y);
}

static BOOL WINAPI HookedClipCursor(const RECT* r)
{
    if (r && s_GameHwnd && !GameIsForeground())
        return s_OrigClipCursor(nullptr);
    return s_OrigClipCursor(r);
}

static bool s_KeepActive = true;
static int s_DesktopWindowW = 1280;
static int s_DesktopWindowH = 720;

typedef bool(__thiscall* tVPanelWrapperIsVisible)(void* thisptr, unsigned int vguiPanel);
static tVPanelWrapperIsVisible s_OrigVPanelIsVisible = nullptr;

static bool __fastcall HookedVPanelIsVisible(void* ecx, void* edx, unsigned int vguiPanel)
{
    if (!vguiPanel)
        return false;
    return s_OrigVPanelIsVisible(ecx, vguiPanel);
}

static LRESULT CALLBACK BackgroundGameWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (!s_KeepActive) return CallWindowProc(s_OrigGameWndProc, hwnd, msg, wParam, lParam);
    if (msg == WM_ACTIVATEAPP && wParam == FALSE)
        return 0; // swallow: engine stays "active app" (no throttle, no audio mute)
    if (msg == WM_ACTIVATE && LOWORD(wParam) == WA_INACTIVE)
        return 0;
    if (msg == WM_KILLFOCUS)
    {
        if (s_OrigClipCursor)
            s_OrigClipCursor(nullptr);
        return 0;
    }
    return CallWindowProc(s_OrigGameWndProc, hwnd, msg, wParam, lParam);
}

static BOOL CALLBACK FindGameWindowProc(HWND hwnd, LPARAM lParam)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER))
        return TRUE;
    char cls[64] = {};
    GetClassNameA(hwnd, cls, sizeof(cls));
    if (strcmp(cls, "Valve001") == 0)
    {
        *(HWND*)lParam = hwnd;
        return FALSE;
    }
    return TRUE;
}

void VR::InstallBackgroundFocus()
{
    s_KeepActive = m_KeepActiveInBackground;
    s_DesktopWindowW = m_DesktopWindowWidth;
    s_DesktopWindowH = m_DesktopWindowHeight;
    if (s_OrigGameWndProc)
    {
        static uint32_t s_mainWinCheck = 0;
        if (s_GameHwnd && ((s_mainWinCheck++ & 63u) == 0u))
            EnforceVRWindowedMode(s_GameHwnd);
        return;
    }
    HWND hwnd = nullptr;
    EnumWindows(FindGameWindowProc, (LPARAM)&hwnd);
    if (!hwnd)
        return;
    s_GameHwnd = hwnd;
    if (!s_OrigSetCursorPos)
    {
        LPVOID origSetCursorPos = nullptr;
        LPVOID targetSetCursorPos = nullptr;
        if (MH_CreateHookApiEx(L"user32", "SetCursorPos", &HookedSetCursorPos, &origSetCursorPos, &targetSetCursorPos) == MH_OK && origSetCursorPos && targetSetCursorPos)
        {
            s_OrigSetCursorPos = (tSetCursorPos)origSetCursorPos;
            s_TargetSetCursorPos = targetSetCursorPos;
            MH_EnableHook(targetSetCursorPos);
        }
        LPVOID origClipCursor = nullptr;
        LPVOID targetClipCursor = nullptr;
        if (MH_CreateHookApiEx(L"user32", "ClipCursor", &HookedClipCursor, &origClipCursor, &targetClipCursor) == MH_OK && origClipCursor && targetClipCursor)
        {
            s_OrigClipCursor = (tClipCursor)origClipCursor;
            s_TargetClipCursor = targetClipCursor;
            MH_EnableHook(targetClipCursor);
        }
    }
    if (!s_OrigVPanelIsVisible)
    {
        HMODULE hVgui2 = GetModuleHandleA("vgui2.dll");
        if (hVgui2)
        {
            LPVOID pIsVisible = (LPVOID)((uintptr_t)hVgui2 + 0x17C40);
            LPVOID origIsVisible = nullptr;
            if (MH_CreateHook(pIsVisible, &HookedVPanelIsVisible, &origIsVisible) == MH_OK && origIsVisible)
            {
                s_OrigVPanelIsVisible = (tVPanelWrapperIsVisible)origIsVisible;
                s_TargetVPanelIsVisible = pIsVisible;
                MH_EnableHook(pIsVisible);
            }
        }
    }
    if (!s_OrigClipCursor)
        s_OrigClipCursor = ::ClipCursor;
    if (!s_OrigSetCursorPos)
        s_OrigSetCursorPos = ::SetCursorPos;
    s_OrigGameWndProc = (WNDPROC)SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)BackgroundGameWndProc);
    std::cout << "Background focus keeper installed\n";
}

void VR::UninstallBackgroundFocus()
{
    if (s_OrigGameWndProc && s_GameHwnd)
    {
        SetWindowLongPtrA((HWND)s_GameHwnd, GWLP_WNDPROC, (LONG_PTR)s_OrigGameWndProc);
        s_OrigGameWndProc = nullptr;
    }

    if (s_TargetSetCursorPos)
    {
        MH_DisableHook(s_TargetSetCursorPos);
        MH_RemoveHook(s_TargetSetCursorPos);
        s_TargetSetCursorPos = nullptr;
    }
    if (s_TargetClipCursor)
    {
        MH_DisableHook(s_TargetClipCursor);
        MH_RemoveHook(s_TargetClipCursor);
        s_TargetClipCursor = nullptr;
    }
    if (s_TargetVPanelIsVisible)
    {
        MH_DisableHook(s_TargetVPanelIsVisible);
        MH_RemoveHook(s_TargetVPanelIsVisible);
        s_TargetVPanelIsVisible = nullptr;
    }
    s_OrigSetCursorPos = nullptr;
    s_OrigClipCursor = nullptr;
    s_OrigVPanelIsVisible = nullptr;
    s_GameHwnd = nullptr;
}

void* VR::GetGameHwndRaw() const
{
    return (void*)s_GameHwnd;
}

bool VR::IsGameForeground() const
{
    return !s_GameHwnd || GameIsForeground();
}

// ============================================================================
// 3D Multi-Layer Startup Splash (OpenVR Overlays while menu_main.bsp loads)
// ============================================================================
static float SplashEaseOutCubic(float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    float inv = 1.0f - t;
    return 1.0f - inv * inv * inv;
}

// Lightweight VPK v1/v2 reader so we can play original INFRA PCM WAVs straight from the
// player's local infra\pak01_dir.vpk without shipping any copyrighted audio files.
static bool ReadFileFromInfraVpk(const std::string& dirVpkPath, const char* targetExt, const char* targetDir, const char* targetName, std::vector<uint8_t>& outBytes)
{
    outBytes.clear();
    std::ifstream f(dirVpkPath, std::ios::binary);
    if (!f)
        return false;

    uint32_t signature = 0, version = 0, treeSize = 0;
    f.read(reinterpret_cast<char*>(&signature), 4);
    f.read(reinterpret_cast<char*>(&version), 4);
    f.read(reinterpret_cast<char*>(&treeSize), 4);
    if (!f || signature != 0x55aa1234u || (version != 1u && version != 2u))
        return false;

    uint32_t headerSize = (version == 2u) ? 28u : 12u;
    if (version == 2u)
        f.seekg(16, std::ios::cur); // skip v2 section sizes

    std::vector<uint8_t> tree(treeSize);
    f.read(reinterpret_cast<char*>(tree.data()), treeSize);
    if (!f)
        return false;

    size_t pos = 0;
    auto readStr = [&](std::string& s) -> bool {
        s.clear();
        while (pos < tree.size())
        {
            char c = static_cast<char>(tree[pos++]);
            if (c == '\0')
                return true;
            s.push_back(c);
        }
        return false;
    };

    std::string ext, dir, name;
    while (readStr(ext) && !ext.empty())
    {
        while (readStr(dir) && !dir.empty())
        {
            while (readStr(name) && !name.empty())
            {
                if (pos + 18 > tree.size())
                    return false;
                uint16_t preloadBytes = 0, archiveIndex = 0;
                uint32_t entryOffset = 0, entryLength = 0;
                memcpy(&preloadBytes, &tree[pos + 4], 2);
                memcpy(&archiveIndex, &tree[pos + 6], 2);
                memcpy(&entryOffset, &tree[pos + 8], 4);
                memcpy(&entryLength, &tree[pos + 12], 4);
                pos += 18;
                size_t preloadPos = pos;
                pos += preloadBytes;
                if (pos > tree.size())
                    return false;

                if (_stricmp(ext.c_str(), targetExt) == 0 &&
                    _stricmp(dir.c_str(), targetDir) == 0 &&
                    _stricmp(name.c_str(), targetName) == 0)
                {
                    outBytes.resize(static_cast<size_t>(preloadBytes) + static_cast<size_t>(entryLength));
                    if (preloadBytes > 0)
                        memcpy(outBytes.data(), &tree[preloadPos], preloadBytes);
                    if (entryLength > 0)
                    {
                        if (archiveIndex == 0x7fff)
                        {
                            f.seekg(static_cast<std::streamoff>(headerSize + treeSize + entryOffset), std::ios::beg);
                            f.read(reinterpret_cast<char*>(outBytes.data() + preloadBytes), entryLength);
                            if (!f) { outBytes.clear(); return false; }
                        }
                        else
                        {
                            // Replace "_dir.vpk" with "_NNN.vpk"
                            size_t dirSuffix = dirVpkPath.rfind("_dir.vpk");
                            if (dirSuffix == std::string::npos) { outBytes.clear(); return false; }
                            char chunkSuffix[16];
                            snprintf(chunkSuffix, sizeof(chunkSuffix), "_%03u.vpk", static_cast<unsigned>(archiveIndex));
                            std::string chunkPath = dirVpkPath.substr(0, dirSuffix) + chunkSuffix;
                            std::ifstream cf(chunkPath, std::ios::binary);
                            if (!cf) { outBytes.clear(); return false; }
                            cf.seekg(static_cast<std::streamoff>(entryOffset), std::ios::beg);
                            cf.read(reinterpret_cast<char*>(outBytes.data() + preloadBytes), entryLength);
                            if (!cf) { outBytes.clear(); return false; }
                        }
                    }
                    return !outBytes.empty();
                }
            }
        }
    }
    return false;
}

// Play an uncompressed 16-bit PCM WAV directly through infra.exe's own Windows audio session
// (waveOutOpen -> WAVE_MAPPER) with optional gain, duration trim, and smooth fade-in/fade-out envelope.
static void PlayPcmWavBytesAsync(
    const std::vector<uint8_t>& wavBytes,
    const std::atomic<bool>& stopFlag,
    float gain = 1.0f,
    float maxDurationSec = 0.0f,
    float fadeInSec = 0.0f,
    float fadeOutSec = 0.0f)
{
    if (wavBytes.size() < 44 || memcmp(wavBytes.data(), "RIFF", 4) != 0 || memcmp(wavBytes.data() + 8, "WAVE", 4) != 0)
        return;

    WAVEFORMATEX wfx{};
    bool hasFmt = false;
    const uint8_t* pcmData = nullptr;
    uint32_t pcmSize = 0;

    size_t pos = 12;
    while (pos + 8 <= wavBytes.size())
    {
        uint32_t chunkSize = 0;
        memcpy(&chunkSize, wavBytes.data() + pos + 4, 4);
        if (pos + 8 + chunkSize > wavBytes.size())
            break;

        if (memcmp(wavBytes.data() + pos, "fmt ", 4) == 0 && chunkSize >= 16)
        {
            memcpy(&wfx, wavBytes.data() + pos + 8, std::min<size_t>(chunkSize, sizeof(WAVEFORMATEX)));
            wfx.cbSize = 0;
            hasFmt = (wfx.wFormatTag == WAVE_FORMAT_PCM);
        }
        else if (memcmp(wavBytes.data() + pos, "data", 4) == 0)
        {
            pcmData = wavBytes.data() + pos + 8;
            pcmSize = chunkSize;
        }
        pos += 8 + chunkSize + (chunkSize & 1u);
    }

    if (!hasFmt || !pcmData || pcmSize == 0 || wfx.nBlockAlign == 0)
        return;

    uint32_t useBytes = pcmSize;
    if (maxDurationSec > 0.0f && wfx.nSamplesPerSec > 0)
    {
        const uint32_t maxFrames = static_cast<uint32_t>(maxDurationSec * static_cast<float>(wfx.nSamplesPerSec));
        const uint32_t maxBytes = maxFrames * wfx.nBlockAlign;
        if (maxBytes < useBytes)
            useBytes = maxBytes;
    }

    std::vector<uint8_t> pcmCopy(pcmData, pcmData + useBytes);

    // Apply gain + fade-in/fade-out envelope cleanly on 16-bit PCM samples
    if (wfx.wBitsPerSample == 16 && (gain != 1.0f || fadeInSec > 0.0f || fadeOutSec > 0.0f))
    {
        int16_t* samples = reinterpret_cast<int16_t*>(pcmCopy.data());
        const size_t numChannels = std::max<size_t>(1, wfx.nChannels);
        const size_t totalFrames = pcmCopy.size() / wfx.nBlockAlign;
        const float sr = static_cast<float>(std::max<DWORD>(1, wfx.nSamplesPerSec));
        const size_t fadeInFrames = static_cast<size_t>(fadeInSec * sr);
        const size_t fadeOutFrames = static_cast<size_t>(fadeOutSec * sr);

        for (size_t fi = 0; fi < totalFrames; ++fi)
        {
            float env = gain;
            if (fadeInFrames > 0 && fi < fadeInFrames)
                env *= static_cast<float>(fi) / static_cast<float>(fadeInFrames);
            if (fadeOutFrames > 0 && fi + fadeOutFrames > totalFrames)
                env *= static_cast<float>(totalFrames - fi) / static_cast<float>(fadeOutFrames);

            for (size_t c = 0; c < numChannels; ++c)
            {
                const size_t idx = fi * numChannels + c;
                float v = static_cast<float>(samples[idx]) * env;
                samples[idx] = static_cast<int16_t>(std::clamp(v, -32768.0f, 32767.0f));
            }
        }
    }

    std::thread([wfx, pcm = std::move(pcmCopy), &stopFlag]() mutable {
        HWAVEOUT hWaveOut = nullptr;
        if (waveOutOpen(&hWaveOut, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR || !hWaveOut)
            return;

        WAVEHDR hdr{};
        hdr.lpData = reinterpret_cast<LPSTR>(pcm.data());
        hdr.dwBufferLength = static_cast<DWORD>(pcm.size());
        if (waveOutPrepareHeader(hWaveOut, &hdr, sizeof(hdr)) == MMSYSERR_NOERROR)
        {
            if (waveOutWrite(hWaveOut, &hdr, sizeof(hdr)) == MMSYSERR_NOERROR)
            {
                while ((hdr.dwFlags & WHDR_DONE) == 0 && !stopFlag.load())
                    std::this_thread::sleep_for(std::chrono::milliseconds(15));
                if (stopFlag.load())
                    waveOutReset(hWaveOut);
            }
            waveOutUnprepareHeader(hWaveOut, &hdr, sizeof(hdr));
        }
        waveOutClose(hWaveOut);
    }).detach();
}

// Keep startup WAV buffers alive for the splash session
static std::vector<uint8_t> s_StartupAmbienceWav;
static std::vector<uint8_t> s_StartupFocusWav;
static std::vector<uint8_t> s_StartupSnapWav;
static std::vector<uint8_t> s_StartupGagWav;

// ---- Native GPU 360-degree Cubemap + Stereo 3D Startup Intro (direct D3D9 into eye targets) ----
namespace
{
    // 0..5: Left/Mono sky (ft,rt,bk,lf,up,dn) | 6..11: Right sky (stereo 3D) | 12: logo_infra | 13: logo_vr | 14: radial flash
    constexpr int kSlotLogoInfra   = 12;
    constexpr int kSlotLogoVr      = 13;
    constexpr int kSlotFlash       = 14;
    constexpr int kSplashFileCount = 14;
    constexpr int kSplashTexCount  = 15;

    struct SplashVtx
    {
        float x, y, z;
        float u, v;
    };
    constexpr DWORD kSplashFvf = D3DFVF_XYZ | D3DFVF_TEX1;

    std::string              s_SplashFilePaths[kSplashFileCount];
    std::mutex               s_TexMutex;
    std::condition_variable  s_TexCv;
    std::thread              s_TexLoaderThread;
    bool                     s_TexLoaderStop = false;
    bool                     s_TexLoaderDone = false;
    int                      s_StagingSlot = -1;
    uint32_t                 s_StagingW = 0;
    uint32_t                 s_StagingH = 0;
    std::vector<uint8_t>     s_StagingPixels;

    std::mutex               s_GpuResMutex;
    IDirect3DTexture9*       s_GpuTex[kSplashTexCount] = {};
    IDirect3DStateBlock9*    s_SplashStateBlock = nullptr;
    bool                     s_AllGpuTexturesReady = false;
    bool                     s_HasAllSixSky = false;
    bool                     s_HasStereoSky = false;

    bool DecodeImageWicBgra(IWICImagingFactory* factory, const std::string& path,
                            std::vector<uint8_t>& outBgra, uint32_t& outW, uint32_t& outH)
    {
        if (!factory || path.empty())
            return false;

        int wlen = MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, nullptr, 0);
        if (wlen <= 1)
            return false;
        std::wstring wpath(static_cast<size_t>(wlen), L'\0');
        MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, wpath.data(), wlen);

        IWICBitmapDecoder* decoder = nullptr;
        if (FAILED(factory->CreateDecoderFromFilename(wpath.c_str(), nullptr, GENERIC_READ,
                                                      WICDecodeMetadataCacheOnDemand, &decoder)) || !decoder)
            return false;

        IWICBitmapFrameDecode* frame = nullptr;
        IWICFormatConverter* conv = nullptr;
        bool ok = false;

        if (SUCCEEDED(decoder->GetFrame(0, &frame)) && frame)
        {
            UINT w = 0, h = 0;
            if (SUCCEEDED(frame->GetSize(&w, &h)) && w > 0 && h > 0 && w <= 8192 && h <= 8192)
            {
                if (SUCCEEDED(factory->CreateFormatConverter(&conv)) && conv &&
                    SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA,
                                               WICBitmapDitherTypeNone, nullptr, 0.0,
                                               WICBitmapPaletteTypeCustom)))
                {
                    const UINT stride = w * 4;
                    const size_t totalBytes = static_cast<size_t>(stride) * static_cast<size_t>(h);
                    outBgra.resize(totalBytes);
                    if (SUCCEEDED(conv->CopyPixels(nullptr, stride, static_cast<UINT>(totalBytes), outBgra.data())))
                    {
                        outW = static_cast<uint32_t>(w);
                        outH = static_cast<uint32_t>(h);
                        ok = true;
                    }
                    else
                    {
                        outBgra.clear();
                    }
                }
            }
        }

        if (conv) conv->Release();
        if (frame) frame->Release();
        decoder->Release();
        return ok;
    }

    bool UploadSurfaceLevel(IDirect3DDevice9* dev, IDirect3DTexture9* tex, UINT level,
                            const uint8_t* bgra, uint32_t w, uint32_t h)
    {
        IDirect3DSurface9* sysSurf = nullptr;
        if (FAILED(dev->CreateOffscreenPlainSurface(w, h, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sysSurf, nullptr)) || !sysSurf)
            return false;

        D3DLOCKED_RECT lr{};
        bool ok = false;
        if (SUCCEEDED(sysSurf->LockRect(&lr, nullptr, 0)))
        {
            const size_t rowBytes = static_cast<size_t>(w) * 4;
            uint8_t* dst = static_cast<uint8_t*>(lr.pBits);
            for (uint32_t y = 0; y < h; ++y)
            {
                memcpy(dst + static_cast<size_t>(y) * static_cast<size_t>(lr.Pitch),
                       bgra + static_cast<size_t>(y) * rowBytes, rowBytes);
            }
            sysSurf->UnlockRect();

            IDirect3DSurface9* dstSurf = nullptr;
            if (SUCCEEDED(tex->GetSurfaceLevel(level, &dstSurf)) && dstSurf)
            {
                ok = SUCCEEDED(dev->UpdateSurface(sysSurf, nullptr, dstSurf, nullptr));
                dstSurf->Release();
            }
        }
        sysSurf->Release();
        return ok;
    }

    IDirect3DTexture9* UploadBgraToGpuTexture(IDirect3DDevice9* dev, const uint8_t* bgra, uint32_t w, uint32_t h,
                                              bool generateMips = false)
    {
        if (!dev || !bgra || w == 0 || h == 0)
            return nullptr;

        // Full mip chain for pow2 4K cubemap faces; 4 mip levels for 3840x2160 16:9 stereo window
        // (eliminates sub-pixel moire on fine steel railings while keeping 100% native 4K sharpness!)
        const bool isPow2 = ((w & (w - 1)) == 0) && ((h & (h - 1)) == 0);
        uint32_t mipCount = 1;
        if (generateMips)
        {
            uint32_t mw = w, mh = h;
            const uint32_t maxLevels = isPow2 ? 16u : 6u;
            while ((mw > 1 || mh > 1) && mipCount < maxLevels)
            {
                if (!isPow2 && ((mw & 1u) != 0 || (mh & 1u) != 0))
                    break;
                mw = std::max(1u, mw >> 1);
                mh = std::max(1u, mh >> 1);
                ++mipCount;
            }
        }

        IDirect3DTexture9* tex = nullptr;
        if (FAILED(dev->CreateTexture(w, h, mipCount, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex, nullptr)) || !tex)
            return nullptr;

        if (!UploadSurfaceLevel(dev, tex, 0, bgra, w, h))
        {
            tex->Release();
            return nullptr;
        }

        if (mipCount > 1)
        {
            const uint8_t* prevPtr = bgra;
            uint32_t pw = w, ph = h;
            std::vector<uint8_t> curBuf;
            std::vector<uint8_t> prevBuf;
            for (UINT lvl = 1; lvl < mipCount; ++lvl)
            {
                const uint32_t nw = std::max(1u, pw >> 1);
                const uint32_t nh = std::max(1u, ph >> 1);
                curBuf.resize(static_cast<size_t>(nw) * static_cast<size_t>(nh) * 4);
                for (uint32_t y = 0; y < nh; ++y)
                {
                    const uint32_t sy0 = std::min(y * 2, ph - 1);
                    const uint32_t sy1 = std::min(sy0 + 1, ph - 1);
                    const uint8_t* row0 = prevPtr + static_cast<size_t>(sy0) * pw * 4;
                    const uint8_t* row1 = prevPtr + static_cast<size_t>(sy1) * pw * 4;
                    uint8_t* outRow = curBuf.data() + static_cast<size_t>(y) * nw * 4;
                    for (uint32_t x = 0; x < nw; ++x)
                    {
                        const uint32_t sx0 = std::min(x * 2, pw - 1) * 4;
                        const uint32_t sx1 = std::min(x * 2 + 1, pw - 1) * 4;
                        for (int c = 0; c < 4; ++c)
                        {
                            const uint32_t sum =
                                static_cast<uint32_t>(row0[sx0 + c]) +
                                static_cast<uint32_t>(row0[sx1 + c]) +
                                static_cast<uint32_t>(row1[sx0 + c]) +
                                static_cast<uint32_t>(row1[sx1 + c]) + 2u;
                            outRow[x * 4 + c] = static_cast<uint8_t>(sum >> 2);
                        }
                    }
                }
                if (!UploadSurfaceLevel(dev, tex, lvl, curBuf.data(), nw, nh))
                    break;
                prevBuf.swap(curBuf);
                prevPtr = prevBuf.data();
                pw = nw;
                ph = nh;
            }
        }
        return tex;
    }

    IDirect3DTexture9* CreateRadialFlashGpuTexture(IDirect3DDevice9* dev)
    {
        constexpr uint32_t kFlashW = 128;
        constexpr uint32_t kFlashH = 72;
        std::vector<uint8_t> buf(static_cast<size_t>(kFlashW) * kFlashH * 4);
        for (uint32_t y = 0; y < kFlashH; ++y)
        {
            const float ny = ((static_cast<float>(y) + 0.5f) / static_cast<float>(kFlashH) - 0.5f) * 2.0f;
            for (uint32_t x = 0; x < kFlashW; ++x)
            {
                const float nx = ((static_cast<float>(x) + 0.5f) / static_cast<float>(kFlashW) - 0.5f) * 2.0f;
                const float r = sqrtf(nx * nx + ny * ny);
                float a = 0.0f;
                if (r < 0.95f)
                {
                    const float c = cosf((r / 0.95f) * 1.5707963f);
                    a = c * c * c;
                }
                const size_t idx = (static_cast<size_t>(y) * kFlashW + x) * 4;
                // BGRA memory order for D3DFMT_A8R8G8B8 (warm DSLR flash white)
                buf[idx + 0] = 238;
                buf[idx + 1] = 250;
                buf[idx + 2] = 255;
                buf[idx + 3] = static_cast<uint8_t>(std::clamp(a * 255.0f, 0.0f, 255.0f));
            }
        }
        return UploadBgraToGpuTexture(dev, buf.data(), kFlashW, kFlashH);
    }

    void DrawSplashQuadUV(IDirect3DDevice9* dev, IDirect3DTexture9* tex, float alpha,
                          const Vector& center, const Vector& rightHalf, const Vector& upHalf,
                          float uMin, float vMin, float uMax, float vMax)
    {
        if (!tex || alpha <= 0.001f)
            return;
        const uint8_t aByte = static_cast<uint8_t>(std::clamp(alpha * 255.0f + 0.5f, 0.0f, 255.0f));
        dev->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_ARGB(aByte, 255, 255, 255));
        dev->SetTexture(0, tex);

        const Vector tl = center - rightHalf + upHalf;
        const Vector tr = center + rightHalf + upHalf;
        const Vector bl = center - rightHalf - upHalf;
        const Vector br = center + rightHalf - upHalf;
        const SplashVtx verts[4] = {
            { tl.x, tl.y, tl.z, uMin, vMin },
            { tr.x, tr.y, tr.z, uMax, vMin },
            { bl.x, bl.y, bl.z, uMin, vMax },
            { br.x, br.y, br.z, uMax, vMax },
        };
        dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, verts, sizeof(SplashVtx));
    }

    void DrawSplashQuad(IDirect3DDevice9* dev, IDirect3DTexture9* tex, float alpha,
                        const Vector& center, const Vector& rightHalf, const Vector& upHalf)
    {
        DrawSplashQuadUV(dev, tex, alpha, center, rightHalf, upHalf, 0.0f, 0.0f, 1.0f, 1.0f);
    }
}

void VR::InitStartupSplash()
{
    static constexpr const char* kSkySuffix[6] = { "ft", "rt", "bk", "lf", "up", "dn" };
    for (int i = 0; i < kSplashFileCount; ++i)
        s_SplashFilePaths[i].clear();

    s_HasStereoSky = false;
    s_HasAllSixSky = false;

    // Stereo 3D 360-degree cubemap
    {
        bool hasStereo = true;
        for (int i = 0; i < 6; ++i)
        {
            const std::string lJpg = ResolveGameRelativePath((std::string("VR\\startup_sky_L_") + kSkySuffix[i] + ".jpg").c_str());
            const std::string rJpg = ResolveGameRelativePath((std::string("VR\\startup_sky_R_") + kSkySuffix[i] + ".jpg").c_str());
            if (GetFileAttributesA(lJpg.c_str()) != INVALID_FILE_ATTRIBUTES &&
                GetFileAttributesA(rJpg.c_str()) != INVALID_FILE_ATTRIBUTES)
            {
                s_SplashFilePaths[i]     = lJpg;
                s_SplashFilePaths[6 + i] = rJpg;
            }
            else
            {
                hasStereo = false;
                break;
            }
        }

        if (hasStereo)
        {
            s_HasStereoSky = true;
            s_HasAllSixSky = true;
        }
        else
        {
            s_HasStereoSky = false;
            s_HasAllSixSky = true;
            for (int i = 0; i < 12; ++i)
                s_SplashFilePaths[i].clear();
            for (int i = 0; i < 6; ++i)
            {
                const std::string jpgPath = ResolveGameRelativePath((std::string("VR\\startup_sky_") + kSkySuffix[i] + ".jpg").c_str());
                const std::string pngPath = ResolveGameRelativePath((std::string("VR\\startup_sky_") + kSkySuffix[i] + ".png").c_str());
                if (GetFileAttributesA(jpgPath.c_str()) != INVALID_FILE_ATTRIBUTES)
                    s_SplashFilePaths[i] = jpgPath;
                else if (GetFileAttributesA(pngPath.c_str()) != INVALID_FILE_ATTRIBUTES)
                    s_SplashFilePaths[i] = pngPath;
                else
                {
                    s_SplashFilePaths[i].clear();
                    s_HasAllSixSky = false;
                }
            }
        }
    }

    const std::string infraLogoPath = ResolveGameRelativePath("VR\\startup_logo_infra.png");
    const std::string vrLogoPath = ResolveGameRelativePath("VR\\startup_logo_vr.png");
    s_SplashFilePaths[kSlotLogoInfra] = (GetFileAttributesA(infraLogoPath.c_str()) != INVALID_FILE_ATTRIBUTES) ? infraLogoPath : std::string();
    s_SplashFilePaths[kSlotLogoVr] = (GetFileAttributesA(vrLogoPath.c_str()) != INVALID_FILE_ATTRIBUTES) ? vrLogoPath : std::string();

    if (!s_HasAllSixSky && s_SplashFilePaths[kSlotLogoInfra].empty())
        return;

    // Stream-decode images 1-by-1 on a background WIC thread (peak 32-bit RAM ~37 MB, 0 MB after upload!)
    {
        std::lock_guard<std::mutex> lk(s_TexMutex);
        s_TexLoaderStop = false;
        s_TexLoaderDone = false;
        s_StagingSlot = -1;
        s_StagingPixels.clear();
    }
    s_AllGpuTexturesReady = false;

    s_TexLoaderThread = std::thread([]() {
        const HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IWICImagingFactory* factory = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&factory))) && factory)
        {
            for (int slot = 0; slot < kSplashFileCount; ++slot)
            {
                {
                    std::lock_guard<std::mutex> lk(s_TexMutex);
                    if (s_TexLoaderStop)
                        break;
                }
                if (s_SplashFilePaths[slot].empty())
                    continue;

                std::vector<uint8_t> pixels;
                uint32_t w = 0, h = 0;
                if (!DecodeImageWicBgra(factory, s_SplashFilePaths[slot], pixels, w, h))
                    continue;

                std::unique_lock<std::mutex> lk(s_TexMutex);
                s_TexCv.wait(lk, []() { return s_StagingSlot == -1 || s_TexLoaderStop; });
                if (s_TexLoaderStop)
                    break;
                s_StagingPixels.swap(pixels);
                s_StagingW = w;
                s_StagingH = h;
                s_StagingSlot = slot;
            }
            factory->Release();
        }
        if (SUCCEEDED(hrCom))
            CoUninitialize();

        std::lock_guard<std::mutex> lk(s_TexMutex);
        s_TexLoaderDone = true;
    });

    // Preload exact startup sounds on a pure disk-I/O thread (never blocks VR::VR() or touches OpenVR):
    // 1) Subtle hall ambience  2) Mark: "I'm pretty sure that's not meant to be like that..."
    // 3) DSLR focus beep (INFRA appears)  4) DSLR shutter snap + flash (VR appears)
    m_StartupAudioLoaded.store(false);
    m_StartupAudioStopRequested.store(false);
    {
        const std::string vpkPath = ResolveGameRelativePath("infra\\pak01_dir.vpk");
        m_StartupAudioLoadThread = std::thread([this, vpkPath]() {
            ReadFileFromInfraVpk(vpkPath, "wav", "sound/ambient/ambience", "ambience_002", s_StartupAmbienceWav);
            ReadFileFromInfraVpk(vpkPath, "wav", "sound/voice/player/monologue", "im_pretty_sure_thats_not_meant_to_be_like_that_1", s_StartupGagWav);
            ReadFileFromInfraVpk(vpkPath, "wav", "sound/items/camera", "focus", s_StartupFocusWav);
            ReadFileFromInfraVpk(vpkPath, "wav", "sound/items/camera", "snap", s_StartupSnapWav);
            m_StartupAudioLoaded.store(true);
        });
    }

    m_StartupSplashActive = true;
}

void VR::ReleaseStartupSplashGpuResources()
{
    std::lock_guard<std::mutex> lk(s_GpuResMutex);
    for (int i = 0; i < kSplashTexCount; ++i)
    {
        if (s_GpuTex[i])
        {
            s_GpuTex[i]->Release();
            s_GpuTex[i] = nullptr;
        }
    }
    if (s_SplashStateBlock)
    {
        s_SplashStateBlock->Release();
        s_SplashStateBlock = nullptr;
    }
    s_AllGpuTexturesReady = false;
}

void VR::RenderStartupSplashDirect(IDirect3DDevice9* dev)
{
    if (!m_StartupSplashActive || !dev || !m_D9LeftEyeSurface || !m_D9RightEyeSurface ||
        m_RenderWidth == 0 || m_RenderHeight == 0)
        return;

    std::lock_guard<std::mutex> gpuLock(s_GpuResMutex);

    // 1. Pump at most 1 GPU texture upload per frame so 120 FPS stays rock-solid during init
    if (!s_AllGpuTexturesReady)
    {
        if (!s_GpuTex[kSlotFlash])
            s_GpuTex[kSlotFlash] = CreateRadialFlashGpuTexture(dev);

        int slotToUpload = -1;
        uint32_t w = 0, h = 0;
        std::vector<uint8_t> pixels;
        bool loaderDone = false;
        {
            std::lock_guard<std::mutex> lk(s_TexMutex);
            if (s_StagingSlot >= 0)
            {
                slotToUpload = s_StagingSlot;
                w = s_StagingW;
                h = s_StagingH;
                pixels.swap(s_StagingPixels);
                s_StagingSlot = -1;
            }
            loaderDone = s_TexLoaderDone && (s_StagingSlot == -1);
        }
        if (slotToUpload >= 0)
        {
            s_TexCv.notify_all();
            if (s_GpuTex[slotToUpload])
                s_GpuTex[slotToUpload]->Release();
            const bool isSkyFace = (slotToUpload < 12);
            s_GpuTex[slotToUpload] = UploadBgraToGpuTexture(dev, pixels.data(), w, h, isSkyFace);
        }
        if (loaderDone && slotToUpload < 0)
        {
            {
                std::lock_guard<std::mutex> lk(s_TexMutex);
                std::vector<uint8_t>().swap(s_StagingPixels);
            }
            s_AllGpuTexturesReady = true;
        }
    }

    using Clock = std::chrono::steady_clock;
    static bool s_ClockStarted = false;
    static Clock::time_point s_StartTime{};
    static bool s_Anchored = false;
    static float s_AnchorYaw = 0.0f;
    static bool s_PlayedAmbience = false;
    static bool s_PlayedGag = false;
    static bool s_PlayedFocus = false;
    static bool s_PlayedSnap = false;
    static bool s_FadingOut = false;
    static Clock::time_point s_FadeStartTime{};

    const auto now = Clock::now();
    vr::IVROverlay* pOverlay = vr::VROverlay();
    const bool dashVisible = (pOverlay && pOverlay->IsDashboardVisible());

    // Helper to save/restore Source's RT & DS around clears or draws
    IDirect3DSurface9* oldRT = nullptr;
    IDirect3DSurface9* oldDS = nullptr;
    dev->GetRenderTarget(0, &oldRT);
    dev->GetDepthStencilSurface(&oldDS);
    const D3DVIEWPORT9 fullVp = { 0, 0, m_RenderWidth, m_RenderHeight, 0.0f, 1.0f };

    // While textures finish streaming (~0.2s) or SteamVR Dashboard is open, hold cleanly on pure black at 120 FPS
    if (!s_AllGpuTexturesReady || (!s_ClockStarted && dashVisible))
    {
        IDirect3DSurface9* eyes[2] = { m_D9LeftEyeSurface, m_D9RightEyeSurface };
        for (int e = 0; e < 2; ++e)
        {
            dev->SetRenderTarget(0, eyes[e]);
            dev->SetDepthStencilSurface(nullptr);
            dev->SetViewport(&fullVp);
            dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 0, 0, 0), 1.0f, 0);
        }
        dev->SetRenderTarget(0, oldRT);
        dev->SetDepthStencilSurface(oldDS);
        if (oldRT) oldRT->Release();
        if (oldDS) oldDS->Release();
        return;
    }

    // Grab the EXACT HMD pose that SubmitVRTextures is about to hand to Submit_TextureWithPose
    vr::HmdMatrix34_t hmdMat{};
    const bool poseValid = PeekPendingFramePose(hmdMat);
    if (!poseValid)
    {
        hmdMat.m[0][0] = 1.0f; hmdMat.m[1][1] = 1.0f; hmdMat.m[2][2] = 1.0f;
    }

    if (!s_ClockStarted)
    {
        s_ClockStarted = true;
        s_StartTime = now;
    }
    if (!s_Anchored && poseValid)
    {
        s_AnchorYaw = atan2f(hmdMat.m[0][2], hmdMat.m[2][2]);
        s_Anchored = true;
    }

    const float elapsed = std::chrono::duration<float>(now - s_StartTime).count();

    // Fixed audio & visual choreography:
    // 0.00s: 360-degree Turbine Hall fades in + subtle industrial ambience
    // 1.40s: Mark: "I'm pretty sure that's not meant to be like that..." (2.96s -> finishes at 4.36s)
    // 4.55s: DSLR focus beep -> "INFRA ↘" appears in true Stereo 3D
    // 5.15s: DSLR shutter click + soft radial flash -> amber "VR" snaps in (fully visible at 5.37s)
    // 7.40s: (~2.0s hold after "VR" appears) -> Slow, silky 2.20s smoothstep dissolve into the live 3D main menu!
    constexpr float kFadeOutDur = 2.20f;
    constexpr float fadeOutStart = 7.40f;

    if (m_StartupAudioLoaded.load() && !s_FadingOut)
    {
        if (!s_PlayedAmbience)
        {
            s_PlayedAmbience = true;
            PlayPcmWavBytesAsync(s_StartupAmbienceWav, m_StartupAudioStopRequested, 0.28f, fadeOutStart + kFadeOutDur + 0.1f, 0.8f, kFadeOutDur);
        }
        if (!s_PlayedGag && elapsed >= 1.40f)
        {
            s_PlayedGag = true;
            PlayPcmWavBytesAsync(s_StartupGagWav, m_StartupAudioStopRequested, 1.0f);
        }
        else if (!s_PlayedFocus && elapsed >= 4.55f)
        {
            s_PlayedFocus = true;
            PlayPcmWavBytesAsync(s_StartupFocusWav, m_StartupAudioStopRequested, 0.90f);
        }
        else if (!s_PlayedSnap && elapsed >= 5.15f)
        {
            s_PlayedSnap = true;
            PlayPcmWavBytesAsync(s_StartupSnapWav, m_StartupAudioStopRequested, 0.95f);
        }
    }

    if (!s_FadingOut && elapsed >= fadeOutStart)
    {
        s_FadingOut = true;
        s_FadeStartTime = now;
    }

    float masterFade = 1.0f;
    bool finishedSplash = false;
    if (s_FadingOut)
    {
        const float fadeElapsed = std::chrono::duration<float>(now - s_FadeStartTime).count();
        const float u = std::clamp(fadeElapsed / kFadeOutDur, 0.0f, 1.0f);
        // Smoothstep S-curve dissolves evenly and gently into the 3D main menu (unlike EaseOutCubic which dropped too fast!)
        const float smooth = u * u * (3.0f - 2.0f * u);
        masterFade = 1.0f - smooth;
        if (fadeElapsed >= kFadeOutDur)
            finishedSplash = true;
    }

    if (finishedSplash)
    {
        dev->SetRenderTarget(0, oldRT);
        dev->SetDepthStencilSurface(oldDS);
        if (oldRT) oldRT->Release();
        if (oldDS) oldDS->Release();
        // Release GPU lock before DestroyStartupSplash acquires it
        s_GpuResMutex.unlock();
        DestroyStartupSplash();
        s_GpuResMutex.lock();
        return;
    }

    // Create reusable D3D9 state block once to preserve 100% of Source Engine's D3D9 state
    if (!s_SplashStateBlock)
        dev->CreateStateBlock(D3DSBT_ALL, &s_SplashStateBlock);
    if (s_SplashStateBlock)
        s_SplashStateBlock->Capture();

    // Exact projection matrix matching m_TextureBounds[0..1] (RH: +X right, +Y up, -Z forward)
    const float tanX = std::max(m_TanHalfFovX, 0.1f);
    const float tanY = std::max(m_TanHalfFovY, 0.1f);
    const float zn = 0.05f;
    const float zf = 100.0f;
    D3DMATRIX matProj{};
    matProj._11 = 1.0f / tanX;
    matProj._22 = 1.0f / tanY;
    matProj._33 = -zf / (zf - zn);
    matProj._34 = -1.0f;
    matProj._43 = -(zn * zf) / (zf - zn);

    D3DMATRIX matIdentity{};
    matIdentity._11 = 1.0f; matIdentity._22 = 1.0f; matIdentity._33 = 1.0f; matIdentity._44 = 1.0f;

    // Anchor horizontal basis aligned with player's initial forward direction
    const float cY = cosf(s_AnchorYaw);
    const float sY = sinf(s_AnchorYaw);
    const Vector vRight{ cY, 0.0f, -sY };
    const Vector vUp{ 0.0f, 1.0f, 0.0f };
    const Vector vFwd{ -sY, 0.0f, -cY };

    // Live HMD rotation columns (world -> HMD view)
    const Vector hX{ hmdMat.m[0][0], hmdMat.m[1][0], hmdMat.m[2][0] };
    const Vector hY{ hmdMat.m[0][1], hmdMat.m[1][1], hmdMat.m[2][1] };
    const Vector hZ{ hmdMat.m[0][2], hmdMat.m[1][2], hmdMat.m[2][2] };

    D3DMATRIX matViewSky{};
    matViewSky._11 = hX.x; matViewSky._12 = hY.x; matViewSky._13 = hZ.x; matViewSky._14 = 0.0f;
    matViewSky._21 = hX.y; matViewSky._22 = hY.y; matViewSky._23 = hZ.y; matViewSky._24 = 0.0f;
    matViewSky._31 = hX.z; matViewSky._32 = hY.z; matViewSky._33 = hZ.z; matViewSky._34 = 0.0f;
    matViewSky._41 = 0.0f; matViewSky._42 = 0.0f; matViewSky._43 = 0.0f; matViewSky._44 = 1.0f;

    // Per-eye stereo IPD offsets in HMD space (same translation Infra-VR uses for real 3D eyes)
    Vector eyeLocal[2] = { { -0.032f, 0.0f, 0.0f }, { 0.032f, 0.0f, 0.0f } };
    if (m_System)
    {
        const vr::HmdMatrix34_t eL = m_System->GetEyeToHeadTransform(vr::Eye_Left);
        const vr::HmdMatrix34_t eR = m_System->GetEyeToHeadTransform(vr::Eye_Right);
        eyeLocal[0] = { eL.m[0][3], eL.m[1][3], eL.m[2][3] };
        eyeLocal[1] = { eR.m[0][3], eR.m[1][3], eR.m[2][3] };
    }

    // Animation alphas & transforms
    const float envIn = SplashEaseOutCubic(elapsed / 0.90f);
    const float envAlpha = envIn * masterFade;

    const float dtFlash = elapsed - 5.15f;
    const float flashAlpha = (dtFlash >= 0.0f && dtFlash < 0.45f)
        ? (0.62f * expf(-dtFlash * 10.0f) * masterFade)
        : 0.0f;

    const float infraIn = SplashEaseOutCubic((elapsed - 4.55f) / 0.35f);
    const float infraAlpha = infraIn * masterFade;
    const float vrIn = SplashEaseOutCubic((elapsed - 5.15f) / 0.22f);
    const float vrAlpha = vrIn * masterFade;

    const float floatZ = (elapsed > 5.35f) ? (0.022f * sinf((elapsed - 5.35f) * 2.1f)) : 0.0f;
    const float floatY = (elapsed > 5.35f) ? (0.007f * sinf((elapsed - 5.35f) * 1.4f)) : 0.0f;
    constexpr float baseLogoDist = 2.00f;
    const Vector logoCenter = vFwd * (baseLogoDist + floatZ) + Vector{ 0.0f, 0.02f + floatY, 0.0f };
    const Vector flashCenter = vFwd * (baseLogoDist + 0.25f);

    // 6 seamless skybox cube faces (S = 10m half-extent, matches Python stitcher face_defs 1:1!)
    constexpr float S = 10.0f;
    const Vector negFwd{ -vFwd.x, -vFwd.y, -vFwd.z };
    const Vector negRight{ -vRight.x, -vRight.y, -vRight.z };
    const Vector negUp{ 0.0f, -1.0f, 0.0f };
    struct CubeFace { Vector fwd, right, up; };
    const CubeFace faces[6] = {
        { vFwd,     vRight,   vUp    }, // 0: ft
        { vRight,   negFwd,   vUp    }, // 1: rt
        { negFwd,   negRight, vUp    }, // 2: bk
        { negRight, vFwd,     vUp    }, // 3: lf
        { vUp,      vRight,   negFwd }, // 4: up
        { negUp,    vRight,   vFwd   }, // 5: dn
    };

    // Clean fixed-function pipeline state (16x Anisotropic + exact Clamp -> ZERO seams, native 4x MSAA!)
    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(nullptr);
    dev->SetFVF(kSplashFvf);
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0000000F);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);

    dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_ANISOTROPIC);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAXANISOTROPY, 16);
    {
        // Negative LOD bias keeps center of curved 3D screen / 360 cubemap razor-sharp on 35 PPD HMDs
        // instead of dropping early into blurry Mip 1 (half-res)!
        const float lodBias = -0.85f;
        DWORD lodBiasBits = 0;
        memcpy(&lodBiasBits, &lodBias, sizeof(DWORD));
        dev->SetSamplerState(0, D3DSAMP_MIPMAPLODBIAS, lodBiasBits);
    }

    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_TFACTOR);
    dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

    dev->SetTransform(D3DTS_WORLD, &matIdentity);
    dev->SetTransform(D3DTS_PROJECTION, &matProj);

    const bool begun = SUCCEEDED(dev->BeginScene());
    IDirect3DSurface9* eyes[2] = { m_D9LeftEyeSurface, m_D9RightEyeSurface };

    for (int e = 0; e < 2; ++e)
    {
        dev->SetRenderTarget(0, eyes[e]);
        dev->SetDepthStencilSurface(nullptr);
        dev->SetViewport(&fullVp);

        // Before fade-out (t < 7.20s): clear to pitch black so 360 hall fades in from black.
        // During fade-out (7.20..8.05s): keep live 3D main menu underneath for silky alpha dissolve!
        if (!s_FadingOut)
            dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 0, 0, 0), 1.0f, 0);

        // Pass 1: True Binocular Stereo 3D 360-degree mipmapped cubemap
        D3DMATRIX matViewStereo = matViewSky;
        matViewStereo._41 = -eyeLocal[e].x;
        matViewStereo._42 = -eyeLocal[e].y;
        matViewStereo._43 = -eyeLocal[e].z;

        if (s_HasAllSixSky && envAlpha > 0.001f)
        {
            dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
            dev->SetTransform(D3DTS_VIEW, &matViewSky);
            const float lookDot = -(hZ.x * vFwd.x + hZ.y * vFwd.y + hZ.z * vFwd.z);
            const int skyBase = s_HasStereoSky ? (((lookDot >= 0.0f) ? e : (1 - e)) * 6) : 0;
            for (int f = 0; f < 6; ++f)
            {
                DrawSplashQuad(dev, s_GpuTex[skyBase + f], envAlpha,
                               faces[f].fwd * S, faces[f].right * S, faces[f].up * S);
            }
        }

        // Pass 2: True Binocular Stereo 3D floating flash + INFRA ↘ + VR logo cleanly in front at 1.95m..2.00m!
        dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        dev->SetTransform(D3DTS_VIEW, &matViewStereo);

        if (flashAlpha > 0.002f && s_GpuTex[kSlotFlash])
        {
            constexpr float fHW = 1.50f;
            constexpr float fHH = fHW * (9.0f / 16.0f);
            DrawSplashQuad(dev, s_GpuTex[kSlotFlash], flashAlpha, flashCenter, vRight * fHW, vUp * fHH);
        }

        if (infraAlpha > 0.001f && s_GpuTex[kSlotLogoInfra])
        {
            const float hw = 0.5f * (1.36f + 0.08f * infraIn);
            const float hh = hw * 0.25f; // 1920x480 = 4:1
            DrawSplashQuad(dev, s_GpuTex[kSlotLogoInfra], infraAlpha, logoCenter, vRight * hw, vUp * hh);
        }

        if (vrAlpha > 0.001f && s_GpuTex[kSlotLogoVr])
        {
            constexpr float hw = 0.5f * 1.44f;
            constexpr float hh = hw * 0.25f; // 1920x480 = 4:1
            DrawSplashQuad(dev, s_GpuTex[kSlotLogoVr], vrAlpha, logoCenter, vRight * hw, vUp * hh);
        }
    }

    if (begun)
        dev->EndScene();

    if (s_SplashStateBlock)
        s_SplashStateBlock->Apply();

    dev->SetRenderTarget(0, oldRT);
    dev->SetDepthStencilSurface(oldDS);
    if (oldRT) oldRT->Release();
    if (oldDS) oldDS->Release();
}

void VR::DestroyStartupSplash()
{
    m_StartupSplashActive = false;

    {
        std::lock_guard<std::mutex> lk(s_TexMutex);
        s_TexLoaderStop = true;
    }
    s_TexCv.notify_all();
    if (s_TexLoaderThread.joinable())
        s_TexLoaderThread.join();
    {
        std::lock_guard<std::mutex> lk(s_TexMutex);
        std::vector<uint8_t>().swap(s_StagingPixels);
    }

    m_StartupAudioStopRequested.store(true);
    if (m_StartupAudioLoadThread.joinable())
        m_StartupAudioLoadThread.join();

    ReleaseStartupSplashGpuResources();
}
