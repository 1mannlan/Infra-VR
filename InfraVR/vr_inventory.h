#pragma once

#include "sdk.h"

class ITexture;

namespace VRInventory
{
	struct ServerSnapshot
	{
		bool valid = false;
		bool hasFlashlight = false;
		bool hasSuperFlashlight = false;
		bool hasCamera = false;
		bool flashlightBroken = false;
		int flashlightBattery = 100;   // 0..100 (CINFRA_Player::m_iFlashlightBattery)
		int cameraBattery = 100;       // 0..100 (CINFRACamera::m_iClip1)
		int dBatteryReserve = 0;       // "Flashlight" ammo reserve
		int aaBatteryReserve = 0;      // "Camera" ammo reserve
	};

	struct PendingActions
	{
		bool equipFlashlight = false;
		bool holsterFlashlight = false;
		bool equipCamera = false;
		bool holsterCamera = false;
		bool instantReloadCamera = false;
		bool instantReloadFlashlight = false;
	};

	void PublishServerSnapshot(const ServerSnapshot& snap);
	ServerSnapshot GetSnapshot();
	PendingActions ConsumePendingActions();

	void UpdateFrame(const Vector& eyeOrigin, const QAngle& eyeAngles);

	void BeginEyeRender(int eye);
	bool ShouldDrawWorldModelsNow(const char* modelName, const ModelRenderInfo_t& info);
	void DrawWorldModelsInDME(void* ecx, void* context, void* state, const ModelRenderInfo_t& pInfo, matrix3x4_t* pCustomBoneToWorld);

	void DrawEyeOverlays(const CViewSetup& eyeView, ITexture* eyeTexture);

	bool IsHandHoldingBattery(bool physicalLeftHand);
	bool IsHandInInventoryZone(bool physicalLeftHand);
	void Reset();
}
