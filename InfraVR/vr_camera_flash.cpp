#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include "vr_camera_flash.h"
#include "vr.h"
#include "vr_hands.h"
#include "vr_grab.h"
#include "game.h"
#include "sdk.h"
#include "trace.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace
{
	constexpr float kPi = 3.14159265358979323846f;
	constexpr float kDeg2Rad = kPi / 180.0f;

	// Xenon flash burst envelope timings (milliseconds)
	constexpr float kFlashPlateauMs = 45.0f;
	constexpr float kFlashTotalDurationMs = 160.0f;
	constexpr float kFlashDecaySteepness = 5.0f;

	// Retinal whiteout afterimage decay time constant (reaches 0 cleanly around ~1.1..1.2s)
	constexpr float kAfterimageTauSec = 0.42f;
	constexpr float kAfterimageTailDrainPerSec = 0.045f;
	constexpr float kMaxBlindDistUnits = 85.0f; // ~2.0 m at VRScale = 43.2

	// Verified client.dll RVAs (ImageBase = 0x10000000)
	constexpr uintptr_t kRvaGpGlobals = 0x5E5704u;
	constexpr uintptr_t kGpGlobalsOffCurtime = 0x0Cu;

	// Verified client.dll ConVar RVAs (m_pParent +0x1C, m_fValue +0x2C, m_nValue +0x30)
	constexpr uintptr_t kCvarFlashTime       = 0x6D4858u; // infra_camera_flash_time
	constexpr uintptr_t kCvarFlashEnable     = 0x6D48B0u; // infra_camera_flash
	constexpr uintptr_t kCvarFlashDist       = 0x6D49D8u; // infra_camera_flashlight_distance
	constexpr uintptr_t kCvarFlashFov        = 0x6D4A30u; // infra_camera_flashlight_fov
	constexpr uintptr_t kCvarFlashR          = 0x6D4A88u; // infra_camera_flashlight_r
	constexpr uintptr_t kCvarFlashG          = 0x6D4AE0u; // infra_camera_flashlight_g
	constexpr uintptr_t kCvarFlashB          = 0x6D4B38u; // infra_camera_flashlight_b
	constexpr uintptr_t kCvarFlashQuadAtten  = 0x6D4BE8u; // infra_camera_flashlight_quadratic_attenuation
	constexpr uintptr_t kCvarFlashLinAtten   = 0x6D4C40u; // infra_camera_flashlight_linear_attenuation
	constexpr uintptr_t kCvarFlashVolumetric = 0x6D4C98u; // infra_camera_flashlight_volumetric
	constexpr uintptr_t kCvarFlashVolInten   = 0x6D4CF0u; // infra_camera_flashlight_volumetric_intensity

	constexpr int kNumFlashConVars = 11;

	struct SavedConVarSlot
	{
		uintptr_t rva = 0;
		bool hasSelf = false;
		float origSelfFloat = 0.0f;
		int32_t origSelfInt = 0;
		bool hasParent = false;
		uintptr_t parentAddr = 0;
		float origParentFloat = 0.0f;
		int32_t origParentInt = 0;
	};

	// Verified Source Engine dlight_t layout (0x44 bytes in INFRA engine.dll CL_AllocDlight)
	struct ColorRGBExp32
	{
		uint8_t r;
		uint8_t g;
		uint8_t b;
		int8_t exponent;
	};
	static_assert(sizeof(ColorRGBExp32) == 4);

	struct dlight_t
	{
		int flags;            // +0x00
		Vector origin;        // +0x04
		float radius;         // +0x10
		ColorRGBExp32 color;  // +0x14
		float die;            // +0x18
		float decay;          // +0x1C
		float minlight;       // +0x20
		int key;              // +0x24
		int style;            // +0x28
		Vector m_Direction;   // +0x2C
		float m_InnerAngle;   // +0x38
		float m_OuterAngle;   // +0x3C
		uint8_t pad_0040[4];  // +0x40 (engine.dll stride is 0x44)
	};
	static_assert(sizeof(dlight_t) == 0x44);

	struct PinnedMaterialSlot
	{
		IMaterial* mat = nullptr;
		unsigned long long lastRetryMs = 0;

		IMaterial* Get(IMaterialSystem* matSys, const char* name, const char* group)
		{
			if (mat)
				return mat;
			if (!matSys)
				return nullptr;
			const unsigned long long nowMs = GetTickCount64();
			if (lastRetryMs != 0 && (nowMs - lastRetryMs) < 1000ULL)
				return nullptr;
			lastRetryMs = nowMs;
			IMaterial* found = matSys->FindMaterial(name, group, false);
			if (found && !found->IsErrorMaterial())
			{
				found->IncrementReferenceCount();
				mat = found;
				return mat;
			}
			return nullptr;
		}
	};

	struct FlashState
	{
		bool hasEvaluatedShouldFlash = false;
		bool lastEvaluatedShouldFlash = false;
		void* lastEvalCameraThis = nullptr;
		std::chrono::steady_clock::time_point lastEvalTime{};

		bool pendingAutoBurst = false;
		void* pendingAutoCameraThis = nullptr;
		std::chrono::steady_clock::time_point pendingAutoTime{};

		bool flashActive = false;
		std::chrono::steady_clock::time_point flashStartTime{};
		float currentIntensity = 0.0f;

		float retinalAfterimage = 0.0f;
		bool hasPrevFrameTime = false;
		std::chrono::steady_clock::time_point prevFrameTime{};

		bool hasCachedEmitterPose = false;
		Vector cachedFlashPos{ 0.0f, 0.0f, 0.0f };
		Vector cachedFlashNormal{ 1.0f, 0.0f, 0.0f };

		uintptr_t savedClientBase = 0;
		bool conVarsSaved = false;
		bool conVarsModified = false;
		SavedConVarSlot savedConVars[kNumFlashConVars]{};
	};

	std::mutex g_FlashMutex;
	FlashState g_State;
	PinnedMaterialSlot g_CameraGlowMatSlot;
	PinnedMaterialSlot g_FallbackGlowMatSlot;
	void* g_CachedEffectsInterface = nullptr;

	inline float ClampF(float v, float lo, float hi)
	{
		return (v < lo) ? lo : ((v > hi) ? hi : v);
	}

	inline float SmoothStep01(float t)
	{
		t = ClampF(t, 0.0f, 1.0f);
		return t * t * (3.0f - 2.0f * t);
	}

	inline float Dot3(const Vector& a, const Vector& b)
	{
		return a.x * b.x + a.y * b.y + a.z * b.z;
	}

	inline Vector NormalizeSafe(const Vector& v, const Vector& fallback)
	{
		const float lenSq = Dot3(v, v);
		if (lenSq > 1e-8f)
		{
			const float inv = 1.0f / std::sqrt(lenSq);
			return Vector(v.x * inv, v.y * inv, v.z * inv);
		}
		return fallback;
	}

	inline bool IsFiniteVec(const Vector& v)
	{
		return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
	}

	inline bool HasModuleRange(uintptr_t base, uintptr_t rva, size_t bytes)
	{
		if (!base)
			return false;
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x1000)
			return false;
		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + static_cast<uintptr_t>(dos->e_lfanew));
		if (nt->Signature != IMAGE_NT_SIGNATURE)
			return false;
		const size_t imgSize = static_cast<size_t>(nt->OptionalHeader.SizeOfImage);
		return (rva < imgSize) && (bytes <= (imgSize - rva));
	}

	inline bool IsReadableMemory(uintptr_t addr, size_t bytes)
	{
		if (!addr)
			return false;
		MEMORY_BASIC_INFORMATION mbi{};
		if (VirtualQuery(reinterpret_cast<const void*>(addr), &mbi, sizeof(mbi)) != sizeof(mbi))
			return false;
		if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0)
			return false;
		const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
		return addr >= reinterpret_cast<uintptr_t>(mbi.BaseAddress) && (regionEnd - addr) >= bytes;
	}

	inline bool IsWritableMemory(uintptr_t addr, size_t bytes)
	{
		if (!addr)
			return false;
		MEMORY_BASIC_INFORMATION mbi{};
		if (VirtualQuery(reinterpret_cast<const void*>(addr), &mbi, sizeof(mbi)) != sizeof(mbi))
			return false;
		const DWORD writableMask = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
		if (mbi.State != MEM_COMMIT || (mbi.Protect & writableMask) == 0 || (mbi.Protect & PAGE_GUARD) != 0)
			return false;
		const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
		return addr >= reinterpret_cast<uintptr_t>(mbi.BaseAddress) && (regionEnd - addr) >= bytes;
	}

	float EvaluateXenonEnvelopeMs(float elapsedMs)
	{
		if (elapsedMs < 0.0f || elapsedMs >= kFlashTotalDurationMs)
			return 0.0f;
		if (elapsedMs <= kFlashPlateauMs)
			return 1.0f;

		const float u = (elapsedMs - kFlashPlateauMs) / (kFlashTotalDurationMs - kFlashPlateauMs);
		const float expEnd = std::exp(-kFlashDecaySteepness);
		const float val = (std::exp(-kFlashDecaySteepness * u) - expEnd) / (1.0f - expEnd);
		return ClampF(val, 0.0f, 1.0f);
	}

	bool ReadClientCurtime(uintptr_t baseClient, float& outCurtime)
	{
		if (!HasModuleRange(baseClient, kRvaGpGlobals, sizeof(uintptr_t)))
			return false;
		const uintptr_t pGlobals = *reinterpret_cast<const uintptr_t*>(baseClient + kRvaGpGlobals);
		if (!IsReadableMemory(pGlobals, kGpGlobalsOffCurtime + sizeof(float)))
			return false;
		const float t = *reinterpret_cast<const float*>(pGlobals + kGpGlobalsOffCurtime);
		if (!std::isfinite(t))
			return false;
		outCurtime = t;
		return true;
	}

	void SaveConVarSlotsLocked(uintptr_t baseClient)
	{
		static constexpr uintptr_t kConVarRvas[kNumFlashConVars] = {
			kCvarFlashTime,
			kCvarFlashEnable,
			kCvarFlashDist,
			kCvarFlashFov,
			kCvarFlashR,
			kCvarFlashG,
			kCvarFlashB,
			kCvarFlashQuadAtten,
			kCvarFlashLinAtten,
			kCvarFlashVolumetric,
			kCvarFlashVolInten,
		};

		if (!baseClient)
			return;
		if (g_State.conVarsSaved && g_State.savedClientBase == baseClient)
			return;

		g_State.savedClientBase = baseClient;
		g_State.conVarsSaved = true;
		g_State.conVarsModified = false;

		for (int i = 0; i < kNumFlashConVars; ++i)
		{
			SavedConVarSlot& slot = g_State.savedConVars[i];
			slot = SavedConVarSlot{};
			slot.rva = kConVarRvas[i];

			if (!HasModuleRange(baseClient, slot.rva, 0x34u))
				continue;

			const uintptr_t self = baseClient + slot.rva;
			if (!IsWritableMemory(self, 0x34u))
				continue;

			slot.hasSelf = true;
			slot.origSelfFloat = *reinterpret_cast<const float*>(self + 0x2Cu);
			slot.origSelfInt = *reinterpret_cast<const int32_t*>(self + 0x30u);

			const uintptr_t parent = *reinterpret_cast<const uintptr_t*>(self + 0x1Cu);
			if (parent != 0u && parent != self && IsWritableMemory(parent, 0x34u))
			{
				slot.hasParent = true;
				slot.parentAddr = parent;
				slot.origParentFloat = *reinterpret_cast<const float*>(parent + 0x2Cu);
				slot.origParentInt = *reinterpret_cast<const int32_t*>(parent + 0x30u);
			}
		}
	}

	void WriteConVarFloatLocked(int slotIndex, float value)
	{
		const SavedConVarSlot& slot = g_State.savedConVars[slotIndex];
		const uintptr_t baseClient = g_State.savedClientBase;
		if (!baseClient || !slot.hasSelf)
			return;

		const uintptr_t self = baseClient + slot.rva;
		const int32_t intVal = static_cast<int32_t>(value);
		*reinterpret_cast<float*>(self + 0x2Cu) = value;
		*reinterpret_cast<int32_t*>(self + 0x30u) = intVal;

		if (slot.hasParent && slot.parentAddr && IsWritableMemory(slot.parentAddr, 0x34u))
		{
			*reinterpret_cast<float*>(slot.parentAddr + 0x2Cu) = value;
			*reinterpret_cast<int32_t*>(slot.parentAddr + 0x30u) = intVal;
		}
	}

	void WriteConVarIntLocked(int slotIndex, int32_t value)
	{
		const SavedConVarSlot& slot = g_State.savedConVars[slotIndex];
		const uintptr_t baseClient = g_State.savedClientBase;
		if (!baseClient || !slot.hasSelf)
			return;

		const uintptr_t self = baseClient + slot.rva;
		const float flVal = static_cast<float>(value);
		*reinterpret_cast<float*>(self + 0x2Cu) = flVal;
		*reinterpret_cast<int32_t*>(self + 0x30u) = value;

		if (slot.hasParent && slot.parentAddr && IsWritableMemory(slot.parentAddr, 0x34u))
		{
			*reinterpret_cast<float*>(slot.parentAddr + 0x2Cu) = flVal;
			*reinterpret_cast<int32_t*>(slot.parentAddr + 0x30u) = value;
		}
	}

	void ApplyFlashConVarsLocked(uintptr_t baseClient, float intensity)
	{
		if (!baseClient)
			return;
		SaveConVarSlotsLocked(baseClient);
		if (!g_State.conVarsSaved)
			return;

		const float clamped = ClampF(intensity, 0.0f, 1.0f);
		WriteConVarFloatLocked(0, 0.16f);             // infra_camera_flash_time
		WriteConVarIntLocked(1, 1);                   // infra_camera_flash
		WriteConVarFloatLocked(2, 1400.0f);           // infra_camera_flashlight_distance
		WriteConVarFloatLocked(3, 135.0f);            // infra_camera_flashlight_fov
		WriteConVarFloatLocked(4, 5.0f * clamped);    // infra_camera_flashlight_r
		WriteConVarFloatLocked(5, 5.2f * clamped);    // infra_camera_flashlight_g
		WriteConVarFloatLocked(6, 5.8f * clamped);    // infra_camera_flashlight_b
		WriteConVarFloatLocked(7, 1200.0f);           // infra_camera_flashlight_quadratic_attenuation
		WriteConVarFloatLocked(8, 12.0f);             // infra_camera_flashlight_linear_attenuation
		WriteConVarIntLocked(9, 1);                   // infra_camera_flashlight_volumetric
		WriteConVarFloatLocked(10, 0.90f * clamped);  // infra_camera_flashlight_volumetric_intensity

		g_State.conVarsModified = true;
	}

	void RestoreFlashConVarsLocked()
	{
		if (!g_State.conVarsSaved || !g_State.conVarsModified)
		{
			g_State.conVarsSaved = false;
			g_State.conVarsModified = false;
			return;
		}

		const uintptr_t baseClient = g_State.savedClientBase;
		if (baseClient && GetModuleHandleA("client.dll") == reinterpret_cast<HMODULE>(baseClient))
		{
			for (int i = 0; i < kNumFlashConVars; ++i)
			{
				const SavedConVarSlot& slot = g_State.savedConVars[i];
				if (slot.hasSelf && HasModuleRange(baseClient, slot.rva, 0x34u))
				{
					const uintptr_t self = baseClient + slot.rva;
					if (IsWritableMemory(self, 0x34u))
					{
						*reinterpret_cast<float*>(self + 0x2Cu) = slot.origSelfFloat;
						*reinterpret_cast<int32_t*>(self + 0x30u) = slot.origSelfInt;
					}
				}
				if (slot.hasParent && slot.parentAddr && IsWritableMemory(slot.parentAddr, 0x34u))
				{
					*reinterpret_cast<float*>(slot.parentAddr + 0x2Cu) = slot.origParentFloat;
					*reinterpret_cast<int32_t*>(slot.parentAddr + 0x30u) = slot.origParentInt;
				}
			}
		}

		g_State.conVarsModified = false;
		g_State.conVarsSaved = false;
		g_State.savedClientBase = 0;
	}

	void RefreshCachedEmitterPoseLocked(VR* vr)
	{
		if (vr && !vr->m_RightHandEmpty && !vr->m_PhoneEquipped &&
		    VRHands::g_ViewModelLens.cameraUpdated &&
		    IsFiniteVec(VRHands::g_ViewModelLens.camFlashPos) &&
		    IsFiniteVec(VRHands::g_ViewModelLens.camFlashNormal))
		{
			g_State.cachedFlashPos = VRHands::g_ViewModelLens.camFlashPos;
			g_State.cachedFlashNormal = NormalizeSafe(
				VRHands::g_ViewModelLens.camFlashNormal,
				g_State.cachedFlashNormal);
			g_State.hasCachedEmitterPose = true;
		}
	}

	void ResolveFlashEmitterPose(VR* vr, Vector& outPos, Vector& outNormal)
	{
		{
			std::lock_guard<std::mutex> lock(g_FlashMutex);
			RefreshCachedEmitterPoseLocked(vr);
			if (vr && !vr->m_RightHandEmpty && !vr->m_PhoneEquipped &&
			    VRHands::g_ViewModelLens.cameraUpdated &&
			    IsFiniteVec(VRHands::g_ViewModelLens.camFlashPos))
			{
				outPos = VRHands::g_ViewModelLens.camFlashPos;
				outNormal = NormalizeSafe(VRHands::g_ViewModelLens.camFlashNormal, g_State.cachedFlashNormal);
				return;
			}
			if (g_State.hasCachedEmitterPose)
			{
				outPos = g_State.cachedFlashPos;
				outNormal = g_State.cachedFlashNormal;
				return;
			}
		}

		if (vr)
		{
			Vector handPos{}, fwd{}, right{}, up{};
			QAngle handAng{};
			VRGrab::GetGimbalFreeControllerBasis(vr, vr->m_LeftHanded, vr->m_SetupOrigin, handPos, fwd, right, up, handAng);
			outNormal = NormalizeSafe(fwd, Vector(1.0f, 0.0f, 0.0f));
			outPos = handPos + outNormal * 2.0f + up * 1.0f;
			return;
		}

		outPos = Vector(0.0f, 0.0f, 0.0f);
		outNormal = Vector(1.0f, 0.0f, 0.0f);
	}

	void SpawnFlashDlight(const Vector& flashPos, const Vector& flashNormal)
	{
		if (!g_Game || !g_Game->m_BaseClient)
			return;

		float curtime = 0.0f;
		if (!ReadClientCurtime(g_Game->m_BaseClient, curtime))
			return;

		if (!g_CachedEffectsInterface)
			g_CachedEffectsInterface = g_Game->GetInterface("engine.dll", "VEngineEffects001");
		if (!g_CachedEffectsInterface)
			return;

		typedef dlight_t* (__thiscall* tCL_AllocDlight)(void* thisptr, int key);
		void** vt = *reinterpret_cast<void***>(g_CachedEffectsInterface);
		if (!vt || !vt[4])
			return;

		constexpr int kFlashDlightKey = -1001;
		auto fnAllocDlight = reinterpret_cast<tCL_AllocDlight>(vt[4]);
		dlight_t* dl = fnAllocDlight(g_CachedEffectsInterface, kFlashDlightKey);
		if (!dl)
			return;

		dl->flags = 0;
		dl->origin = flashPos + flashNormal * 4.0f;
		dl->radius = 520.0f;
		dl->color.r = 255;
		dl->color.g = 248;
		dl->color.b = 240;
		dl->color.exponent = 10;
		dl->die = curtime + 0.060f;
		dl->decay = 7500.0f;
		dl->minlight = 0.0f;
		dl->key = kFlashDlightKey;
		dl->style = 0;
	}

	void TriggerFlashBurstNow(VR* vr, const std::chrono::steady_clock::time_point& now)
	{
		Vector flashPos{}, flashNormal{};
		ResolveFlashEmitterPose(vr, flashPos, flashNormal);

		{
			std::lock_guard<std::mutex> lock(g_FlashMutex);
			g_State.pendingAutoBurst = false;
			g_State.pendingAutoCameraThis = nullptr;
			g_State.flashActive = true;
			g_State.flashStartTime = now;
			g_State.currentIntensity = 1.0f;
			g_State.cachedFlashPos = flashPos;
			g_State.cachedFlashNormal = flashNormal;
			g_State.hasCachedEmitterPose = true;

			if (g_Game && g_Game->m_BaseClient)
				ApplyFlashConVarsLocked(g_Game->m_BaseClient, 1.0f);
		}

		SpawnFlashDlight(flashPos, flashNormal);
		if (vr)
			vr->TriggerHapticPulseOnHand(vr->m_LeftHanded, 900, 0.72f);
	}

	class CTraceFilterWorldOnlyFlash final : public CTraceFilter
	{
	public:
		CTraceFilterWorldOnlyFlash() : CTraceFilter(nullptr, 0) {}
		bool ShouldHitEntity(IHandleEntity*, int) override { return false; }
		TraceType GetTraceType() const override { return TraceType::TRACE_WORLD_ONLY; }
	};

	bool HasFlashLineOfSight(const Vector& eyeOrigin, const Vector& flashPos)
	{
		if (!g_Game || !g_Game->m_EngineTrace)
			return true;

		const Vector delta = flashPos - eyeOrigin;
		const float dist = VectorLength(delta);
		if (dist < 1.5f)
			return true;

		CTraceFilterWorldOnlyFlash filter;
		CGameTrace tr;
		Ray_t ray;
		ray.Init(eyeOrigin, flashPos);
		g_Game->m_EngineTrace->TraceRay(ray, CONTENTS_SOLID, &filter, &tr);

		if (tr.startsolid)
			return true;
		return (1.0f - tr.fraction) * dist <= 1.5f;
	}

	float ComputeEyeBlindingExposure(
		const VR* vr,
		const Vector& eyeOrigin,
		const Vector& eyeForward,
		const Vector& flashPos,
		const Vector& flashNormal,
		float flashIntensity)
	{
		if (!vr || flashIntensity <= 0.001f)
			return 0.0f;

		const float strength = ClampF(vr->m_CameraFlashBlindingStrength, 0.0f, 2.0f);
		if (strength <= 0.001f)
			return 0.0f;

		const Vector toFlash = flashPos - eyeOrigin;
		const float dist = VectorLength(toFlash);
		if (dist > kMaxBlindDistUnits)
			return 0.0f;

		const Vector dirToFlash = (dist > 0.001f) ? (toFlash * (1.0f / dist)) : eyeForward;
		const float facingEye = Dot3(flashNormal, -dirToFlash);
		if (facingEye <= 0.35f)
			return 0.0f;

		const float lookingAt = Dot3(eyeForward, dirToFlash);
		if (lookingAt <= -0.15f)
			return 0.0f;

		if (!HasFlashLineOfSight(eyeOrigin, flashPos))
			return 0.0f;

		const float faceFactor = SmoothStep01((facingEye - 0.35f) / (1.0f - 0.35f));
		const float steepFace = std::pow(faceFactor, 0.7f);
		const float distFactor = 1.0f - SmoothStep01((dist - 12.0f) / (kMaxBlindDistUnits - 12.0f));
		const float lookFactor = 0.30f + 0.70f * SmoothStep01((lookingAt - 0.02f) / 0.85f);
		const float closeFloor = (dist <= 45.0f)
			? (0.95f * flashIntensity * steepFace * lookFactor * std::min(strength, 1.0f))
			: 0.0f;

		return ClampF(std::max(flashIntensity * steepFace * distFactor * lookFactor * strength, closeFloor), 0.0f, 1.0f);
	}
}

namespace VRCameraFlash
{
	bool EvaluateShouldFlash(void* cameraThis, bool origShouldFlash)
	{
		VR* vr = g_Game ? g_Game->m_VR : nullptr;
		if (!vr || !vr->m_IsVREnabled)
			return origShouldFlash;

		bool shouldFlash = origShouldFlash;
		switch (vr->m_CameraFlashMode)
		{
		case VR::CameraFlashMode::Off:
			shouldFlash = false;
			break;
		case VR::CameraFlashMode::On:
			shouldFlash = true;
			break;
		case VR::CameraFlashMode::Auto:
		default:
			shouldFlash = origShouldFlash;
			break;
		}

		const auto now = std::chrono::steady_clock::now();
		bool triggerPendingBurst = false;
		{
			std::lock_guard<std::mutex> lock(g_FlashMutex);
			g_State.hasEvaluatedShouldFlash = true;
			g_State.lastEvaluatedShouldFlash = shouldFlash;
			g_State.lastEvalCameraThis = cameraThis;
			g_State.lastEvalTime = now;

			if (g_State.pendingAutoBurst)
			{
				const float pendingAgeMs = std::chrono::duration<float, std::milli>(now - g_State.pendingAutoTime).count();
				g_State.pendingAutoBurst = false;
				g_State.pendingAutoCameraThis = nullptr;
				if (pendingAgeMs <= 250.0f && shouldFlash)
					triggerPendingBurst = true;
			}
		}

		if (triggerPendingBurst)
			TriggerFlashBurstNow(vr, now);

		return shouldFlash;
	}

	void OnPhotoTaken(void* cameraThis)
	{
		VR* vr = g_Game ? g_Game->m_VR : nullptr;
		if (!vr || !vr->m_IsVREnabled)
			return;

		if (vr->m_CameraFlashMode == VR::CameraFlashMode::Off)
		{
			std::lock_guard<std::mutex> lock(g_FlashMutex);
			g_State.pendingAutoBurst = false;
			g_State.pendingAutoCameraThis = nullptr;
			return;
		}

		const auto now = std::chrono::steady_clock::now();
		if (vr->m_CameraFlashMode == VR::CameraFlashMode::On)
		{
			TriggerFlashBurstNow(vr, now);
			return;
		}

		// Auto mode: ClientCameraThink often evaluates ShouldFlash right after OnPhotoTaken sets m_bFlashing.
		// If a fresh evaluation from this shot is already available, honor it immediately; otherwise wait up to ~35 ms
		// for EvaluateShouldFlash to confirm (with safety fallback in UpdateFrame if uncalled).
		bool fireNow = false;
		{
			std::lock_guard<std::mutex> lock(g_FlashMutex);
			if (g_State.hasEvaluatedShouldFlash && g_State.lastEvalCameraThis == cameraThis)
			{
				const float evalAgeMs = std::chrono::duration<float, std::milli>(now - g_State.lastEvalTime).count();
				if (evalAgeMs <= 60.0f)
				{
					g_State.pendingAutoBurst = false;
					if (!g_State.lastEvaluatedShouldFlash)
						return;
					fireNow = true;
				}
			}
			if (!fireNow)
			{
				g_State.pendingAutoBurst = true;
				g_State.pendingAutoCameraThis = cameraThis;
				g_State.pendingAutoTime = now;
			}
		}

		if (fireNow)
			TriggerFlashBurstNow(vr, now);
	}

	bool GetProjectedLightOverride(Vector& outPos, Vector& outForward, float& outIntensity)
	{
		VR* vr = g_Game ? g_Game->m_VR : nullptr;
		if (!vr || !vr->m_IsVREnabled)
			return false;

		const auto now = std::chrono::steady_clock::now();
		float intensity = 0.0f;
		{
			std::lock_guard<std::mutex> lock(g_FlashMutex);
			if (g_State.flashActive)
			{
				const float elapsedMs = std::chrono::duration<float, std::milli>(now - g_State.flashStartTime).count();
				intensity = EvaluateXenonEnvelopeMs(elapsedMs);
			}
			else if (g_State.pendingAutoBurst)
			{
				const float pendingAgeMs = std::chrono::duration<float, std::milli>(now - g_State.pendingAutoTime).count();
				if (pendingAgeMs <= 80.0f)
					intensity = 1.0f;
			}
		}

		if (intensity <= 0.001f)
			return false;

		Vector flashPos{}, flashNormal{};
		ResolveFlashEmitterPose(vr, flashPos, flashNormal);
		outForward = NormalizeSafe(flashNormal, Vector(1.0f, 0.0f, 0.0f));
		outPos = flashPos + outForward * 1.5f;
		outIntensity = intensity;
		return true;
	}

	void UpdateFrame(const Vector& eyeOrigin, const QAngle& eyeAngles)
	{
		VR* vr = g_Game ? g_Game->m_VR : nullptr;
		const bool inActiveGameplay = vr && vr->m_IsVREnabled && g_Game && g_Game->m_EngineClient &&
			g_Game->m_EngineClient->IsInGame() &&
			!g_Game->m_EngineClient->IsLevelMainMenuBackground() &&
			!vr->m_In3DViewControlMenu;

		if (!inActiveGameplay)
		{
			Reset();
			return;
		}

		const auto now = std::chrono::steady_clock::now();

		// Expire Auto-mode pending burst after 90 ms; only fire if ShouldFlash already evaluated true.
		bool triggerFallbackBurst = false;
		{
			std::lock_guard<std::mutex> lock(g_FlashMutex);
			RefreshCachedEmitterPoseLocked(vr);
			if (g_State.pendingAutoBurst)
			{
				const float pendingAgeMs = std::chrono::duration<float, std::milli>(now - g_State.pendingAutoTime).count();
				if (pendingAgeMs >= 90.0f)
				{
					g_State.pendingAutoBurst = false;
					g_State.pendingAutoCameraThis = nullptr;
					const float evalAgeMs = g_State.hasEvaluatedShouldFlash
						? std::chrono::duration<float, std::milli>(now - g_State.lastEvalTime).count()
						: 1e9f;
					if (g_State.hasEvaluatedShouldFlash && evalAgeMs <= 250.0f && g_State.lastEvaluatedShouldFlash)
						triggerFallbackBurst = true;
				}
			}
		}
		if (triggerFallbackBurst)
			TriggerFlashBurstNow(vr, now);

		float dtSec = 1.0f / 90.0f;
		bool activeNow = false;
		float intensityNow = 0.0f;
		{
			std::lock_guard<std::mutex> lock(g_FlashMutex);
			if (g_State.hasPrevFrameTime)
			{
				const float rawDt = std::chrono::duration<float>(now - g_State.prevFrameTime).count();
				dtSec = ClampF(rawDt, 0.0005f, 0.05f);
			}
			g_State.prevFrameTime = now;
			g_State.hasPrevFrameTime = true;

			if (g_State.flashActive)
			{
				const float elapsedMs = std::chrono::duration<float, std::milli>(now - g_State.flashStartTime).count();
				intensityNow = EvaluateXenonEnvelopeMs(elapsedMs);
				g_State.currentIntensity = intensityNow;
				if (intensityNow > 0.001f)
				{
					activeNow = true;
					if (g_Game->m_BaseClient)
						ApplyFlashConVarsLocked(g_Game->m_BaseClient, intensityNow);
				}
				else
				{
					g_State.flashActive = false;
					g_State.currentIntensity = 0.0f;
					RestoreFlashConVarsLocked();
				}
			}
			else if (g_State.conVarsModified)
			{
				RestoreFlashConVarsLocked();
			}

			if (g_State.retinalAfterimage > 0.0f)
			{
				float decayed = g_State.retinalAfterimage * std::exp(-dtSec / kAfterimageTauSec) - kAfterimageTailDrainPerSec * dtSec;
				g_State.retinalAfterimage = (decayed > 0.001f) ? decayed : 0.0f;
			}
		}

		if (activeNow)
		{
			Vector flashPos{}, flashNormal{};
			ResolveFlashEmitterPose(vr, flashPos, flashNormal);

			Vector eyeFwd{}, eyeRight{}, eyeUp{};
			QAngle::AngleVectors(eyeAngles, &eyeFwd, &eyeRight, &eyeUp);
			eyeFwd = NormalizeSafe(eyeFwd, Vector(1.0f, 0.0f, 0.0f));

			const Vector leftEye = vr->GetViewOriginLeft(vr->m_SetupOrigin);
			const Vector rightEye = vr->GetViewOriginRight(vr->m_SetupOrigin);

			float maxExposure = ComputeEyeBlindingExposure(vr, eyeOrigin, eyeFwd, flashPos, flashNormal, intensityNow);
			maxExposure = std::max(maxExposure, ComputeEyeBlindingExposure(vr, leftEye, eyeFwd, flashPos, flashNormal, intensityNow));
			maxExposure = std::max(maxExposure, ComputeEyeBlindingExposure(vr, rightEye, eyeFwd, flashPos, flashNormal, intensityNow));

			if (maxExposure > 0.001f)
			{
				std::lock_guard<std::mutex> lock(g_FlashMutex);
				g_State.retinalAfterimage = std::max(g_State.retinalAfterimage, maxExposure);
			}
		}
	}

	bool IsFlashActive()
	{
		VR* vr = g_Game ? g_Game->m_VR : nullptr;
		if (!vr || !vr->m_IsVREnabled)
			return false;

		const auto now = std::chrono::steady_clock::now();
		std::lock_guard<std::mutex> lock(g_FlashMutex);
		if (!g_State.flashActive)
			return false;
		const float elapsedMs = std::chrono::duration<float, std::milli>(now - g_State.flashStartTime).count();
		return EvaluateXenonEnvelopeMs(elapsedMs) > 0.001f;
	}

	float GetScreenWhiteoutOverride()
	{
		VR* vr = g_Game ? g_Game->m_VR : nullptr;
		if (!vr || !vr->m_IsVREnabled)
			return 0.0f;

		std::lock_guard<std::mutex> lock(g_FlashMutex);
		return ClampF(g_State.retinalAfterimage, 0.0f, 1.0f);
	}

	void DrawCameraFlashGlare(const CViewSetup& eyeView, ITexture* eyeTexture)
	{
		VR* vr = g_Game ? g_Game->m_VR : nullptr;
		if (!vr || !vr->m_IsVREnabled || !g_Game || !g_Game->m_MaterialSystem || !eyeTexture)
			return;

		float intensity = 0.0f;
		{
			const auto now = std::chrono::steady_clock::now();
			std::lock_guard<std::mutex> lock(g_FlashMutex);
			if (!g_State.flashActive)
				return;
			const float elapsedMs = std::chrono::duration<float, std::milli>(now - g_State.flashStartTime).count();
			intensity = EvaluateXenonEnvelopeMs(elapsedMs);
		}
		if (intensity <= 0.001f)
			return;

		Vector flashPos{}, flashNormal{};
		ResolveFlashEmitterPose(vr, flashPos, flashNormal);

		const Vector toFlash = flashPos - eyeView.origin;
		const float dist = VectorLength(toFlash);
		if (dist < 0.5f)
			return;

		const Vector dirToFlash = toFlash * (1.0f / dist);
		const float facingEye = Dot3(flashNormal, -dirToFlash);
		if (facingEye <= 0.10f)
			return;

		if (!HasFlashLineOfSight(eyeView.origin, flashPos))
			return;

		Vector vf{}, vr_{}, vu{};
		QAngle::AngleVectors(QAngle(eyeView.angles.x, eyeView.angles.y, eyeView.angles.z), &vf, &vr_, &vu);
		vf = NormalizeSafe(vf, Vector(1.0f, 0.0f, 0.0f));
		const float lookingAt = Dot3(vf, dirToFlash);

		const float z = Dot3(toFlash, vf);
		if (z < 1.0f)
			return;

		const float W = static_cast<float>(vr->m_RenderWidth);
		const float H = static_cast<float>(vr->m_RenderHeight);
		if (W <= 1.0f || H <= 1.0f)
			return;

		const float fovRad = ClampF(eyeView.fov, 10.0f, 170.0f) * kDeg2Rad;
		const float tanX = std::tan(fovRad * 0.5f);
		if (tanX <= 0.001f)
			return;
		const float aspect = W / H;
		const float tanY = tanX / aspect;
		const float sx = (0.5f + 0.5f * Dot3(toFlash, vr_) / (z * tanX)) * W;
		const float sy = (0.5f - 0.5f * Dot3(toFlash, vu) / (z * tanY)) * H;

		IMaterial* glowMat = g_CameraGlowMatSlot.Get(g_Game->m_MaterialSystem, "vr/camera_flash_glow", "Other textures");
		if (!glowMat)
			glowMat = g_FallbackGlowMatSlot.Get(g_Game->m_MaterialSystem, "vr/flashlight_glow", "Other textures");
		if (!glowMat)
			return;

		IMatRenderContext* ctx = g_Game->m_MaterialSystem->GetRenderContext();
		if (!ctx)
			return;

		ctx->SetRenderTarget(eyeTexture);
		ctx->Viewport(0, 0, static_cast<int>(W), static_cast<int>(H));

		typedef void (__thiscall* tDrawScreenSpaceRectangle)(
			void* thisptr, IMaterial* mat, int destX, int destY, int width, int height,
			float srcX0, float srcY0, float srcX1, float srcY1,
			int srcTexW, int srcTexH, void* pClientRenderable, int nXDice, int nYDice);
		auto fnDrawRect = (*reinterpret_cast<tDrawScreenSpaceRectangle**>(ctx))[105];
		const int texW = std::max(glowMat->GetMappingWidth(), 1);
		const int texH = std::max(glowMat->GetMappingHeight(), 1);

		auto drawGlowPass = [&](float pixRadius, int passes)
		{
			if (pixRadius < 2.0f || passes <= 0)
				return;
			const int size = static_cast<int>(pixRadius * 2.0f);
			const int x = static_cast<int>(sx - pixRadius);
			const int y = static_cast<int>(sy - pixRadius);
			if (x + size < 0 || y + size < 0 || x > static_cast<int>(W) || y > static_cast<int>(H))
				return;
			for (int p = 0; p < passes; ++p)
			{
				fnDrawRect(ctx, glowMat, x, y, size, size,
				           0.0f, 0.0f, static_cast<float>(texW - 1), static_cast<float>(texH - 1),
				           texW, texH, nullptr, 1, 1);
			}
		};

		const float cmToUnits = 0.01f * vr->m_VRScale;
		const float worldToPix = (W * 0.5f) / (z * tanX);
		const float intensitySqrt = std::sqrt(intensity);

		if (facingEye <= 0.35f)
		{
			// Sideways glimpse of the xenon tube: crisp compact spark/corona right on the flash window, no full-screen wash
			const float sideFactor = SmoothStep01((facingEye - 0.10f) / (0.35f - 0.10f));
			const float sparkWorldRad = (1.6f + 3.2f * sideFactor) * cmToUnits * (0.25f + 0.75f * intensitySqrt);
			const float sparkPixRad = ClampF(sparkWorldRad * worldToPix, 0.0f, W * 0.12f);
			drawGlowPass(sparkPixRad, 1);
			if (sideFactor > 0.65f && intensity > 0.60f)
				drawGlowPass(sparkPixRad * 0.45f, 1);
		}
		else
		{
			// Pointed toward face: intense xenon bulb hotspot + corona + overexposed blinding flare within ~85 u (~2.0 m)
			const float faceFactor = SmoothStep01((facingEye - 0.35f) / (1.0f - 0.35f));
			const float steepFace = std::pow(faceFactor, 0.7f);
			const float distFactor = (dist < kMaxBlindDistUnits)
				? (1.0f - SmoothStep01((dist - 12.0f) / (kMaxBlindDistUnits - 12.0f)))
				: 0.0f;
			const float lookFactor = (lookingAt > -0.15f)
				? (0.30f + 0.70f * SmoothStep01((lookingAt - 0.02f) / 0.85f))
				: 0.0f;
			const float blindingStrength = ClampF(vr->m_CameraFlashBlindingStrength, 0.0f, 2.0f);

			const float coreWorldRad = (4.0f + 5.5f * faceFactor) * cmToUnits * (0.20f + 0.80f * intensitySqrt);
			const float corePixRad = ClampF(coreWorldRad * worldToPix, 0.0f, W * 0.25f);
			drawGlowPass(corePixRad, (intensity > 0.50f) ? 2 : 1);

			const float coronaWorldRad = (10.0f + 28.0f * faceFactor * faceFactor) * cmToUnits * intensity;
			const float coronaPixRad = ClampF(coronaWorldRad * worldToPix, 0.0f, W * 0.50f);
			if (coronaPixRad >= 4.0f)
				drawGlowPass(coronaPixRad, 1);

			const float flareScale = steepFace * distFactor * lookFactor * intensity * std::min(blindingStrength, 1.5f);
			if (flareScale > 0.02f)
			{
				const float dazzlePixRad = (0.18f + 0.68f * flareScale) * W;
				const int dazzlePasses = std::clamp(1 + static_cast<int>(flareScale * 4.0f), 1, 6);
				drawGlowPass(dazzlePixRad, dazzlePasses);
			}

			// Refresh retinal afterimage if this eye pass got a fresher post-DME camera pose
			if (dist < kMaxBlindDistUnits && lookingAt > -0.15f && blindingStrength > 0.001f)
			{
				const float closeFloor = (dist <= 45.0f)
					? (0.95f * intensity * steepFace * lookFactor * std::min(blindingStrength, 1.0f))
					: 0.0f;
				const float eyeExposure = ClampF(std::max(intensity * steepFace * distFactor * lookFactor * blindingStrength, closeFloor), 0.0f, 1.0f);
				if (eyeExposure > 0.001f)
				{
					std::lock_guard<std::mutex> lock(g_FlashMutex);
					g_State.retinalAfterimage = std::max(g_State.retinalAfterimage, eyeExposure);
				}
			}
		}

		ctx->Release();
	}

	void Reset()
	{
		std::lock_guard<std::mutex> lock(g_FlashMutex);
		RestoreFlashConVarsLocked();

		g_State.hasEvaluatedShouldFlash = false;
		g_State.lastEvaluatedShouldFlash = false;
		g_State.lastEvalCameraThis = nullptr;
		g_State.lastEvalTime = std::chrono::steady_clock::time_point{};

		g_State.pendingAutoBurst = false;
		g_State.pendingAutoCameraThis = nullptr;
		g_State.pendingAutoTime = std::chrono::steady_clock::time_point{};

		g_State.flashActive = false;
		g_State.flashStartTime = std::chrono::steady_clock::time_point{};
		g_State.currentIntensity = 0.0f;

		g_State.retinalAfterimage = 0.0f;
		g_State.hasPrevFrameTime = false;
		g_State.prevFrameTime = std::chrono::steady_clock::time_point{};

		g_State.hasCachedEmitterPose = false;
		g_State.cachedFlashPos = Vector(0.0f, 0.0f, 0.0f);
		g_State.cachedFlashNormal = Vector(1.0f, 0.0f, 0.0f);
	}
}
