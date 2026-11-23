#pragma once

#include "sdk.h"

class ITexture;

namespace VRCameraFlash
{
	bool EvaluateShouldFlash(void* cameraThis, bool origShouldFlash);
	void OnPhotoTaken(void* cameraThis);
	bool GetProjectedLightOverride(Vector& outPos, Vector& outForward, float& outIntensity);
	void UpdateFrame(const Vector& eyeOrigin, const QAngle& eyeAngles);
	bool IsFlashActive();
	float GetScreenWhiteoutOverride();
	void DrawCameraFlashGlare(const CViewSetup& eyeView, ITexture* eyeTexture);
	void Reset();
}
