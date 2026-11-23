#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include "openvr.h"
#include "vector.h"
#include "vr_profiler.h"
#include <chrono>
#include <mutex>
#include <atomic>
#include <thread>
#include <deque>

#define MAX_STR_LEN 1024

class Game;
struct IDirect3DDevice9;
struct IDirect3DTexture9;
struct IDirect3DSurface9;
class ITexture;


struct TrackedDevicePoseData 
{
	std::string TrackedDeviceName;
	Vector TrackedDevicePos;
	Vector TrackedDeviceVel;
	QAngle TrackedDeviceAng;
	QAngle TrackedDeviceAngVel;
};

struct SharedTextureHolder 
{
	vr::VRVulkanTextureData_t m_VulkanData{};
	vr::Texture_t m_VRTexture{ nullptr, vr::TextureType_Vulkan, vr::ColorSpace_Auto };
};

class VR
{
public:
	Game *m_Game = nullptr;

	vr::IVRSystem *m_System = nullptr;
	vr::IVRInput *m_Input = nullptr;
	vr::IVROverlay *m_Overlay = nullptr;

	vr::VROverlayHandle_t m_MainMenuHandle = vr::k_ulOverlayHandleInvalid;
	//vr::VROverlayHandle_t m_HUDHandle;

	float m_HorizontalOffsetLeft = 0.0f;
	float m_VerticalOffsetLeft = 0.0f;
	float m_HorizontalOffsetRight = 0.0f;
	float m_VerticalOffsetRight = 0.0f;

	uint32_t m_RenderWidth = 0;
	uint32_t m_RenderHeight = 0;
	uint32_t m_AntiAliasing = 0;
	uint32_t m_RenderWindow = 0;
	float m_RenderScale = 0.85f;
	float m_Aspect = 1.0f;
	float m_Fov = 90.0f;
	float m_TanHalfFovX = 1.0f;
	float m_TanHalfFovY = 1.0f;

	vr::VRTextureBounds_t m_TextureBounds[2]{};
	vr::TrackedDevicePose_t m_Poses[vr::k_unMaxTrackedDeviceCount]{};

	// ---- Multi-threaded rendering (mat_queue_mode 2) ----------------------------------------
	// Main thread  (dRenderView -> MainThreadUpdate): input, tracking (predicted poses), engine/VGUI queries.
	// Render thread (PresentEx  -> RenderThreadPresent): resolves, SteamVR Submit, WaitGetPoses.
	// In single-threaded mode both run on the same thread and behave like before.
	struct VRFrameData
	{
		vr::HmdMatrix34_t hmdPose {};   // HMD pose used to render this frame (for Submit_TextureWithPose)
		bool hmdPoseValid = false;
	};
	std::atomic<unsigned long> m_MainThreadId { 0 };
	std::atomic<unsigned long> m_RenderThreadId { 0 };
	bool IsQueuedRendering() const
	{
		unsigned long r = m_RenderThreadId.load(), m = m_MainThreadId.load();
		return r != 0 && m != 0 && r != m;
	}
	std::mutex m_FrameMutex;
	std::deque<VRFrameData> m_PendingFrames;     // pushed at the end of dRenderView, popped in PresentEx
	VRFrameData m_CurrentFrame;                  // main thread: pose captured in MainThreadUpdate
	vr::TrackedDevicePose_t m_RenderPoses[vr::k_unMaxTrackedDeviceCount]{}; // render thread: WaitGetPoses result
	std::mutex m_RenderPosesMutex;
	// Main-thread snapshot for the render thread (engine/VGUI must not be queried from the render thread)
	std::atomic<bool> m_SnapConVisible { false };
	std::atomic<bool> m_SnapSubMenuVisible { false };
	std::atomic<bool> m_SnapInActiveGameplay { false };
	std::atomic<int>  m_SnapWindowW { 1920 };
	std::atomic<int>  m_SnapWindowH { 1080 };
	std::atomic<unsigned long long> m_LastRenderViewTickMs { 0 };
	std::atomic<unsigned long long> m_LastMainUpdateTickMs { 0 };
	std::atomic<bool> m_InRenderView { false };
	bool m_MultiThreadedRendering = true;        // config: MultiThreadedRendering
	void MainThreadUpdate();
	void RenderThreadPresent();
	void PushRenderedFrame();
	bool HasPendingFrame();
	void ClearPendingFrames();
	void UpdatePredictedPoses();
	void UpdateActionsAndControllers();

	Vector m_EyeToHeadTransformPosLeft = { 0,0,0 };
	Vector m_EyeToHeadTransformPosRight = { 0,0,0 };

	Vector m_HmdForward;
	Vector m_HmdRight;
	Vector m_HmdUp;

	Vector m_HmdPosLocalInWorld = { 0,0,0 };

	Vector m_LeftControllerForward;
	Vector m_LeftControllerRight;
	Vector m_LeftControllerUp;

	Vector m_RightControllerForward;
	Vector m_RightControllerRight;
	Vector m_RightControllerUp;

	Vector m_ViewmodelForward;
	Vector m_ViewmodelRight;
	Vector m_ViewmodelUp;

	QAngle m_HmdAngAbs;

	Vector m_HmdPosRelativeRaw = { 0,0,0 };
	Vector m_HmdPosRelativeRawPrev = { 0,0,0 };

	Vector m_HmdPosRelative = { 0,0,0 };
	Vector m_HmdPosRelativePrev = { 0,0,0 };

	Vector m_AimPos = { 0, 0, 0 };
	bool m_Traced = false;

	Vector m_Center = { 0,0,0 };
	Vector m_SetupOrigin = { 0,0,0 };

	float m_HeightOffset = 0.0f;
	bool m_RoomscaleActive = true;
	bool m_RoomscaleSyncAllowed = false;
	bool m_RoomscaleCmdActive = false;
	Vector m_RoomscaleStickVelXY = { 0.0f, 0.0f, 0.0f };
	Vector m_RoomscalePullVelXY = { 0.0f, 0.0f, 0.0f };

	Vector m_LeftControllerPosAbs;											
	Vector m_LeftControllerPosRel = { 0, 0, 0 };
	QAngle m_LeftControllerAngAbs;
	Vector m_RightControllerPosRel;											
	QAngle m_RightControllerAngAbs;

	Vector m_ViewmodelPosOffset;
	QAngle m_ViewmodelAngOffset;

	Vector m_ViewmodelPosCustomOffset; // Custom (from config) viewmodel position offset applied on top of hardcoded ones
    QAngle m_ViewmodelAngCustomOffset; // Custom (from config) viewmodel angle offset applied on top of hardcoded ones

	// Per-hand offsets: Pos in cm (X=fwd/back, Y=right/left, Z=up/down), Ang in deg (X=pitch fwd+/back-, Y=yaw, Z=roll)
	Vector m_RightHandPosOffset = { -10.0f, 0.0f, -10.0f };
	QAngle m_RightHandAngOffset = { 45.0f, 0.0f, 0.0f };
	Vector m_LeftHandPosOffset = { 0.0f, 0.0f, -20.0f };
	QAngle m_LeftHandAngOffset = { -45.0f, 0.0f, 0.0f };

	bool m_ShowHands = true;
	// Hand-based use selection ("virtual fingertip")
	float m_UseTipDistanceCm = 9.0f;
	float m_UseTouchRadiusCm = 15.0f;
	// Physical pointer pose (raw controller + ergonomic pitch, WITHOUT the camera/flashlight grip calibration offsets)
	Vector m_LeftPointerPosRel = { 0.0f, 0.0f, 0.0f };
	Vector m_RightPointerPosRel = { 0.0f, 0.0f, 0.0f };
	Vector m_LeftPointerFwd = { 1.0f, 0.0f, 0.0f };
	Vector m_LeftPointerRight = { 0.0f, 1.0f, 0.0f };
	Vector m_LeftPointerUp = { 0.0f, 0.0f, 1.0f };
	Vector m_RightPointerFwd = { 1.0f, 0.0f, 0.0f };
	Vector m_RightPointerRight = { 0.0f, 1.0f, 0.0f };
	Vector m_RightPointerUp = { 0.0f, 0.0f, 1.0f };
	Vector GetLeftPointerAbsPos();
	Vector GetRightPointerAbsPos();
	// 3D main menu seat relative to the laptop camera (menu_viewcontrol)
	float m_MenuSeatHeightCm = 23.5f;
	float m_MenuSeatForwardCm = 0.0f;
	// Chapter 1 intro chair eye height offset (cm)
	float m_IntroSeatHeightCm = 5.0f;
	// Chapter 2 intro car eye height offset (cm)
	float m_CarSeatHeightCm = 15.0f;
	// MSAA is fixed once the eye render targets exist (changing it live mixes 4x/8x surfaces)
	uint32_t m_AntiAliasingConfig = 4;
	uint32_t m_AntiAliasingLocked = 0;
	bool m_HasLockedAntiAliasing = false;
	bool m_CalibrationMode = false;
	bool m_IsCalibrating = false;
	bool m_CalibrationDirty = false;
	bool m_CalibratingRight = false;
	bool m_CalibratingLeft = false;
	bool m_CalRightPoseLocked = false;
	bool m_CalLeftPoseLocked = false;
	Vector m_CalRightLockedPosRel = { 0.0f, 0.0f, 0.0f };
	Vector m_CalRightLockedFwd = { 1.0f, 0.0f, 0.0f };
	Vector m_CalRightLockedRight = { 0.0f, 1.0f, 0.0f };
	Vector m_CalRightLockedUp = { 0.0f, 0.0f, 1.0f };
	Vector m_CalLeftLockedPosRel = { 0.0f, 0.0f, 0.0f };
	Vector m_CalLeftLockedFwd = { 1.0f, 0.0f, 0.0f };
	Vector m_CalLeftLockedRight = { 0.0f, 1.0f, 0.0f };
	Vector m_CalLeftLockedUp = { 0.0f, 0.0f, 1.0f };
	float m_StickChordHoldMs = 0.0f;
	bool m_StickChordHandled = false;
	bool m_SprintActive = false;
	bool m_StickJumpActive = false;
	bool m_StickCrouchActive = false;
	bool m_FlashlightEquipped = false;
	bool m_FlashlightBeamOn = false;
	bool m_RightHandEmpty = true;
	bool m_PhoneEquipped = false;
	float m_CameraZoomTarget = 1.0f;
	float m_CameraZoomFactor = 1.0f;
	bool m_UseWithLeftHand = false;
	bool m_InPickupUpdate = false;
	bool m_PendingCameraReload = false;
	bool m_PendingFlashlightReload = false;
	bool m_AllowFlashlightReload = false;
	int m_CameraReloadTicks = 0;
	bool m_PendingInstantCameraReload = false;
	bool m_PendingInstantFlashlightReload = false;
	bool m_HasSuperFlashlight = false;
	bool m_PendingFlashlightBoost = false;
	bool m_FlashlightTriggerHeld = false;
	bool m_FlashlightTriggerTurnedOnBeam = false;
	bool m_FlashlightBoostTriggered = false;
	float m_FlashlightTriggerHoldMs = 0.0f;
	bool m_SuppressFlashlightTriggerUntilRelease = false;
	bool m_FlashlightOverhandGrip = false;
	float m_LeftHandSpeedMps = 0.0f;
	float m_RightHandSpeedMps = 0.0f;
	bool m_IsHoldingObject = false;
	bool m_HoldingWithLeftHand = false;
	int m_DropObjectTicks = 0;
	std::chrono::steady_clock::time_point m_LastGrabUpdateTime{};

	float m_Ipd = 0.0f;																	
	float m_EyeZ = 0.0f;

	Vector m_IntendedPositionOffset = { 0,0,0 };

	enum TextureID
	{
		Texture_None = -1,
		Texture_LeftEye,
		Texture_RightEye,
		Texture_HUD,
		Texture_Blank
	};

	ITexture *m_LeftEyeTexture = nullptr;
	ITexture *m_RightEyeTexture = nullptr;
	ITexture *m_HUDTexture = nullptr;
	ITexture *m_BlankTexture = nullptr;

	IDirect3DSurface9 *m_D9LeftEyeSurface = nullptr;
	IDirect3DSurface9 *m_D9RightEyeSurface = nullptr;
	IDirect3DSurface9 *m_D9HUDSurface = nullptr;
	IDirect3DSurface9 *m_D9BlankSurface = nullptr;

	SharedTextureHolder m_VKLeftEye{};
	SharedTextureHolder m_VKRightEye{};
	SharedTextureHolder m_VKBackBuffer{};
	SharedTextureHolder m_VKHUD{};
	SharedTextureHolder m_VKBlankTexture{};

	bool m_IsVREnabled = false;
	bool m_IsInitialized = false;
	std::atomic<bool> m_RenderedNewFrame{ false };
	bool m_RenderedHud = false;
	bool m_CreatedVRTextures = false;
	bool m_DrawCrosshair = false;
	TextureID m_CreatingTextureID = Texture_None;
	int m_ActiveEyePass = 0;

	bool m_PressedTurn = false;
	bool m_PushingThumbstick = false;
	bool m_PointerCreated = false;

	// action set
	vr::VRActionSetHandle_t m_ActionSet = vr::k_ulInvalidActionSetHandle;
	vr::VRActionSetHandle_t m_BaseActionSet = vr::k_ulInvalidActionSetHandle;
	vr::VRActiveActionSet_t m_ActiveActionSet = {};
	vr::VRActiveActionSet_t m_ActiveActionSets[2] = {};

	// actions
	vr::VRActionHandle_t m_ActionJump = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionPrimaryAttack = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionSecondaryAttack = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionReload = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionWalk = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionTurn = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionUse = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionNextItem = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionPrevItem = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionResetPosition = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionCrouch = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionFlashlight = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionActivateVR = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_MenuSelect = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_MenuBack = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_MenuUp = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_MenuDown = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_MenuLeft = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_MenuRight = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_Spray = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_Scoreboard = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ShowHUD = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_Pause = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionLeftTrackpad = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionRightTrackpad = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionReloadFlashlight = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionReloadCamera = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionSkeletonLeft = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionSkeletonRight = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionVibrationLeft = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t m_ActionVibrationRight = vr::k_ulInvalidActionHandle;

	// Live 5-finger curl [0=Thumb, 1=Index, 2=Middle, 3=Ring, 4=Pinky] in [0..1] and splay [0..3]
	float m_LeftFingerCurl[5] = { 0.35f, 0.35f, 0.35f, 0.35f, 0.35f };
	float m_RightFingerCurl[5] = { 0.35f, 0.35f, 0.35f, 0.35f, 0.35f };
	float m_LeftFingerSplay[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	float m_RightFingerSplay[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

	TrackedDevicePoseData m_HmdPose;
	TrackedDevicePoseData m_LeftControllerPose;
	TrackedDevicePoseData m_RightControllerPose;

	vr::TrackedDeviceIndex_t m_LeftControllerIndex = vr::k_unTrackedDeviceIndexInvalid;
	vr::TrackedDeviceIndex_t m_RightControllerIndex = vr::k_unTrackedDeviceIndexInvalid;
	vr::VRControllerState_t m_LeftControllerState{};
	vr::VRControllerState_t m_RightControllerState{};
	bool m_HasLeftControllerState = false;
	bool m_HasRightControllerState = false;

	bool m_ApplyPortalRotationOffset = false;
	QAngle m_PortalRotationOffset = {0, 0, 0};
	QAngle m_RotationOffset = { 0, 0, 0 };
	bool m_OverrideEyeAngles = false;
	bool m_OverrideClientEyeAngles = false;
	bool m_OverrideClientUseEye = false;
	bool m_OverrideFlashlightEyeAngles = false;
	bool m_HasCenteredHMD = false;
	bool m_In3DViewControlMenu = false;
	Vector m_MenuViewOrigin = { 35.0f, 98.0f, 40.0f };
	QAngle m_MenuViewAngles = { 13.0f, 270.0f, 0.0f };
	float m_MenuViewFov = 42.0f;
	// Fixed VR viewpoint in the 3D main menu (laptop camera), independent of the engine's
	// Options/Chapters camera transitions which are pointless in VR.
	Vector m_MenuAnchorOrigin = { 35.0f, 98.0f, 40.0f };
	float m_MenuAnchorYaw = 270.0f;
	// CPU profiler (F12 toggles a 10 s capture -> VR\profile_*.txt + infra\vr_vprof.log)
	VRProfiler m_Profiler;
	void UpdateProfiler();
	int m_MenuCursorX = 960;
	int m_MenuCursorY = 540;
	bool m_MenuCursorInitialized = false;
	// Menu <-> gameplay input transition state (prevents button-release bleed when
	// buttons are shared across menu and gameplay contexts, e.g. Right A = MenuSelect + Camera,
	// Right B = MenuBack + Pause).
	bool m_WasInMenuInput = false;
	bool m_WasMenuSelectDown = false;
	bool m_SentOsMenuMouseDown = false;
	bool m_SuppressMenuSelectUntilRelease = false;
	bool m_SuppressMenuBackUntilRelease = false;
	bool m_SuppressCameraUntilRelease = false;
	bool m_SuppressFlashlightUntilRelease = false;
	bool m_SuppressAltFireUntilRelease = false;
	bool m_SuppressPauseUntilRelease = false;
	float m_CameraBtnHoldMs = 0.0f;
	bool m_CameraReloadTriggered = false;
	float m_FlashlightBtnHoldMs = 0.0f;
	bool m_FlashlightReloadTriggered = false;
	bool m_PauseOpenRequested = false;
	std::chrono::steady_clock::time_point m_PauseOpenedAt{};
	void* m_ActiveVGuiScreen = nullptr;
	bool m_PendingVGuiMousePress = false;
	bool m_PendingVGuiMouseRelease = false;
	bool m_ForceCenterCursor = false;
	std::chrono::steady_clock::time_point m_PrevFrameTime;

	float m_TurnSpeed = 0.15f;
	bool m_SnapTurning = false;
	float m_SnapTurnAngle = 45.0f;
	bool m_LeftHanded = false;
	float m_VRScale = 43.2f;
	float m_IpdScale = 1.0f;
	bool m_6DOF = true;
	float m_HudDistance = 1.3f;
	float m_HudSize = 4.0f;
	bool m_HudAlwaysVisible = false;
	int m_AimMode = 2;

	mutable std::mutex m_ConfigMutex;
	std::atomic<bool> m_ConfigStopRequested{false};
	std::thread m_ConfigThread;

	// ---- Native GPU 360-degree Cubemap Stereo 3D Startup Intro ----
	bool m_StartupSplashActive = false;
	std::atomic<bool> m_StartupAudioLoaded{false};
	std::atomic<bool> m_StartupAudioStopRequested{false};
	std::thread m_StartupAudioLoadThread;
	void InitStartupSplash();
	void RenderStartupSplashDirect(IDirect3DDevice9* dev);
	void ReleaseStartupSplashGpuResources();
	void DestroyStartupSplash();
	bool PeekPendingFramePose(vr::HmdMatrix34_t& outPose);

	VR() {};
	VR(Game *game);
	~VR();
	int SetActionManifest(const char *fileName);
	void InstallApplicationManifest(const char *fileName);
	void Update();
	bool Is2DSubMenuVisible() const;
	void InstallBackgroundFocus();
	void UninstallBackgroundFocus();
	bool IsGameForeground() const;
	void* GetGameHwndRaw() const;
	enum class CameraFlashMode : int
	{
		Off = 0,
		Auto = 1,
		On = 2
	};

	enum class BatteryInventoryMode : int
	{
		Belt = 0,
		Wrist = 1,
		Both = 2,
		Off = 3
	};

	BatteryInventoryMode m_BatteryInventoryMode = BatteryInventoryMode::Belt;
	bool m_BeltHolsterEnabled = true;
	float m_BeltHeightOffsetCm = 78.0f;
	bool m_BeltHeightOffsetAuto = true;
	float m_PlayerHeightCm = 0.0f;
	float m_PlayerInseamCm = 0.0f;
	float m_MeasuredEyeHeightCm = 0.0f;
	std::atomic<float> m_EffectiveBeltHeightOffsetCm{ 78.0f };
	std::atomic<bool> m_MeasureHeightRequested{ false };
	bool m_ShowBatteryCounts = true;
	bool m_AllowQuickReload = false;
	bool m_ShowFlashlightBatteryRing = true;
	float m_CameraFlashBlindingStrength = 1.0f;
	CameraFlashMode m_CameraFlashMode = CameraFlashMode::Auto;

	bool m_KeepActiveInBackground = true;
	bool m_VRAMOffload = true;
	bool m_DebugFingerLog = false;
	bool m_EnableProfilerHotkey = false;
	std::string m_SpatialAudioMode = "steam_audio";
	bool m_SpatialAudioOcclusion = true;
	float m_SpatialAudioRoomSend = 1.0f;
	std::atomic<int> m_SkeletalTrackingStateLeft{ -1 };
	std::atomic<int> m_SkeletalTrackingStateRight{ -1 };
	int m_DesktopWindowWidth = 1280;
	int m_DesktopWindowHeight = 720;
	static inline std::string ResolveGameRelativePath(const char* relPath)
	{
		if (!relPath || !relPath[0])
			return "";
		if ((relPath[0] == '\\' && relPath[1] == '\\') || (relPath[0] != '\0' && relPath[1] == ':'))
			return relPath;

		char exePath[MAX_PATH] = {};
		DWORD len = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
		if (len > 0 && len < MAX_PATH)
		{
			std::string dir(exePath, len);
			size_t slash = dir.find_last_of("\\/");
			if (slash != std::string::npos)
			{
				dir.resize(slash + 1);
				dir += relPath;
				return dir;
			}
		}
		return relPath;
	}
	static inline float ComputeImpactHapticAmplitude(float speedMps, float minAmp, float maxSpeedMps = 2.3f)
	{
		if (maxSpeedMps <= 0.05f) maxSpeedMps = 2.3f;
		float t = speedMps / maxSpeedMps;
		if (t < 0.0f) t = 0.0f;
		if (t > 1.0f) t = 1.0f;
		const float curved = t * (0.45f + 0.55f * t);
		float amp = minAmp + (1.0f - minAmp) * curved;
		if (amp < minAmp) amp = minAmp;
		if (amp > 1.0f) amp = 1.0f;
		return amp;
	}
	static inline const char* BatteryInventoryModeToString(BatteryInventoryMode mode)
	{
		switch (mode)
		{
		case BatteryInventoryMode::Wrist: return "wrist";
		case BatteryInventoryMode::Both:  return "both";
		case BatteryInventoryMode::Off:   return "off";
		case BatteryInventoryMode::Belt:
		default:                          return "belt";
		}
	}
	static inline BatteryInventoryMode ParseBatteryInventoryMode(const std::string& val)
	{
		std::string lower = val;
		for (char& c : lower)
			if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
		if (lower == "wrist" || lower == "hand") return BatteryInventoryMode::Wrist;
		if (lower == "both" || lower == "all")   return BatteryInventoryMode::Both;
		if (lower == "off" || lower == "none" || lower == "disabled") return BatteryInventoryMode::Off;
		return BatteryInventoryMode::Belt;
	}
	static inline const char* CameraFlashModeToString(CameraFlashMode mode)
	{
		switch (mode)
		{
		case CameraFlashMode::Off:  return "Off";
		case CameraFlashMode::On:   return "On";
		case CameraFlashMode::Auto:
		default:                    return "Auto";
		}
	}
	inline void TriggerHapticPulseOnHand(bool physicalLeftHand, unsigned short legacyDurationUs, float amplitude = 0.6f)
	{
		const vr::VRActionHandle_t vibAct = physicalLeftHand ? m_ActionVibrationLeft : m_ActionVibrationRight;
		if (m_Input && vibAct != vr::k_ulInvalidActionHandle)
		{
			const float rawDur = static_cast<float>(legacyDurationUs) * 1e-6f * 8.0f;
			const float durSec = (rawDur > 0.005f) ? rawDur : 0.005f;
			if (m_Input->TriggerHapticVibrationAction(vibAct, 0.0f, durSec, 160.0f, amplitude, vr::k_ulInvalidInputValueHandle) == vr::VRInputError_None)
				return;
		}
		if (m_System)
		{
			vr::TrackedDeviceIndex_t devIdx = m_System->GetTrackedDeviceIndexForControllerRole(
				physicalLeftHand ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand);
			if (devIdx >= vr::k_unMaxTrackedDeviceCount)
				devIdx = physicalLeftHand ? m_LeftControllerIndex : m_RightControllerIndex;
			if (devIdx < vr::k_unMaxTrackedDeviceCount)
				m_System->TriggerHapticPulse(devIdx, 0, legacyDurationUs);
		}
	}
	void SetScreenSizeOverride(bool bState);
	void CreateVRTextures();
	void SubmitVRTextures(bool renderedNewFrame, const VRFrameData& frame);
	void RepositionOverlays(const vr::TrackedDevicePose_t& hmdPose);
	void GetPoses();
	void UpdatePosesAndActions();
	void GetViewParameters();
	void ProcessMenuInput();
	void ProcessInput();
	void OpenPhonePauseMenu();
	void ClosePhonePauseMenu();
	VMatrix VMatrixFromHmdMatrix(const vr::HmdMatrix34_t &hmdMat);
	vr::HmdMatrix34_t VMatrixToHmdMatrix(const VMatrix &vMat);
	vr::HmdMatrix34_t GetControllerTipMatrix(vr::ETrackedControllerRole controllerRole);
	bool CheckOverlayIntersectionForController(vr::VROverlayHandle_t overlayHandle, vr::ETrackedControllerRole controllerRole);
	QAngle GetLeftControllerAbsAngle();
	QAngle& GetLeftControllerAbsAngleConst();
	Vector GetLeftControllerAbsPos(Vector eyePosition = {0, 0, 0});
	QAngle GetRightControllerAbsAngle();
	QAngle& GetRightControllerAbsAngleConst();
	Vector GetRightControllerAbsPos(Vector eyePosition = {0, 0, 0});
	Vector GetRecommendedViewmodelAbsPos(Vector eyePosition);
	QAngle GetRecommendedViewmodelAbsAngle();
	void UpdateHMDAngles();
	void UpdateTracking();
	Vector GetViewAngle();
	Vector GetViewOrigin(Vector setupOrigin);
	Vector GetViewOriginLeft(Vector setupOrigin);
	Vector GetViewOriginRight(Vector setupOrigin);
	bool PressedDigitalAction(vr::VRActionHandle_t &actionHandle, bool checkIfActionChanged = false);
	bool GetAnalogActionData(vr::VRActionHandle_t &actionHandle, vr::InputAnalogActionData_t &analogDataOut);
	void ResetPosition();
	void GetPoseData(vr::TrackedDevicePose_t &poseRaw, TrackedDevicePoseData &poseOut);
	void RecomputeEffectiveBeltHeight();
	float GetEffectiveBeltHeightOffsetCm() const { return m_EffectiveBeltHeightOffsetCm.load(std::memory_order_acquire); }
	bool MeasureStandingPlayerHeight();
	bool CalibrateBeltHeightFromController(bool useLeftController);
	void ParseConfigFile();
	void SaveConfigFile();
	void WaitForConfigUpdate();
	Vector Trace(uint32_t* localPlayer);
	Vector TraceEye(uint32_t* localPlayer, Vector cameraPos, Vector eyePos, QAngle& eyeAngle);
};

QAngle TransformAnglesToWorldSpace(const QAngle& angles, const matrix3x4_t& parentMatrix);
QAngle TransformAnglesToLocalSpace(const QAngle& worldAngles, const matrix3x4_t& parentMatrix);