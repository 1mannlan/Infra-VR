#pragma once
#include <iostream>
#include "MinHook.h"
#include "bitbuf.h"

class Game;
class VR;
class ITexture;
class CViewSetup;
class CUserCmd;
class QAngle;
class Vector;
class edict_t;
class ModelRenderInfo_t;
class IMatRenderContext;
class bf_write;
class bf_read;


template <typename T>
struct Hook {
	T fOriginal = nullptr;
	LPVOID pTarget = nullptr;
	bool isEnabled = false;

	int createHook(LPVOID targetFunc, LPVOID detourFunc)
	{
		if (MH_CreateHook(targetFunc, detourFunc, reinterpret_cast<LPVOID *>(&fOriginal)) != MH_OK)
		{
			char errorString[512];
			sprintf_s(errorString, 512, "Failed to create hook with this signature: %s", typeid(T).name());
			Game::errorMsg(errorString);
			return 1;
		}
		pTarget = targetFunc;
		return 0;
	}

	int enableHook()
	{
		if (!pTarget)
			throw std::invalid_argument("pTarget is empty, did you miss a call to createHook?");

		MH_STATUS status = MH_EnableHook(pTarget);
		if (status != MH_OK)
		{
			char errorString[256];
			sprintf_s(errorString, 256, "Failed to enable hook: %i", status);
			Game::errorMsg(errorString);
			return 1;
		}
		isEnabled = true;
		return 0;
	}

	int disableHook()
	{
		if (MH_DisableHook(pTarget) != MH_OK)
		{
			Game::errorMsg("Failed to disable hook");
			return 1;
		}
		isEnabled = false;
		return 0;
	}
};


// Source Engine functions
typedef void(__thiscall *tRenderView)(void *thisptr, CViewSetup &setup, CViewSetup &hudViewSetup, int nClearFlags, int whatToDraw);
typedef void(__thiscall *tDrawViewModels)(void *thisptr, CViewSetup &viewViewmodel, bool drawViewmodel);
typedef bool(__thiscall *tCreateMove)(void *thisptr, float flInputSampleTime, CUserCmd *cmd);
typedef void(__thiscall *tCalcViewModelView)(void *thisptr, const Vector &eyePosition, const QAngle &eyeAngles);
typedef float(__thiscall *tProcessUsercmds)(void *thisptr, edict_t *player, void *buf, int numcmds, int totalcmds, int dropped_packets, bool ignore, bool paused);
typedef int(__cdecl *tReadUsercmd)(void *buf, CUserCmd *move, CUserCmd *from);
typedef int(__cdecl *tWriteUsercmd)(void *buf, CUserCmd *to, CUserCmd *from);
typedef Vector *(__thiscall *tEyePosition)(void *thisptr, Vector *eyePos);
typedef void(__thiscall *tDrawModelExecute)(void *thisptr, void *ctx, void *state, const ModelRenderInfo_t &info, void *pCustomBoneToWorld);

typedef Vector* (__thiscall* tWeapon_ShootPosition)(void* thisptr, Vector* shootPos);
typedef bool(__thiscall* tUpdateObject)(void* thisptr, void* pPlayer, float flError, bool bIsTeleport);
typedef bool(__thiscall* tUpdateObjectVM)(void* thisptr, void* pPlayer, float flError);
typedef QAngle&(__thiscall* tEyeAngles)(void* thisptr);

typedef double(__thiscall* tGetDefaultFOV)(void* thisptr);
typedef double(__thiscall* tGetFOV)(void* thisptr);
typedef double(__thiscall* tGetViewModelFOV)(void* thisptr);

typedef bool(__thiscall* tCHudCrosshair_ShouldDraw)(void* thisptr);

typedef int(__thiscall* tEntindex)(void* thisptr);
typedef void*(__thiscall* tGetOwner)(void* thisptr);

// INFRA-specific VR hooks
typedef void(__thiscall* tUpdateFlashlight)(void* thisptr);
typedef QAngle&(__thiscall* tClientEyeAngles)(void* thisptr);
typedef Vector*(__thiscall* tClientEyePosition)(void* thisptr, Vector* eyePos);
typedef void(__thiscall* tClientCameraThink)(void* thisptr);
typedef bool(__cdecl* tShouldUsePhonePause)();
typedef void*(__thiscall* tFindUseEntity)(void* thisptr);
typedef void(__thiscall* tCameraPrimaryAttack)(void* thisptr);
typedef void(__thiscall* tCameraSecondaryAttack)(void* thisptr);
typedef void(__thiscall* tCameraItemPostFrame)(void* thisptr);
typedef bool(__thiscall* tCameraShouldFlash)(void* thisptr);
typedef void(__thiscall* tCameraOnPhotoTaken)(void* thisptr);
typedef void(__thiscall* tCameraFlashlightUpdateLight)(void* thisptr, Vector pos, float qx, float qy, float qz, float qw);
typedef void(__thiscall* tCameraScreenOnTick)(void* thisptr);
typedef void(__thiscall* tHandsItemPostFrame)(void* thisptr);
typedef double(__thiscall* tGetCarryDistance)(void* thisptr);
typedef double(__thiscall* tGetCarryVerticalOffset)(void* thisptr, void* pObject);
typedef void(__thiscall* tFlashlightHideThink)(void* thisptr);
typedef bool(__thiscall* tViewModelShouldDraw)(void* thisptr);
typedef bool(__thiscall* tReloadFlashlight)(void* thisptr);
typedef void(__thiscall* tGetCursorPosition)(void* thisptr, int* pX, int* pY);
typedef bool(__thiscall* tVGuiScreenGetCursorPos)(void* thisptr, int* pX, int* pY);
typedef void*(__cdecl* tFindVGuiScreenUnderCursor)(const Vector& vecOrigin, const QAngle& vecAngles, float flFOV);
typedef void(__thiscall* tGrabControllerSetTargetPosition)(void* thisptr, const Vector& target, const QAngle& targetOrientation, bool bIsTeleport);
typedef void(__thiscall* tTextureDownload)(void* thisptr, void* pRect, int nAdditionalCreationFlags);

class Hooks
{
public:
	static inline Game *m_Game;
	static inline VR *m_VR;

	static inline Hook<tRenderView> hkRenderView;
	static inline Hook<tDrawViewModels> hkDrawViewModels;
	static inline Hook<tCreateMove> hkCreateMove;
	static inline Hook<tCalcViewModelView> hkCalcViewModelView;
	static inline Hook<tProcessUsercmds> hkProcessUsercmds;
	static inline Hook<tReadUsercmd> hkReadUsercmd;
	static inline Hook<tWriteUsercmd> hkWriteUsercmd;
	static inline Hook<tEyePosition> hkEyePosition;
	static inline Hook<tDrawModelExecute> hkDrawModelExecute;
	static inline Hook<tWeapon_ShootPosition> hkWeapon_ShootPosition;

	static inline Hook<tUpdateObject> hkUpdateObject;
	static inline Hook<tUpdateObjectVM> hkUpdateObjectVM;
	static inline Hook<tEyeAngles> hkEyeAngles;

	static inline Hook<tGetDefaultFOV> hkGetDefaultFOV;
	static inline Hook<tGetFOV> hkGetFOV;
	static inline Hook<tGetViewModelFOV> hkGetViewModelFOV;

	static inline Hook<tCHudCrosshair_ShouldDraw> hkCHudCrosshair_ShouldDraw;

	// INFRA-specific VR Hooks
	static inline Hook<tUpdateFlashlight> hkUpdateFlashlight;
	static inline Hook<tClientEyeAngles> hkClientEyeAngles;
	static inline Hook<tClientEyePosition> hkClientEyePosition;
	static inline Hook<tClientCameraThink> hkClientCameraThink;
	static inline Hook<tShouldUsePhonePause> hkShouldUsePhonePause;
	static inline Hook<tFindUseEntity> hkClientFindUseEntity;
	static inline Hook<tFindUseEntity> hkFindUseEntity;
	static inline Hook<tCameraPrimaryAttack> hkCameraPrimaryAttack;
	static inline Hook<tCameraSecondaryAttack> hkCameraSecondaryAttack;
	static inline Hook<tCameraItemPostFrame> hkCameraItemPostFrame;
	static inline Hook<tHandsItemPostFrame> hkHandsItemPostFrame;
	static inline Hook<tGetCarryDistance> hkGetCarryDistance;
	static inline Hook<tGetCarryVerticalOffset> hkGetCarryVerticalOffset;
	static inline Hook<tFlashlightHideThink> hkFlashlightHideThink;
	static inline Hook<tViewModelShouldDraw> hkViewModelShouldDraw;
	static inline Hook<tCameraShouldFlash> hkCameraShouldFlash;
	static inline Hook<tCameraOnPhotoTaken> hkCameraOnPhotoTaken;
	static inline Hook<tCameraFlashlightUpdateLight> hkCameraFlashlightUpdateLight;
	static inline Hook<tCameraScreenOnTick> hkCameraScreenOnTick;
	static inline Hook<tReloadFlashlight> hkReloadFlashlight;
	static inline Hook<tGetCursorPosition> hkGetCursorPosition;
	static inline Hook<tVGuiScreenGetCursorPos> hkVGuiScreenGetCursorPos;
	static inline Hook<tFindVGuiScreenUnderCursor> hkFindVGuiScreenUnderCursor;
	static inline Hook<tGrabControllerSetTargetPosition> hkGrabControllerSetTargetPosition;
	static inline Hook<tTextureDownload> hkTextureDownload;

	Hooks() {};
	Hooks(Game *game);

	~Hooks();

	int initSourceHooks();

	// Shared helpers
	static const char* SelectBareHandModel(int* outModelIndex = nullptr);
	static void EnsureServerBareHandViewModel(void* pWeapon, void* pVm0);
	static void InstallTextureDownloadHook(ITexture* pTex);

	// Detour functions
	static void __fastcall dRenderView(void *ecx, void *edx, CViewSetup &setup, CViewSetup &hudViewSetup, int nClearFlags, int whatToDraw);
	static void __fastcall dDrawViewModels(void *ecx, void *edx, CViewSetup &viewViewmodel, bool drawViewmodel);
	static bool __fastcall dCreateMove(void *ecx, void *edx, float flInputSampleTime, CUserCmd *cmd);
	static void __fastcall dCalcViewModelView(void *ecx, void *edx, const Vector &eyePosition, const QAngle &eyeAngles);
	static float __fastcall dProcessUsercmds(void *ecx, void *edx, edict_t *player, void *buf, int numcmds, int totalcmds, int dropped_packets, bool ignore, bool paused);
	static int dReadUsercmd(bf_read *buf, CUserCmd *move, CUserCmd *from);
	static int dWriteUsercmd(bf_write *buf, CUserCmd *to, CUserCmd *from);
	static Vector *__fastcall dEyePosition(void *ecx, void *edx, Vector *eyePos);
	static void __fastcall dDrawModelExecute(void *ecx, void* edx, void *ctx, void *state, const ModelRenderInfo_t &info, void *pCustomBoneToWorld);

	// Grabbable objects & weapons
	static Vector* __fastcall dWeapon_ShootPosition(void* ecx, void* edx, Vector* shootPos);
	static bool __fastcall dUpdateObject(void* ecx, void* edx, void* pPlayer, float flError, bool bIsTeleport = false);
	static bool __fastcall dUpdateObjectVM(void* ecx, void* edx, void* pPlayer, float flError);
	static QAngle& __fastcall dEyeAngles(void* ecx, void* edx);

	static double __fastcall dGetDefaultFOV(void* ecx, void* edx);
	static double __fastcall dGetFOV(void* ecx, void* edx);
	static double __fastcall dGetViewModelFOV(void* ecx, void* edx);

	static bool __fastcall dCHudCrosshair_ShouldDraw(void* ecx, void* edx);

	// INFRA-specific VR Detours
	static void __fastcall dUpdateFlashlight(void* ecx, void* edx);
	static QAngle& __fastcall dClientEyeAngles(void* ecx, void* edx);
	static Vector* __fastcall dClientEyePosition(void* ecx, void* edx, Vector* eyePos);
	static void __fastcall dClientCameraThink(void* ecx, void* edx);
	static bool __cdecl dShouldUsePhonePause();
	static void* __fastcall dClientFindUseEntity(void* ecx, void* edx);
	static void* __fastcall dFindUseEntity(void* ecx, void* edx);
	static void __fastcall dCameraPrimaryAttack(void* ecx, void* edx);
	static void __fastcall dCameraSecondaryAttack(void* ecx, void* edx);
	static void __fastcall dCameraItemPostFrame(void* ecx, void* edx);
	static void __fastcall dHandsItemPostFrame(void* ecx, void* edx);
	static double __fastcall dGetCarryDistance(void* ecx, void* edx);
	static double __fastcall dGetCarryVerticalOffset(void* ecx, void* edx, void* pObject);
	static void __fastcall dFlashlightHideThink(void* ecx, void* edx);
	static bool __fastcall dViewModelShouldDraw(void* ecx, void* edx);
	static bool __fastcall dCameraShouldFlash(void* ecx, void* edx);
	static void __fastcall dCameraOnPhotoTaken(void* ecx, void* edx);
	static void __fastcall dCameraFlashlightUpdateLight(void* ecx, void* edx, Vector pos, float qx, float qy, float qz, float qw);
	static void __fastcall dCameraScreenOnTick(void* ecx, void* edx);
	static bool __fastcall dReloadFlashlight(void* ecx, void* edx);
	static void __fastcall dGetCursorPosition(void* ecx, void* edx, int* pX, int* pY);
	static bool __fastcall dVGuiScreenGetCursorPos(void* ecx, void* edx, int* pX, int* pY);
	static void* __cdecl dFindVGuiScreenUnderCursor(const Vector& vecOrigin, const QAngle& vecAngles, float flFOV);
	static void __fastcall dGrabControllerSetTargetPosition(void* ecx, void* edx, const Vector& target, const QAngle& targetOrientation, bool bIsTeleport);
	static void __fastcall dTextureDownload(void* ecx, void* edx, void* pRect, int nAdditionalCreationFlags);

	static inline int m_PushHUDStep;
	static inline bool m_PushedHud;

	static inline tEntindex EntityIndex;
	static inline tGetOwner GetOwner;
};
