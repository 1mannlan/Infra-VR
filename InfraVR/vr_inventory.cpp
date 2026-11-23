#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include "vr_inventory.h"
#include "vr.h"
#include "vr_hands.h"
#include "vr_grab.h"
#include "game.h"
#include "hooks.h"
#include "sdk.h"
#include "trace.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace
{
	constexpr float kPi = 3.14159265358979323846f;
	constexpr float kDeg2Rad = kPi / 180.0f;
	constexpr float kCmToInches = 1.0f / 2.54f;

	// Verified VPK model paths (models/items/batterypack_001.mdl = AA, _002.mdl = D)
	constexpr const char* kModelFlashlight      = "models/weapons/w_light.mdl";
	constexpr const char* kModelSuperFlashlight = "models/weapons/w_superlight.mdl";
	constexpr const char* kModelCamera          = "models/weapons/w_camera.mdl";
	constexpr const char* kModelPhone           = "models/props_office/mobile_phone_001.mdl";
	constexpr const char* kModelDBattery        = "models/items/batterypack_002.mdl";
	constexpr const char* kModelAABattery       = "models/items/batterypack_001.mdl";
	constexpr const char* kModelDBatterySingle  = "models/vr/battery_d_single.mdl";
	constexpr const char* kModelAABatterySingle = "models/vr/battery_aa_single.mdl";

	// Verified P2B * VVD bounding box centers for exact slot centering
	const Vector kCenterAABattery(0.0f, -0.80f, 0.391f);
	const Vector kCenterDBattery(-1.06f, 0.0f, 0.726f);
	const Vector kCenterCamera(1.914f, -3.719f, -0.191f);
	const Vector kCenterPhone(0.0f, 0.0f, 0.199f); // Verified VVD center of mobile_phone_001.mdl
	const Vector kCenterLight(-0.039f, -0.041f, 1.359f);
	const Vector kCenterSuperLight(6.20f, 0.0f, -1.908f);

	// Interaction & physical reload thresholds (Source world units: 1u ~ 2.315 cm at VRScale=43.2)
	constexpr float kBeltGrabRadius       = 4.4f;
	constexpr float kBeltExitRadius       = 5.4f;
	constexpr float kPhoneGrabRadius      = 4.2f;
	constexpr float kPhoneExitRadius      = 5.0f;
	constexpr float kBatteryGrabRadius    = 4.0f;
	constexpr float kBatteryExitRadius    = 4.9f;
	constexpr float kWristGrabRadius      = 4.2f;
	constexpr float kWristExitRadius      = 5.1f;
	constexpr float kHolsterRadius        = 4.8f;
	constexpr float kHolsterExitRadius    = 5.8f;
	constexpr float kReloadSnapDist       = 4.5f; // ~11.5 cm

	// Atlas layout (1024x1024 vr/vr_ui_atlas; logical 512-space cell coordinates scaled by 2)
	constexpr int kAtlasSize = 1024;
	enum AtlasColor : int
	{
		Color_Green   = 0, // #30E860
		Color_Yellow  = 1, // #FFD426
		Color_Red     = 2, // #FF3030
		Color_Cyan    = 3, // #40C8FF
		Color_Amber   = 4, // Unused color slot; row y=448 cell 4 is Icon_Flashlight
		Color_DimGray = 5, // #404850 (row y=384); row y=448 cell 5 is Icon_Camera
		Color_Track   = 6, // Dark unlit dot
		Color_AltSlot = 7, // Soft hover ring (row 3a) / Dim Red (row 3b)

		Icon_Flashlight = 4, // Row y=448, cell 4 (x=256)
		Icon_Camera     = 5  // Row y=448, cell 5 (x=320)
	};

	enum class BatteryType : uint8_t
	{
		None = 0,
		D    = 1, // Flashlight
		AA   = 2  // Camera
	};

	enum class ModelKind : uint8_t
	{
		None = 0,
		Flashlight,
		SuperFlashlight,
		Camera,
		Phone,
		DBattery,
		AABattery,
		DBatterySingle,
		AABatterySingle
	};

	enum SlotId : int
	{
		Slot_None           = -1,
		Slot_BeltFlashlight = 0,
		Slot_BeltDBattery   = 1,
		Slot_BeltAABattery  = 2,
		Slot_BeltCamera     = 3,
		Slot_BeltPhone      = 4,
		Slot_WristDBattery  = 5,
		Slot_WristAABattery = 6,
		Slot_Count          = 7
	};

	// -------------------------------------------------------------------------
	// Small math & trace helpers
	// -------------------------------------------------------------------------
	inline float Dot3(const Vector& a, const Vector& b)
	{
		return a.x * b.x + a.y * b.y + a.z * b.z;
	}

	inline float LenSq3(const Vector& v)
	{
		return v.x * v.x + v.y * v.y + v.z * v.z;
	}

	inline float Len3(const Vector& v)
	{
		return std::sqrt(LenSq3(v));
	}

	inline Vector Cross3(const Vector& a, const Vector& b)
	{
		return Vector(
			a.y * b.z - a.z * b.y,
			a.z * b.x - a.x * b.z,
			a.x * b.y - a.y * b.x);
	}

	inline Vector NormalizeSafe(const Vector& v, const Vector& fallback)
	{
		const float lsq = LenSq3(v);
		if (lsq > 1e-8f)
		{
			const float inv = 1.0f / std::sqrt(lsq);
			return Vector(v.x * inv, v.y * inv, v.z * inv);
		}
		return fallback;
	}

	inline float WrapAngle180(float deg)
	{
		while (deg > 180.0f)  deg -= 360.0f;
		while (deg <= -180.0f) deg += 360.0f;
		return deg;
	}

	inline float DistPointToSegment(const Vector& p, const Vector& a, const Vector& b)
	{
		const Vector ab = b - a;
		const float abLenSq = LenSq3(ab);
		if (abLenSq <= 1e-6f)
			return Len3(p - a);
		const float t = std::clamp(Dot3(p - a, ab) / abLenSq, 0.0f, 1.0f);
		return Len3(p - (a + ab * t));
	}

	class CTraceFilterWorldOnlyInv : public CTraceFilter
	{
	public:
		CTraceFilterWorldOnlyInv() : CTraceFilter(nullptr, 0) {}
		virtual bool ShouldHitEntity(IHandleEntity*, int) override { return false; }
		virtual TraceType GetTraceType() const override { return TraceType::TRACE_WORLD_ONLY; }
	};

	bool IsLineOfSightClear(IEngineTrace* trace, const Vector& from, const Vector& to)
	{
		if (!trace)
			return true;
		const Vector delta = to - from;
		const float dist = Len3(delta);
		if (dist <= 0.5f)
			return true;

		CTraceFilterWorldOnlyInv filter;
		CGameTrace tr;
		Ray_t ray;
		ray.Init(from, to);
		trace->TraceRay(ray, CONTENTS_SOLID, &filter, &tr);
		if (!tr.startsolid && (1.0f - tr.fraction) * dist > 1.0f)
			return false;
		return true;
	}

	bool ProjectWorldToEyeScreen(
		const CViewSetup& eyeView,
		float viewW,
		float viewH,
		const Vector& worldPos,
		float& outSx,
		float& outSy,
		float* outZ = nullptr)
	{
		Vector vf, vr, vu;
		QAngle::AngleVectors(QAngle(eyeView.angles.x, eyeView.angles.y, eyeView.angles.z), &vf, &vr, &vu);
		const Vector v = worldPos - eyeView.origin;
		const float z = Dot3(v, vf);
		if (z < 1.0f)
			return false;

		const float tanX = std::tan(eyeView.fov * 0.5f * kDeg2Rad);
		if (tanX <= 1e-4f)
			return false;
		const float aspect = (viewH > 1.0f) ? (viewW / viewH) : 1.0f;
		const float tanY = tanX / aspect;

		outSx = (0.5f + 0.5f * Dot3(v, vr) / (z * tanX)) * viewW;
		outSy = (0.5f - 0.5f * Dot3(v, vu) / (z * tanY)) * viewH;
		if (outZ)
			*outZ = z;
		return true;
	}

	void PlayInventorySound(Game* game, const char* wavRelPath)
	{
		if (!game || !game->m_EngineClient || !wavRelPath || !wavRelPath[0])
			return;
		if (!game->m_EngineClient->IsInGame() ||
		    game->m_EngineClient->IsLevelMainMenuBackground() ||
		    game->m_EngineClient->IsPaused())
		{
			return;
		}
		char cmd[160]{};
		std::snprintf(cmd, sizeof(cmd), "play %s", wavRelPath);
		game->m_EngineClient->ClientCmd_Unrestricted(cmd);
	}

	// -------------------------------------------------------------------------
	// Material & DME rendering types
	// -------------------------------------------------------------------------
	struct PinnedMaterialSlot
	{
		IMaterial* mat = nullptr;
		unsigned long long lastRetryMs = 0ULL;

		IMaterial* Get(IMaterialSystem* matSys, const char* name, const char* group)
		{
			if (mat)
				return mat;
			if (!matSys)
				return nullptr;
			const unsigned long long nowMs = GetTickCount64();
			if (lastRetryMs != 0ULL && (nowMs - lastRetryMs) < 1000ULL)
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

	struct LocalDrawModelState
	{
		void* m_pStudioHdr = nullptr;
		void* m_pStudioHWData = nullptr;
		void* m_pRenderable = nullptr;
		const matrix3x4_t* m_pModelToWorld = nullptr;
		uint32_t m_decals = 0;
		int m_drawFlags = 0;
		int m_lod = 0;
	};
	static_assert(sizeof(LocalDrawModelState) == 28);

	// engine.dll CModelRender reads +0x40 (float alpha/scale = 1.0f) and +0x44 (uint16_t instance = 0xFFFF)
	struct alignas(4) EngineModelRenderInfo
	{
		ModelRenderInfo_t info{};
		uint32_t instanceExt = 0xFFFFu;
	};
	static_assert(sizeof(EngineModelRenderInfo) == 0x48);

	class StubClientRenderable;

	class StubClientUnknown
	{
	public:
		uint32_t m_refEHandle = 0xFFFFFFFFu;
		StubClientRenderable* m_pRenderable = nullptr;

		virtual ~StubClientUnknown() = default;                                       // 0 (+0x00) ~IHandleEntity
		virtual void SetRefEHandle(const uint32_t& handle) { m_refEHandle = handle; } // 1 (+0x04)
		virtual const uint32_t& GetRefEHandle() const { return m_refEHandle; }        // 2 (+0x08)
		virtual void* GetCollideable() { return nullptr; }                            // 3 (+0x0C)
		virtual void* GetClientNetworkable() { return nullptr; }                      // 4 (+0x10)
		virtual void* GetClientRenderable() { return m_pRenderable; }                 // 5 (+0x14)
		virtual void* GetIClientEntity() { return nullptr; }                          // 6 (+0x18)
		virtual void* GetBaseEntity() { return nullptr; }                             // 7 (+0x1C)
		virtual void* GetClientThinkable() { return nullptr; }                        // 8 (+0x20)
		virtual void* GetClientAlphaProperty() { return nullptr; }                    // 9 (+0x24)
	};

	class StubClientRenderable
	{
	public:
		Vector m_origin{ 0.0f, 0.0f, 0.0f };
		QAngle m_angles{ 0.0f, 0.0f, 0.0f };
		model_t* m_pModel = nullptr;
		matrix3x4_t m_transform{};
		uint16_t m_renderHandle = 0xFFFFu;
		Vector m_mins{ -16.0f, -16.0f, -16.0f };
		Vector m_maxs{  16.0f,  16.0f,  16.0f };
		StubClientUnknown m_unknown{};

		virtual void* GetIClientUnknown() { m_unknown.m_pRenderable = this; return &m_unknown; }          // 0
		virtual const Vector& GetRenderOrigin() { return m_origin; }                                      // 1
		virtual const QAngle& GetRenderAngles() { return m_angles; }                                      // 2
		virtual bool ShouldDraw() { return true; }                                                        // 3
		virtual int GetRenderFlags() { return 0; }                                                        // 4
		virtual void Unused() {}                                                                          // 5
		virtual uint16_t GetShadowHandle() const { return 0xFFFFu; }                                      // 6
		virtual uint16_t& RenderHandle() { return m_renderHandle; }                                       // 7
		virtual const model_t* GetModel() const { return m_pModel; }                                      // 8
		virtual int DrawModel(int, const void*) { return 1; }                                             // 9
		virtual int GetBody() { return 0; }                                                               // 10
		virtual void GetColorModulation(float* c) { if (c) { c[0] = 1.0f; c[1] = 1.0f; c[2] = 1.0f; } }   // 11
		virtual bool LODTest() { return true; }                                                           // 12
		virtual bool SetupBones(matrix3x4_t* pBoneToWorldOut, int nMaxBones, int, float)                  // 13
		{
			if (pBoneToWorldOut && nMaxBones > 0)
			{
				for (int i = 0; i < nMaxBones; ++i)
					pBoneToWorldOut[i] = m_transform;
			}
			return true;
		}
		virtual void SetupWeights(const matrix3x4_t*, int, float*, float*) {}                             // 14
		virtual void DoAnimationEvents() {}                                                               // 15
		virtual void* GetPVSNotifyInterface() { return nullptr; }                                         // 16
		virtual void GetRenderBounds(Vector& mins, Vector& maxs) { mins = m_mins; maxs = m_maxs; }        // 17
		virtual void GetRenderBoundsWorldspace(Vector& mins, Vector& maxs) { mins = m_mins; maxs = m_maxs; } // 18
		virtual void GetShadowRenderBounds(Vector& mins, Vector& maxs, int) { mins = m_mins; maxs = m_maxs; } // 19
		virtual bool ShouldReceiveProjectedTextures(int) { return true; }                                 // 20
		virtual bool GetShadowCastDistance(float*, int) const { return false; }                           // 21
		virtual bool GetShadowCastDirection(Vector*, int) const { return false; }                         // 22
		virtual bool IsShadowDirty() { return false; }                                                    // 23
		virtual void MarkShadowDirty(bool) {}                                                             // 24
		virtual StubClientRenderable* GetShadowParent() { return nullptr; }                               // 25
		virtual StubClientRenderable* FirstShadowChild() { return nullptr; }                              // 26
		virtual StubClientRenderable* NextShadowPeer() { return nullptr; }                                // 27
		virtual int ShadowCastType() { return 0; }                                                        // 28
		virtual void CreateModelInstance() {}                                                             // 29
		virtual uint16_t GetModelInstance() { return 0xFFFFu; }                                           // 30
		virtual const matrix3x4_t& RenderableToWorldTransform() { return m_transform; }                   // 31
		virtual int LookupAttachment(const char*) { return -1; }                                          // 32
		virtual bool GetAttachment(int, Vector& absOrigin, QAngle& absAngles)                             // 33
		{
			absOrigin = m_origin;
			absAngles = m_angles;
			return false;
		}
		virtual bool GetAttachment(int, matrix3x4_t& matrix) { matrix = m_transform; return false; }      // 34
		virtual float* GetRenderClipPlane() { return nullptr; }                                           // 35
		virtual int GetSkin() { return 0; }                                                               // 36
		virtual void OnThreadedDrawSetup() {}                                                             // 37
		virtual bool UsesFlexDelayedWeights() { return false; }                                           // 38
		virtual void RecordToolMessage() {}                                                               // 39
		virtual bool ShouldDrawForSplitScreenUser(int) { return true; }                                   // 40
		virtual uint8_t OverrideAlphaModulation(uint8_t nAlpha) { return nAlpha; }                        // 41
		virtual uint8_t OverrideShadowModulation(uint8_t nAlpha) { return nAlpha; }                       // 42
	};

	struct ResolvedWorldModel
	{
		model_t* pModel = nullptr;
		void* pStudioHdr = nullptr;
		void* pStudioHWData = nullptr;
		int numBones = 0;
		bool valid = false;
	};

	ResolvedWorldModel ResolveWorldModel(Game* game, const char* modelPath)
	{
		ResolvedWorldModel out{};
		if (!game || !game->m_ModelInfo || !game->m_BaseClient || !modelPath || !modelPath[0])
			return out;

		void** miVtbl = *reinterpret_cast<void***>(game->m_ModelInfo);
		if (!miVtbl)
			return out;

		model_t* pModel = nullptr;
		const int idx = game->m_ModelInfo->GetModelIndex(modelPath);
		if (idx > 0)
		{
			pModel = static_cast<model_t*>(game->m_ModelInfo->GetModel(idx));
		}
		if (!pModel && miVtbl[41])
		{
			typedef model_t* (__thiscall* tFindOrLoadModel)(void* thisptr, const char* name);
			auto fnFindOrLoadModel = reinterpret_cast<tFindOrLoadModel>(miVtbl[41]);
			pModel = fnFindOrLoadModel(game->m_ModelInfo, modelPath);
		}
		if (!pModel)
			return out;

		typedef void* (__thiscall* tGetStudiomodel)(void* thisptr, const model_t* model);
		typedef uint16_t (__thiscall* tGetCacheHandle)(void* thisptr, const model_t* model);
		auto fnGetStudiomodel = reinterpret_cast<tGetStudiomodel>(miVtbl[30]);
		auto fnGetCacheHandle = reinterpret_cast<tGetCacheHandle>(miVtbl[42]);
		if (!fnGetStudiomodel || !fnGetCacheHandle)
			return out;

		void* pStudioHdr = fnGetStudiomodel(game->m_ModelInfo, pModel);
		const uint16_t cacheHandle = fnGetCacheHandle(game->m_ModelInfo, pModel);
		if (!pStudioHdr || cacheHandle == 0xFFFFu)
			return out;

		const int numBones = *reinterpret_cast<const int*>(reinterpret_cast<uintptr_t>(pStudioHdr) + 156);
		if (numBones < 1 || numBones > 128)
			return out;

		static void* s_MdlCache = nullptr;
		if (!s_MdlCache)
			s_MdlCache = game->GetInterface("datacache.dll", "MDLCache004");
		void* mdlCache = s_MdlCache;
		if (!mdlCache)
		{
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(game->m_BaseClient);
			if (dos->e_magic == IMAGE_DOS_SIGNATURE && dos->e_lfanew > 0 && dos->e_lfanew <= 0x1000)
			{
				const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(game->m_BaseClient + static_cast<uintptr_t>(dos->e_lfanew));
				if (nt->Signature == IMAGE_NT_SIGNATURE && (0x70DF6Cu + sizeof(void*)) <= nt->OptionalHeader.SizeOfImage)
				{
					const void* ptrAddr = reinterpret_cast<const void*>(game->m_BaseClient + 0x70DF6Cu);
					MEMORY_BASIC_INFORMATION mbi{};
					if (VirtualQuery(ptrAddr, &mbi, sizeof(mbi)) == sizeof(mbi) &&
					    mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0)
					{
						mdlCache = *reinterpret_cast<void* const*>(ptrAddr);
					}
				}
			}
		}
		if (!mdlCache)
			return out;

		void** mcVtbl = *reinterpret_cast<void***>(mdlCache);
		if (!mcVtbl)
			return out;

		typedef void* (__thiscall* tGetHardwareData)(void* thisptr, uint16_t handle);
		auto fnGetHardwareData = reinterpret_cast<tGetHardwareData>(mcVtbl[14]);
		if (!fnGetHardwareData)
			return out;

		void* pHWData = fnGetHardwareData(mdlCache, cacheHandle);
		if (!pHWData)
			return out;

		out.pModel = pModel;
		out.pStudioHdr = pStudioHdr;
		out.pStudioHWData = pHWData;
		out.numBones = numBones;
		out.valid = true;
		return out;
	}

	struct RenderItem
	{
		ModelKind model = ModelKind::None;
		Vector worldCenter{ 0.0f, 0.0f, 0.0f };
		QAngle worldAngles{ 0.0f, 0.0f, 0.0f };
		matrix3x4_t boneToWorld{};
		matrix3x4_t lightingM2W{};
	};

	void BuildCenteredRenderItem(
		ModelKind modelKind,
		const Vector& worldCenter,
		const Vector& axisX,
		const Vector& axisY,
		const Vector& axisZ,
		float scale,
		const Vector& localCenter,
		RenderItem& outItem)
	{
		const Vector origin = worldCenter
			- axisX * (scale * localCenter.x)
			- axisY * (scale * localCenter.y)
			- axisZ * (scale * localCenter.z);

		outItem.model = modelKind;
		outItem.worldCenter = worldCenter;
		QAngle::VectorAngles(axisX, axisZ, outItem.worldAngles);
		outItem.boneToWorld = matrix3x4_t(axisX * scale, axisY * scale, axisZ * scale, origin);
		outItem.lightingM2W = matrix3x4_t(axisX, axisY, axisZ, worldCenter);
	}

	// -------------------------------------------------------------------------
	// Shared Inventory State
	// -------------------------------------------------------------------------
	std::mutex s_SnapMutex;
	VRInventory::ServerSnapshot s_ServerSnap{};

	std::mutex s_ActionMutex;
	VRInventory::PendingActions s_PendingActions{};

	std::atomic<bool> s_IsHoldingBattery[2]{ { false }, { false } };
	std::atomic<bool> s_IsInInventoryZone[2]{ { false }, { false } };

	struct HandInventoryState
	{
		BatteryType holdingBattery = BatteryType::None;
		int hoverSlot = Slot_None;
		bool prevGripDown = false;
		bool prevDigitalGrip = false;
		bool curlArmed = false;
		bool curlLatched = false;
		bool holsterGripArmed = false;
		bool prevHoldingProp = false;
		bool blockGrabUntilRelease = false;
		bool requireHolsterZoneExit = false;
		std::chrono::steady_clock::time_point lastEquipTime{};
		bool hasHolsterHoverStart = false;
		std::chrono::steady_clock::time_point holsterHoverStart{};
		std::chrono::steady_clock::time_point holsterRegrabCooldownUntil{};
	};

	struct BadgeItem
	{
		bool active = false;
		Vector worldPos{ 0.0f, 0.0f, 0.0f };
		int count = 0;
		int iconCellIdx = Icon_Flashlight;
	};

	struct HolsterMarker
	{
		bool active = false;
		Vector worldPos{ 0.0f, 0.0f, 0.0f };
		int colorIdx = Color_AltSlot;
		bool occupied = false;
		bool hovered = false;
	};

	// Protected by main-thread render sequence (UpdateFrame -> DrawWorldModelsInDME -> DrawEyeOverlays)
	std::mutex s_FrameMutex;
	Vector s_LastBeltRight(0.0f, -1.0f, 0.0f);
	bool s_HasRefHeadZ = false;
	float s_RefHeadRelZ = 0.0f;
	std::chrono::steady_clock::time_point s_LastFrameTime{};
	bool s_HasLastFrameTime = false;
	HandInventoryState s_Hand[2]{};
	std::chrono::steady_clock::time_point s_PhoneHolsterCooldownUntil{};

	constexpr int kMaxRenderItems = 10;
	RenderItem s_RenderItems[kMaxRenderItems]{};
	int s_RenderItemCount = 0;

	constexpr int kMaxBadges = 4;
	BadgeItem s_Badges[kMaxBadges]{};
	int s_BadgeCount = 0;

	constexpr int kMaxMarkers = 8;
	HolsterMarker s_Markers[kMaxMarkers]{};
	int s_MarkerCount = 0;

	bool s_BeltUiVisible = false;

	int s_ActiveEye = 0;
	bool s_RenderedThisEye = false;
	bool s_SawViewModelThisEye = false;
	bool s_InInventoryDME = false;

	// Evaluates digital grip + Knuckles finger curl directly for inventory slots without VRGrab interference.
	bool EvaluateInventoryHandGrip(VR* vr, bool physicalLeft, HandInventoryState& st, bool hasInventoryTarget, bool isHolsterTarget, bool& outDigitalGrip, bool& outAutoLatchedOnHolsterEntry)
	{
		outAutoLatchedOnHolsterEntry = false;
		outDigitalGrip = physicalLeft
			? vr->PressedDigitalAction(vr->m_ActionCrouch)
			: vr->PressedDigitalAction(vr->m_ActionReload);

		const bool holdingBatt = (st.holdingBattery != BatteryType::None);
		const int skelState = (physicalLeft ? vr->m_SkeletalTrackingStateLeft : vr->m_SkeletalTrackingStateRight)
			.load(std::memory_order_acquire);
		if (skelState != 0)
		{
			const float* curls = physicalLeft ? vr->m_LeftFingerCurl : vr->m_RightFingerCurl;
			const float meanMRP = (curls[2] + curls[3] + curls[4]) * (1.0f / 3.0f);
			const int count70 =
				(curls[2] >= 0.70f ? 1 : 0) +
				(curls[3] >= 0.70f ? 1 : 0) +
				(curls[4] >= 0.70f ? 1 : 0);

			if (st.curlLatched)
			{
				const float releaseThresh = isHolsterTarget ? 0.46f : 0.52f;
				if (meanMRP <= releaseThresh || (!holdingBatt && !hasInventoryTarget))
				{
					st.curlLatched = false;
					st.curlArmed = (meanMRP <= 0.56f);
				}
			}
			else
			{
				// Entering a holster slot while already gripping Camera / Flashlight arms release-to-holster immediately.
				if (isHolsterTarget && !holdingBatt && meanMRP >= 0.62f)
				{
					st.curlArmed = false;
					st.curlLatched = true;
					outAutoLatchedOnHolsterEntry = true;
				}
				else if (meanMRP <= 0.56f)
				{
					st.curlArmed = true;
				}
				else if (st.curlArmed && meanMRP >= 0.74f && count70 >= 2)
				{
					st.curlArmed = false;
					if (hasInventoryTarget)
						st.curlLatched = true;
				}
			}
		}
		else
		{
			st.curlArmed = false;
			st.curlLatched = false;
		}

		return outDigitalGrip || st.curlLatched;
	}

	const char* ModelKindToPath(ModelKind kind)
	{
		switch (kind)
		{
		case ModelKind::Flashlight:      return kModelFlashlight;
		case ModelKind::SuperFlashlight: return kModelSuperFlashlight;
		case ModelKind::Camera:          return kModelCamera;
		case ModelKind::Phone:           return kModelPhone;
		case ModelKind::DBattery:        return kModelDBattery;
		case ModelKind::AABattery:       return kModelAABattery;
		case ModelKind::DBatterySingle:  return kModelDBatterySingle;
		case ModelKind::AABatterySingle: return kModelAABatterySingle;
		default:                         return nullptr;
		}
	}

	// Draws a single 2D atlas cell at screen coordinates (dstX, dstY, dstW, dstH)
	template <typename DrawRectFn>
	void DrawAtlasCell(
		DrawRectFn fnDrawRect,
		IMatRenderContext* ctx,
		IMaterial* atlasMat,
		int viewW,
		int viewH,
		int dstX,
		int dstY,
		int dstW,
		int dstH,
		int srcX,
		int srcY,
		int srcCellSize)
	{
		if (dstW <= 0 || dstH <= 0)
			return;
		if (dstX + dstW < 0 || dstY + dstH < 0 || dstX >= viewW || dstY >= viewH)
			return;

		constexpr int kAtlasScale = 2;
		fnDrawRect(ctx, atlasMat, dstX, dstY, dstW, dstH,
		           static_cast<float>(srcX * kAtlasScale + 2),
		           static_cast<float>(srcY * kAtlasScale + 2),
		           static_cast<float>((srcX + srcCellSize) * kAtlasScale - 3),
		           static_cast<float>((srcY + srcCellSize) * kAtlasScale - 3),
		           kAtlasSize, kAtlasSize, nullptr, 1, 1);
	}

	void GetDigitCell(char ch, int& outX, int& outY)
	{
		if (ch >= '0' && ch <= '3')
		{
			outX = (ch - '0') * 128;
			outY = 0;
		}
		else if (ch >= '4' && ch <= '7')
		{
			outX = (ch - '4') * 128;
			outY = 128;
		}
		else if (ch == '8' || ch == '9')
		{
			outX = (ch - '8') * 128;
			outY = 256;
		}
		else // 'x'
		{
			outX = 256;
			outY = 256;
		}
	}
}

namespace VRInventory
{
	void PublishServerSnapshot(const ServerSnapshot& snap)
	{
		std::lock_guard<std::mutex> lock(s_SnapMutex);
		s_ServerSnap = snap;
	}

	ServerSnapshot GetSnapshot()
	{
		std::lock_guard<std::mutex> lock(s_SnapMutex);
		return s_ServerSnap;
	}

	PendingActions ConsumePendingActions()
	{
		std::lock_guard<std::mutex> lock(s_ActionMutex);
		PendingActions out = s_PendingActions;
		s_PendingActions = PendingActions{};
		return out;
	}

	void Reset()
	{
		{
			std::lock_guard<std::mutex> lock(s_SnapMutex);
			s_ServerSnap = ServerSnapshot{};
		}
		{
			std::lock_guard<std::mutex> lock(s_ActionMutex);
			s_PendingActions = PendingActions{};
		}
		{
			std::lock_guard<std::mutex> lock(s_FrameMutex);
			s_LastBeltRight = Vector(0.0f, -1.0f, 0.0f);
			s_HasRefHeadZ = false;
			s_RefHeadRelZ = 0.0f;
			s_HasLastFrameTime = false;
			s_Hand[0] = HandInventoryState{};
			s_Hand[1] = HandInventoryState{};
			s_PhoneHolsterCooldownUntil = std::chrono::steady_clock::time_point{};
			s_RenderItemCount = 0;
			s_BadgeCount = 0;
			s_MarkerCount = 0;
			s_BeltUiVisible = false;
			s_ActiveEye = 0;
			s_RenderedThisEye = false;
			s_SawViewModelThisEye = false;
			s_InInventoryDME = false;
		}
		s_IsHoldingBattery[0].store(false, std::memory_order_relaxed);
		s_IsHoldingBattery[1].store(false, std::memory_order_relaxed);
		s_IsInInventoryZone[0].store(false, std::memory_order_relaxed);
		s_IsInInventoryZone[1].store(false, std::memory_order_relaxed);
	}

	bool IsHandHoldingBattery(bool physicalLeftHand)
	{
		return s_IsHoldingBattery[physicalLeftHand ? 1 : 0].load(std::memory_order_relaxed);
	}

	bool IsHandInInventoryZone(bool physicalLeftHand)
	{
		return s_IsInInventoryZone[physicalLeftHand ? 1 : 0].load(std::memory_order_relaxed);
	}

	void UpdateFrame(const Vector& eyeOrigin, const QAngle& eyeAngles)
	{
		Game* game = g_Game;
		VR* vr = game ? game->m_VR : nullptr;

		if (!vr || !vr->m_IsVREnabled || vr->m_In3DViewControlMenu || !game->m_EngineClient ||
		    !game->m_EngineClient->IsInGame() || game->m_EngineClient->IsLevelMainMenuBackground())
		{
			Reset();
			return;
		}

		const bool isPaused = game->m_EngineClient->IsPaused();
		const bool phonePauseHolsterOnly = isPaused && vr->m_PhoneEquipped && !vr->Is2DSubMenuVisible();
		if ((isPaused && !phonePauseHolsterOnly) || vr->m_IsCalibrating)
		{
			std::lock_guard<std::mutex> lock(s_FrameMutex);
			s_HasLastFrameTime = false;
			s_RenderItemCount = 0;
			s_BadgeCount = 0;
			s_MarkerCount = 0;
			s_BeltUiVisible = false;
			return;
		}

		const VR::BatteryInventoryMode mode = vr->m_BatteryInventoryMode;
		const bool beltEnabled  = (mode == VR::BatteryInventoryMode::Belt || mode == VR::BatteryInventoryMode::Both);
		const bool wristEnabled = (mode == VR::BatteryInventoryMode::Wrist || mode == VR::BatteryInventoryMode::Both);

		if (!beltEnabled && !wristEnabled)
		{
			std::lock_guard<std::mutex> lock(s_FrameMutex);
			s_Hand[0] = HandInventoryState{};
			s_Hand[1] = HandInventoryState{};
			s_PhoneHolsterCooldownUntil = std::chrono::steady_clock::time_point{};
			s_RenderItemCount = 0;
			s_BadgeCount = 0;
			s_MarkerCount = 0;
			s_BeltUiVisible = false;
			s_IsHoldingBattery[0].store(false, std::memory_order_relaxed);
			s_IsHoldingBattery[1].store(false, std::memory_order_relaxed);
			s_IsInInventoryZone[0].store(false, std::memory_order_relaxed);
			s_IsInInventoryZone[1].store(false, std::memory_order_relaxed);
			return;
		}

		const ServerSnapshot snap = GetSnapshot();
		std::lock_guard<std::mutex> lock(s_FrameMutex);

		const auto nowSteady = std::chrono::steady_clock::now();
		float dt = 1.0f / 90.0f;
		if (s_HasLastFrameTime)
		{
			dt = std::clamp(std::chrono::duration<float>(nowSteady - s_LastFrameTime).count(), 0.0005f, 0.05f);
		}
		s_LastFrameTime = nowSteady;
		s_HasLastFrameTime = true;

		// 1. Live HMD-Locked Torso Anchor (1:1 yaw with HMD; neck-pivot & pitch-locked height prevent belt drift/sink when looking down)
		const float cmUnits = 0.01f * vr->m_VRScale;
		Vector hmdCenter = vr->m_6DOF ? (vr->m_SetupOrigin + vr->m_HmdPosRelative) : vr->m_SetupOrigin;
		if (LenSq3(hmdCenter) < 1e-4f)
			hmdCenter = eyeOrigin;

		const float hmdPitch = WrapAngle180(eyeAngles.x);
		Vector hmdFwd{}, hmdRight{}, hmdUpVec{};
		QAngle::AngleVectors(eyeAngles, &hmdFwd, &hmdRight, &hmdUpVec);
		const Vector horizRight(hmdRight.x, hmdRight.y, 0.0f);
		if (LenSq3(horizRight) > 1e-4f)
		{
			s_LastBeltRight = NormalizeSafe(horizRight, s_LastBeltRight);
		}

		const Vector torsoCol = hmdCenter - (hmdFwd * 7.5f + hmdUpVec * 15.0f) * cmUnits;
		const float headRelZNow = (torsoCol.z + 15.0f * cmUnits) - vr->m_SetupOrigin.z;
		if (!s_HasRefHeadZ || std::fabs(hmdPitch) < 15.0f)
		{
			s_RefHeadRelZ = headRelZNow;
			s_HasRefHeadZ = true;
		}
		const float physKneelDropUnits = std::min(0.0f, headRelZNow - s_RefHeadRelZ + 22.0f * cmUnits);
		const float beltDropUnits = std::clamp(vr->GetEffectiveBeltHeightOffsetCm(), 10.0f, 130.0f) * cmUnits;
		const Vector beltUp(0.0f, 0.0f, 1.0f);
		const Vector beltRight = s_LastBeltRight;
		const Vector beltFwd = NormalizeSafe(Cross3(beltUp, beltRight), Vector(1.0f, 0.0f, 0.0f));
		const Vector beltOrigin(torsoCol.x, torsoCol.y, vr->m_SetupOrigin.z + s_RefHeadRelZ + physKneelDropUnits - beltDropUnits);

		const float mirrorSign = vr->m_LeftHanded ? -1.0f : 1.0f;
		const bool flPhysLeft  = !vr->m_LeftHanded; // Flashlight is always on vm1 (Left hand when !LeftHanded)
		const bool camPhysLeft = vr->m_LeftHanded;  // Camera is always on vm0 (Right hand when !LeftHanded)

		// Belt slot world positions (front hips arc centered over legs, tilted ~18 deg forward, mirrored when m_LeftHanded)
		constexpr float kBeltTiltRad = 18.0f * kDeg2Rad;
		const float cTilt = std::cos(kBeltTiltRad);
		const float sTilt = std::sin(kBeltTiltRad);
		const Vector beltFwdTilted = beltFwd * cTilt - beltUp * sTilt;
		const Vector beltUpTilted  = beltUp  * cTilt + beltFwd * sTilt;
		const Vector beltArcPivot  = beltOrigin + beltFwd * 6.3f;
		const Vector posBeltFlashlight = beltArcPivot - beltRight * (6.8f * mirrorSign) - beltFwdTilted * 0.9f - beltUpTilted * 0.2f;
		const Vector posBeltDBattery   = beltArcPivot - beltRight * (2.5f * mirrorSign) + beltFwdTilted * 0.9f;
		const Vector posBeltAABattery  = beltArcPivot + beltRight * (2.5f * mirrorSign) + beltFwdTilted * 0.9f;
		const Vector posBeltCamera     = beltArcPivot + beltRight * (8.4f * mirrorSign) - beltFwdTilted * 0.9f - beltUpTilted * 0.2f;
		// Side trouser pocket slot: ~10 cm lower and ~9.5 cm further back than the belt camera
		const Vector posBeltPhone      = posBeltCamera
			- beltUp * (10.0f * cmUnits)
			- beltFwd * (9.5f * cmUnits)
			- beltRight * (1.5f * cmUnits * mirrorSign);

		// Straight vertically down (-beltUp) after pitching belt ~18 deg forward
		const Vector beltFlashlightLensDir(0.0f, 0.0f, -1.0f);

		// Wrist-hole poses & slot world positions
		VRHandGripPose flWristHp{};
		VRHandGripPose camWristHp{};
		const bool haveFlWristPose  = VRHands::ComputeHandGripPose(vr, flPhysLeft,  vr->m_SetupOrigin, flWristHp,  false) && flWristHp.valid;
		const bool haveCamWristPose = VRHands::ComputeHandGripPose(vr, camPhysLeft, vr->m_SetupOrigin, camWristHp, false) && camWristHp.valid;

		const Vector posWristDBattery = haveFlWristPose
			? (flWristHp.wristWorldPos - flWristHp.fingerDistalDir * 1.45f - flWristHp.palmOutwardNormal * 0.85f + flWristHp.thumbRadialDir * 0.55f)
			: Vector(0.0f, 0.0f, 0.0f);
		const Vector posWristAABattery = haveCamWristPose
			? (camWristHp.wristWorldPos - camWristHp.fingerDistalDir * 1.45f - camWristHp.palmOutwardNormal * 0.85f + camWristHp.thumbRadialDir * 0.55f)
			: Vector(0.0f, 0.0f, 0.0f);

		// Slot availability
		const float sincePauseOpenSec = std::chrono::duration<float>(nowSteady - vr->m_PauseOpenedAt).count();
		const bool phoneOpeningPending = vr->m_PauseOpenRequested && sincePauseOpenSec >= 0.0f && sincePauseOpenSec < 2.0f;
		const bool canOpenPhoneMenu = !Hooks::hkShouldUsePhonePause.fOriginal || Hooks::hkShouldUsePhonePause.fOriginal();
		const bool phoneHolsterCooldown = (nowSteady < s_PhoneHolsterCooldownUntil);

		const bool showBeltFlashlight = !phonePauseHolsterOnly && beltEnabled && snap.valid && snap.hasFlashlight && !vr->m_FlashlightEquipped && vr->m_BeltHolsterEnabled;
		const bool showBeltCamera     = !phonePauseHolsterOnly && beltEnabled && snap.valid && snap.hasCamera && (vr->m_RightHandEmpty || vr->m_PhoneEquipped) && vr->m_BeltHolsterEnabled;
		const bool showBeltPhone      = !phonePauseHolsterOnly && beltEnabled && snap.valid && vr->m_BeltHolsterEnabled && canOpenPhoneMenu && !vr->m_PhoneEquipped && !phoneOpeningPending;
		const bool showBeltDBattery   = !phonePauseHolsterOnly && beltEnabled && snap.valid && (snap.dBatteryReserve > 0);
		const bool showBeltAABattery  = !phonePauseHolsterOnly && beltEnabled && snap.valid && (snap.aaBatteryReserve > 0);

		const bool showWristDBattery  = !phonePauseHolsterOnly && wristEnabled && snap.valid && haveFlWristPose  && (snap.dBatteryReserve > 0);
		const bool showWristAABattery = !phonePauseHolsterOnly && wristEnabled && snap.valid && haveCamWristPose && (snap.aaBatteryReserve > 0);

		// Empty belt holster targets for returning held Flashlight / Camera / Phone
		const bool canHolsterFlashlight = !phonePauseHolsterOnly && beltEnabled && snap.valid && vr->m_BeltHolsterEnabled && vr->m_FlashlightEquipped;
		const bool canHolsterCamera     = !phonePauseHolsterOnly && beltEnabled && snap.valid && vr->m_BeltHolsterEnabled && !vr->m_RightHandEmpty && !vr->m_PhoneEquipped;
		const bool canHolsterPhone      = beltEnabled && vr->m_BeltHolsterEnabled && vr->m_PhoneEquipped && (isPaused || vr->m_PauseOpenRequested) && !vr->Is2DSubMenuVisible();

		// Compute hand selection centers & grip poses upfront
		Vector handGrabCenter[2]{};
		bool haveHandPose[2]{ false, false };
		VRHandGripPose handGripPose[2]{};
		for (int h = 0; h < 2; ++h)
		{
			const bool physLeft = (h == 1);
			haveHandPose[h] = VRHands::ComputeHandGripPose(vr, physLeft, vr->m_SetupOrigin, handGripPose[h], false) && handGripPose[h].valid;
			Vector palmPos, pinchPos, pointDir;
			VRGrab::GetCalibratedHandSelectPose(vr, physLeft, vr->m_SetupOrigin, palmPos, pinchPos, pointDir);
			handGrabCenter[h] = (palmPos + pinchPos) * 0.5f;
		}

		PendingActions frameActions{};
		bool anyActionProduced = false;

		// 2. Per-Hand Inventory Zone Evaluation, Grab, Instant Reload, Holster & Loss Protection
		for (int h = 0; h < 2; ++h)
		{
			const bool physLeft = (h == 1);
			HandInventoryState& st = s_Hand[h];

			const bool isCamHand = (physLeft == vr->m_LeftHanded);
			const bool isBare = isCamHand
				? (vr->m_RightHandEmpty && !vr->m_PhoneEquipped)
				: (!vr->m_FlashlightEquipped);
			const bool holdingProp = vr->m_IsHoldingObject && (vr->m_HoldingWithLeftHand == physLeft);

			if ((holdingProp || phonePauseHolsterOnly) && st.holdingBattery != BatteryType::None)
			{
				st.holdingBattery = BatteryType::None;
				s_IsHoldingBattery[h].store(false, std::memory_order_relaxed);
			}

			// Evaluate eligible inventory slots for this hand
			struct SlotCandidate
			{
				int id;
				Vector pos;
				float enterRad;
				float exitRad;
				bool isHolster;
			};
			SlotCandidate cands[Slot_Count]{};
			int candCount = 0;

			if (!holdingProp)
			{
				if (st.holdingBattery == BatteryType::D)
				{
					if (!phonePauseHolsterOnly && beltEnabled && snap.valid)
						cands[candCount++] = { Slot_BeltDBattery, posBeltDBattery, kBatteryGrabRadius, kBatteryExitRadius, false };
					if (!phonePauseHolsterOnly && wristEnabled && snap.valid && haveFlWristPose && physLeft != flPhysLeft)
						cands[candCount++] = { Slot_WristDBattery, posWristDBattery, kWristGrabRadius, kWristExitRadius, false };
				}
				else if (st.holdingBattery == BatteryType::AA)
				{
					if (!phonePauseHolsterOnly && beltEnabled && snap.valid)
						cands[candCount++] = { Slot_BeltAABattery, posBeltAABattery, kBatteryGrabRadius, kBatteryExitRadius, false };
					if (!phonePauseHolsterOnly && wristEnabled && snap.valid && haveCamWristPose && physLeft != camPhysLeft)
						cands[candCount++] = { Slot_WristAABattery, posWristAABattery, kWristGrabRadius, kWristExitRadius, false };
				}
				else if (isBare)
				{
					if (!isCamHand && showBeltFlashlight)
						cands[candCount++] = { Slot_BeltFlashlight, posBeltFlashlight, kBeltGrabRadius, kBeltExitRadius, false };
					if (isCamHand && showBeltCamera && vr->m_RightHandEmpty)
						cands[candCount++] = { Slot_BeltCamera, posBeltCamera, kBeltGrabRadius, kBeltExitRadius, false };
					if (isCamHand && showBeltPhone && !phoneHolsterCooldown && vr->m_RightHandEmpty)
						cands[candCount++] = { Slot_BeltPhone, posBeltPhone, kPhoneGrabRadius, kPhoneExitRadius, false };
					if (showBeltDBattery)
						cands[candCount++] = { Slot_BeltDBattery, posBeltDBattery, kBatteryGrabRadius, kBatteryExitRadius, false };
					if (showBeltAABattery)
						cands[candCount++] = { Slot_BeltAABattery, posBeltAABattery, kBatteryGrabRadius, kBatteryExitRadius, false };

					// Wrist batteries are grabbed by the opposite bare hand
					if (physLeft != flPhysLeft && showWristDBattery)
						cands[candCount++] = { Slot_WristDBattery, posWristDBattery, kWristGrabRadius, kWristExitRadius, false };
					if (physLeft != camPhysLeft && showWristAABattery)
						cands[candCount++] = { Slot_WristAABattery, posWristAABattery, kWristGrabRadius, kWristExitRadius, false };
				}
				else
				{
					if (st.requireHolsterZoneExit)
					{
						const Vector ownHolsterPos = isCamHand
							? (vr->m_PhoneEquipped ? posBeltPhone : posBeltCamera)
							: posBeltFlashlight;
						const float sinceEquipMs = std::chrono::duration<float, std::milli>(nowSteady - st.lastEquipTime).count();
						if (sinceEquipMs >= 450.0f && Len3(handGrabCenter[h] - ownHolsterPos) > (kHolsterExitRadius + 1.2f))
						{
							st.requireHolsterZoneExit = false;
						}
					}
					if (!st.requireHolsterZoneExit)
					{
						if (!isCamHand && canHolsterFlashlight)
							cands[candCount++] = { Slot_BeltFlashlight, posBeltFlashlight, kHolsterRadius, kHolsterExitRadius, true };
						if (isCamHand && canHolsterCamera)
							cands[candCount++] = { Slot_BeltCamera, posBeltCamera, kHolsterRadius, kHolsterExitRadius, true };
						if (isCamHand && canHolsterPhone)
							cands[candCount++] = { Slot_BeltPhone, posBeltPhone, kHolsterRadius, kHolsterExitRadius, true };
					}
				}
			}

			int bestSlot = Slot_None;
			bool bestIsHolster = false;
			float bestDist = 1e9f;
			for (int ci = 0; ci < candCount; ++ci)
			{
				const float d = Len3(handGrabCenter[h] - cands[ci].pos);
				const float thresh = (st.hoverSlot == cands[ci].id) ? cands[ci].exitRad : cands[ci].enterRad;
				if (d <= thresh && d < bestDist)
				{
					bestDist = d;
					bestSlot = cands[ci].id;
					bestIsHolster = cands[ci].isHolster;
				}
			}

			const bool inZone = (bestSlot != Slot_None);
			s_IsInInventoryZone[h].store(inZone, std::memory_order_relaxed);

			if (bestSlot != Slot_None && bestSlot != st.hoverSlot)
			{
				vr->TriggerHapticPulseOnHand(physLeft, 750, 0.08f);
			}
			st.hoverSlot = bestSlot;

			bool digitalGrip = false;
			bool autoLatchedOnHolsterEntry = false;
			const bool gripDown = EvaluateInventoryHandGrip(vr, physLeft, st, inZone, bestIsHolster, digitalGrip, autoLatchedOnHolsterEntry);
			const bool gripJustPressed        = gripDown && !st.prevGripDown;
			const bool gripJustReleased       = !gripDown && st.prevGripDown;
			const bool digitalGripJustPressed = digitalGrip && !st.prevDigitalGrip;
			const bool freshCurlGripPressed   = gripJustPressed && !autoLatchedOnHolsterEntry;
			const bool holsterRegrabCooldown  = (nowSteady < st.holsterRegrabCooldownUntil);

			if (!gripDown)
				st.blockGrabUntilRelease = false;

			// If currently holding a battery in this hand: check return-to-belt, instant reload, or loss-protection release
			if (st.holdingBattery != BatteryType::None)
			{
				if (!gripDown)
				{
					// Clean return to belt / reserve
					st.holdingBattery = BatteryType::None;
					s_IsHoldingBattery[h].store(false, std::memory_order_relaxed);
					st.curlLatched = false;
					st.curlArmed = false;
					st.blockGrabUntilRelease = true;
					vr->TriggerHapticPulseOnHand(physLeft, 650, 0.16f);
				}
				else
				{
					const Vector heldPos = haveHandPose[h] ? handGripPose[h].gripWorldPos : handGrabCenter[h];
					if (st.holdingBattery == BatteryType::D &&
					    vr->m_FlashlightEquipped &&
					    snap.valid &&
					    !snap.flashlightBroken &&
					    snap.flashlightBattery < 100 &&
					    !snap.hasSuperFlashlight)
					{
						float flDist = 1e9f;
						if (VRHands::g_ViewModelLens.tailUpdated)
						{
							flDist = DistPointToSegment(heldPos, VRHands::g_ViewModelLens.tailPos, VRHands::g_ViewModelLens.pos);
						}
						else if (haveFlWristPose)
						{
							flDist = Len3(heldPos - flWristHp.gripWorldPos);
						}

						if (flDist <= kReloadSnapDist)
						{
							frameActions.instantReloadFlashlight = true;
							anyActionProduced = true;
							st.holdingBattery = BatteryType::None;
							s_IsHoldingBattery[h].store(false, std::memory_order_relaxed);
							st.blockGrabUntilRelease = true;
							PlayInventorySound(game, "items/batterypack_001.wav");
							vr->TriggerHapticPulseOnHand(true,  900, 0.85f);
							vr->TriggerHapticPulseOnHand(false, 900, 0.85f);
						}
					}
					else if (st.holdingBattery == BatteryType::AA &&
					         !vr->m_RightHandEmpty &&
					         !vr->m_PhoneEquipped &&
					         snap.valid &&
					         snap.cameraBattery < 100)
					{
						float camDist = 1e9f;
						if (VRHands::g_ViewModelLens.cameraUpdated)
						{
							camDist = Len3(heldPos - VRHands::g_ViewModelLens.camBodyPos);
						}
						else if (haveCamWristPose)
						{
							camDist = Len3(heldPos - camWristHp.gripWorldPos);
						}

						if (camDist <= kReloadSnapDist)
						{
							frameActions.instantReloadCamera = true;
							anyActionProduced = true;
							st.holdingBattery = BatteryType::None;
							s_IsHoldingBattery[h].store(false, std::memory_order_relaxed);
							st.blockGrabUntilRelease = true;
							PlayInventorySound(game, "items/batterypack_001.wav");
							vr->TriggerHapticPulseOnHand(true,  900, 0.85f);
							vr->TriggerHapticPulseOnHand(false, 900, 0.85f);
						}
					}
				}
			}
			else if (!holdingProp && !st.prevHoldingProp && !st.blockGrabUntilRelease && !holsterRegrabCooldown)
			{
				// Grabbing from belt / wrist on fresh grip press
				if (bestSlot != Slot_None && !bestIsHolster && gripJustPressed)
				{
					switch (bestSlot)
					{
					case Slot_BeltFlashlight:
					{
						if (haveHandPose[h])
						{
							const float gripDot = Dot3(handGripPose[h].thumbRadialDir, beltFlashlightLensDir);
							vr->m_FlashlightOverhandGrip = (gripDot > 0.0f);
						}
						else
						{
							vr->m_FlashlightOverhandGrip = false;
						}
						VRHands::ResetFlashlightGripAnim();
						frameActions.equipFlashlight = true;
						anyActionProduced = true;
						st.blockGrabUntilRelease = true;
						st.requireHolsterZoneExit = true;
						st.lastEquipTime = nowSteady;
						st.holsterGripArmed = false;
						st.hasHolsterHoverStart = false;
						vr->TriggerHapticPulseOnHand(physLeft, 850, 0.50f);
						break;
					}
					case Slot_BeltCamera:
					{
						frameActions.equipCamera = true;
						anyActionProduced = true;
						st.blockGrabUntilRelease = true;
						st.requireHolsterZoneExit = true;
						st.lastEquipTime = nowSteady;
						st.holsterGripArmed = false;
						st.hasHolsterHoverStart = false;
						vr->TriggerHapticPulseOnHand(physLeft, 850, 0.50f);
						break;
					}
					case Slot_BeltPhone:
					{
						vr->OpenPhonePauseMenu();
						st.blockGrabUntilRelease = true;
						st.requireHolsterZoneExit = true;
						st.lastEquipTime = nowSteady;
						st.holsterGripArmed = false;
						st.hasHolsterHoverStart = false;
						vr->TriggerHapticPulseOnHand(physLeft, 850, 0.50f);
						break;
					}
					case Slot_BeltDBattery:
					case Slot_WristDBattery:
					{
						st.holdingBattery = BatteryType::D;
						s_IsHoldingBattery[h].store(true, std::memory_order_relaxed);
						vr->TriggerHapticPulseOnHand(physLeft, 800, 0.40f);
						break;
					}
					case Slot_BeltAABattery:
					case Slot_WristAABattery:
					{
						st.holdingBattery = BatteryType::AA;
						s_IsHoldingBattery[h].store(true, std::memory_order_relaxed);
						vr->TriggerHapticPulseOnHand(physLeft, 800, 0.40f);
						break;
					}
					default:
						break;
					}
				}
			}

			// Holstering held Flashlight / Camera / Phone back into empty belt / pocket slot on Grip press or release
			if (bestSlot != Slot_None && bestIsHolster && !holdingProp && !st.prevHoldingProp)
			{
				if (!st.hasHolsterHoverStart)
				{
					st.hasHolsterHoverStart = true;
					st.holsterHoverStart = nowSteady;
				}
				const float hoverMs = std::chrono::duration<float, std::milli>(nowSteady - st.holsterHoverStart).count();

				const bool holsterPressTrigger   = !st.blockGrabUntilRelease && (digitalGripJustPressed || freshCurlGripPressed);
				const bool holsterReleaseTrigger = gripJustReleased && st.holsterGripArmed && (st.prevDigitalGrip || hoverMs >= 60.0f);
				if (holsterPressTrigger || holsterReleaseTrigger)
				{
					st.holsterGripArmed = false;
					st.hasHolsterHoverStart = false;
					st.blockGrabUntilRelease = true;
					st.holsterRegrabCooldownUntil = nowSteady + std::chrono::milliseconds(400);
					if (bestSlot == Slot_BeltFlashlight && vr->m_FlashlightEquipped)
					{
						frameActions.holsterFlashlight = true;
						anyActionProduced = true;
						vr->TriggerHapticPulseOnHand(physLeft, 750, 0.38f);
					}
					else if (bestSlot == Slot_BeltCamera && !vr->m_RightHandEmpty && !vr->m_PhoneEquipped)
					{
						frameActions.holsterCamera = true;
						anyActionProduced = true;
						vr->TriggerHapticPulseOnHand(physLeft, 750, 0.38f);
					}
					else if (bestSlot == Slot_BeltPhone && vr->m_PhoneEquipped)
					{
						vr->ClosePhonePauseMenu();
						s_PhoneHolsterCooldownUntil = nowSteady + std::chrono::milliseconds(600);
						vr->TriggerHapticPulseOnHand(physLeft, 750, 0.38f);
					}
				}
				else if (gripDown)
				{
					st.holsterGripArmed = true;
				}
			}
			else
			{
				st.holsterGripArmed = false;
				st.hasHolsterHoverStart = false;
			}

			st.prevGripDown = gripDown;
			st.prevDigitalGrip = digitalGrip;
			st.prevHoldingProp = holdingProp;
		}

		if (anyActionProduced)
		{
			std::lock_guard<std::mutex> actLock(s_ActionMutex);
			if (frameActions.equipFlashlight)         s_PendingActions.equipFlashlight = true;
			if (frameActions.holsterFlashlight)       s_PendingActions.holsterFlashlight = true;
			if (frameActions.equipCamera)             s_PendingActions.equipCamera = true;
			if (frameActions.holsterCamera)           s_PendingActions.holsterCamera = true;
			if (frameActions.instantReloadCamera)     s_PendingActions.instantReloadCamera = true;
			if (frameActions.instantReloadFlashlight) s_PendingActions.instantReloadFlashlight = true;
		}

		// 3. Build World Model Render List, Holster Marker List & Count Badge List for This Frame
		s_RenderItemCount = 0;
		s_BadgeCount = 0;
		s_MarkerCount = 0;

		auto pushItem = [&](ModelKind kind, const Vector& center, const Vector& ax, const Vector& ay, const Vector& az, float scale, const Vector& localCtr)
		{
			if (s_RenderItemCount >= kMaxRenderItems)
				return;
			BuildCenteredRenderItem(kind, center, ax, ay, az, scale, localCtr, s_RenderItems[s_RenderItemCount++]);
		};

		auto pushBadge = [&](const Vector& pos, int count, int iconCellIdx)
		{
			if (!vr->m_ShowBatteryCounts || s_BadgeCount >= kMaxBadges || count <= 0)
				return;
			s_Badges[s_BadgeCount++] = { true, pos, count, iconCellIdx };
		};

		auto pushMarker = [&](const Vector& pos, int colorIdx, bool occupied, bool hovered)
		{
			if (s_MarkerCount >= kMaxMarkers)
				return;
			s_Markers[s_MarkerCount++] = { true, pos, colorIdx, occupied, hovered };
		};

		auto isSlotHovered = [&](int slotId) -> bool
		{
			return (s_Hand[0].hoverSlot == slotId) || (s_Hand[1].hoverSlot == slotId);
		};

		const bool lookingDownAtBelt = (hmdPitch >= 18.0f);
		const bool anyBeltHover =
			(s_Hand[0].hoverSlot >= Slot_BeltFlashlight && s_Hand[0].hoverSlot <= Slot_BeltPhone) ||
			(s_Hand[1].hoverSlot >= Slot_BeltFlashlight && s_Hand[1].hoverSlot <= Slot_BeltPhone);
		s_BeltUiVisible = beltEnabled && (snap.valid || phonePauseHolsterOnly) && (lookingDownAtBelt || anyBeltHover);

		// Show clean subtle empty-slot rings only when looking down at the belt or hovering,
		// and highlight rings only on hover (never idle floating dots/rings over 3D models or wrists).
		if (s_BeltUiVisible)
		{
			if (!phonePauseHolsterOnly && vr->m_BeltHolsterEnabled && snap.hasFlashlight)
			{
				const bool hov = isSlotHovered(Slot_BeltFlashlight);
				if (!showBeltFlashlight || hov)
					pushMarker(posBeltFlashlight, hov ? Color_Green : Color_DimGray, false, hov);
			}
			if (!phonePauseHolsterOnly && (snap.hasFlashlight || snap.dBatteryReserve > 0))
			{
				const bool hov = isSlotHovered(Slot_BeltDBattery);
				if (!showBeltDBattery || hov)
					pushMarker(posBeltDBattery, hov ? Color_Green : Color_DimGray, false, hov);
			}
			if (!phonePauseHolsterOnly && (snap.hasCamera || snap.aaBatteryReserve > 0))
			{
				const bool hov = isSlotHovered(Slot_BeltAABattery);
				if (!showBeltAABattery || hov)
					pushMarker(posBeltAABattery, hov ? Color_Cyan : Color_DimGray, false, hov);
			}
			if (!phonePauseHolsterOnly && vr->m_BeltHolsterEnabled && snap.hasCamera)
			{
				const bool hov = isSlotHovered(Slot_BeltCamera);
				if (!showBeltCamera || hov)
					pushMarker(posBeltCamera, hov ? Color_Cyan : Color_DimGray, false, hov);
			}
			if (vr->m_BeltHolsterEnabled && (canOpenPhoneMenu || vr->m_PhoneEquipped))
			{
				const bool hov = isSlotHovered(Slot_BeltPhone);
				if (!showBeltPhone || hov)
					pushMarker(posBeltPhone, hov ? Color_Cyan : Color_DimGray, false, hov);
			}
		}
		if (wristEnabled && snap.valid)
		{
			if (showWristDBattery && isSlotHovered(Slot_WristDBattery))
				pushMarker(posWristDBattery, Color_Green, false, true);
			if (showWristAABattery && isSlotHovered(Slot_WristAABattery))
				pushMarker(posWristAABattery, Color_Cyan, false, true);
		}

		// Belt world models
		if (showBeltFlashlight)
		{
			if (snap.hasSuperFlashlight)
			{
				const Vector zAx = -beltFlashlightLensDir; // w_superlight lens is -Z_local
				const Vector xAx = NormalizeSafe(beltRight - zAx * Dot3(beltRight, zAx), beltFwd);
				const Vector yAx = NormalizeSafe(Cross3(zAx, xAx), beltFwd);
				pushItem(ModelKind::SuperFlashlight, posBeltFlashlight, xAx, yAx, zAx, 0.82f, kCenterSuperLight);
			}
			else
			{
				const Vector xAx = beltFlashlightLensDir; // w_light lens is +X_local
				const Vector yAx = NormalizeSafe(Cross3(beltFwd, xAx), beltRight);
				const Vector zAx = NormalizeSafe(Cross3(xAx, yAx), beltFwd);
				pushItem(ModelKind::Flashlight, posBeltFlashlight, xAx, yAx, zAx, 0.85f, kCenterLight);
			}
		}

		// Belt battery packs oriented flat-forward and facing straight up toward eyes when looking down
		const Vector cardFront = beltUp;
		const Vector cardRight = beltRight;
		const Vector cardTop   = NormalizeSafe(Cross3(cardFront, cardRight), beltFwd);

		if (showBeltDBattery)
		{
			// batterypack_002: header at -X_local, text reads -Y -> +Y, blister at +Z_local
			const Vector xAx = -cardTop;
			const Vector yAx = cardRight;
			const Vector zAx = cardFront;
			pushItem(ModelKind::DBattery, posBeltDBattery, xAx, yAx, zAx, 0.68f, kCenterDBattery);
			if (s_BeltUiVisible)
				pushBadge(posBeltDBattery + beltUp * 1.85f + beltFwd * 0.6f, snap.dBatteryReserve, Icon_Flashlight);
		}

		if (showBeltAABattery)
		{
			// batterypack_001: header at -Y_local, text upright across -X -> +X, blister at +Z_local
			const Vector xAx = -cardRight;
			const Vector yAx = -cardTop;
			const Vector zAx = cardFront;
			pushItem(ModelKind::AABattery, posBeltAABattery, xAx, yAx, zAx, 0.75f, kCenterAABattery);
			if (s_BeltUiVisible)
				pushBadge(posBeltAABattery + beltUp * 1.85f + beltFwd * 0.6f, snap.aaBatteryReserve, Icon_Camera);
		}

		if (showBeltCamera)
		{
			// w_camera P2B local axes: lens downward (-beltUp), back LCD upward toward eyes, shutter top forward, right grip outer right
			const Vector kCamLocalLens = NormalizeSafe(Vector(0.8616f, -0.1130f, 0.4948f), Vector(1.0f, 0.0f, 0.0f));
			const Vector rawCamTop( -0.4980f, -0.0005f, 0.8672f );
			const Vector kCamLocalTop = NormalizeSafe(rawCamTop - kCamLocalLens * Dot3(rawCamTop, kCamLocalLens), Vector(0.0f, 0.0f, 1.0f));
			const Vector kCamLocalRight = NormalizeSafe(Cross3(kCamLocalLens, kCamLocalTop), Vector(0.0f, -1.0f, 0.0f));

			const Vector worldLens  = -beltUp;
			const Vector worldTop   = NormalizeSafe(beltFwd - worldLens * Dot3(beltFwd, worldLens), beltFwd);
			const Vector worldRight = NormalizeSafe(Cross3(worldLens, worldTop), beltRight);

			const Vector xAx = worldRight * kCamLocalRight.x + worldLens * kCamLocalLens.x + worldTop * kCamLocalTop.x;
			const Vector yAx = worldRight * kCamLocalRight.y + worldLens * kCamLocalLens.y + worldTop * kCamLocalTop.y;
			const Vector zAx = worldRight * kCamLocalRight.z + worldLens * kCamLocalLens.z + worldTop * kCamLocalTop.z;
			pushItem(ModelKind::Camera, posBeltCamera, xAx, yAx, zAx, 0.90f, kCenterCamera);
		}

		if (showBeltPhone)
		{
			// mobile_phone_001.mdl: +X = screen front, +Y = screen left, +Z = top (earpiece)
			// Strictly vertical (-beltUp downward) along the side pocket, screen facing inward/slightly back.
			const Vector zAx = beltUp;
			const Vector xAx = NormalizeSafe(-beltRight * (0.90f * mirrorSign) - beltFwd * 0.44f, -beltRight * mirrorSign);
			const Vector yAx = NormalizeSafe(Cross3(zAx, xAx), beltFwd);
			pushItem(ModelKind::Phone, posBeltPhone, xAx, yAx, zAx, 0.96f, kCenterPhone);
		}

		// Wrist-hole miniature battery packs
		if (showWristDBattery)
		{
			const Vector xAx = NormalizeSafe(-flWristHp.fingerDistalDir, beltFwd);
			const Vector zAx = NormalizeSafe(-flWristHp.palmOutwardNormal * 0.78f + flWristHp.thumbRadialDir * 0.62f, beltUp);
			const Vector yAx = NormalizeSafe(Cross3(zAx, xAx), flWristHp.thumbRadialDir);
			const Vector xOrtho = NormalizeSafe(Cross3(yAx, zAx), xAx);
			pushItem(ModelKind::DBattery, posWristDBattery, xOrtho, yAx, zAx, 0.42f, kCenterDBattery);
			if (!beltEnabled)
				pushBadge(posWristDBattery + zAx * 1.45f, snap.dBatteryReserve, Icon_Flashlight);
		}

		if (showWristAABattery)
		{
			const Vector xAx = NormalizeSafe(-camWristHp.fingerDistalDir, beltFwd);
			const Vector zAx = NormalizeSafe(-camWristHp.palmOutwardNormal * 0.78f + camWristHp.thumbRadialDir * 0.62f, beltUp);
			const Vector yAx = NormalizeSafe(Cross3(zAx, xAx), camWristHp.thumbRadialDir);
			const Vector xOrtho = NormalizeSafe(Cross3(yAx, zAx), xAx);
			pushItem(ModelKind::AABattery, posWristAABattery, xOrtho, yAx, zAx, 0.45f, kCenterAABattery);
			if (!beltEnabled)
				pushBadge(posWristAABattery + zAx * 1.45f, snap.aaBatteryReserve, Icon_Camera);
		}

		// Held batteries in hands: single round battery cell across palmar grip pocket
		for (int h = 0; h < 2; ++h)
		{
			if (s_Hand[h].holdingBattery == BatteryType::None)
				continue;

			const Vector ctr = haveHandPose[h] ? handGripPose[h].gripWorldPos : handGrabCenter[h];
			const Vector xAx = haveHandPose[h] ? handGripPose[h].thumbRadialDir : beltRight;
			const Vector yAx = haveHandPose[h] ? handGripPose[h].fingerDistalDir : beltFwd;
			const Vector zAx = NormalizeSafe(Cross3(xAx, yAx), beltUp);
			const Vector yOrtho = NormalizeSafe(Cross3(zAx, xAx), yAx);
			const Vector zeroCenter(0.0f, 0.0f, 0.0f);

			if (s_Hand[h].holdingBattery == BatteryType::D)
				pushItem(ModelKind::DBatterySingle, ctr, xAx, yOrtho, zAx, 1.05f, zeroCenter);
			else
				pushItem(ModelKind::AABatterySingle, ctr, xAx, yOrtho, zAx, 1.20f, zeroCenter);
		}
	}

	void BeginEyeRender(int eye)
	{
		s_ActiveEye = eye;
		s_RenderedThisEye = false;
		s_SawViewModelThisEye = false;
	}

	bool ShouldDrawWorldModelsNow(const char* modelName, const ModelRenderInfo_t& info)
	{
		if (s_ActiveEye == 0 || s_InInventoryDME)
			return false;
		if ((info.flags & 0x1) == 0 || (info.flags & 0x48000000) != 0)
			return false;
		if (!modelName)
			return false;
		if (_strnicmp(modelName, "models/weapons/v_", 17) != 0 &&
		    _strnicmp(modelName, "models\\weapons\\v_", 17) != 0)
		{
			return false;
		}
		s_SawViewModelThisEye = true;
		if (s_RenderedThisEye || s_RenderItemCount <= 0)
			return false;
		return true;
	}

	void DrawWorldModelsInDME(
		void* ecx,
		void* context,
		void* /*state*/,
		const ModelRenderInfo_t& /*pInfo*/,
		matrix3x4_t* /*pCustomBoneToWorld*/)
	{
		if (s_RenderedThisEye || s_InInventoryDME)
			return;
		s_RenderedThisEye = true;

		Game* game = g_Game;
		VR* vr = game ? game->m_VR : nullptr;
		if (!game || !vr || !vr->m_IsVREnabled || !game->m_ModelRender || !Hooks::hkDrawModelExecute.fOriginal)
			return;
		if (!ecx || !context)
			return;

		RenderItem localItems[kMaxRenderItems]{};
		int itemCount = 0;
		{
			std::lock_guard<std::mutex> lock(s_FrameMutex);
			itemCount = std::min(s_RenderItemCount, kMaxRenderItems);
			for (int i = 0; i < itemCount; ++i)
				localItems[i] = s_RenderItems[i];
		}
		if (itemCount <= 0)
			return;

		struct ScopedDmeGuard
		{
			bool& flag;
			~ScopedDmeGuard() { flag = false; }
		};
		s_InInventoryDME = true;
		ScopedDmeGuard guard{ s_InInventoryDME };

		game->m_ModelRender->ForcedMaterialOverride(nullptr);

		// Keep DME stub & transform storage alive across mat_queue_mode 2 queued frames (max 3 queued frames -> 8-slot ring)
		static StubClientRenderable s_DmeStubs[8][kMaxRenderItems]{};
		static matrix3x4_t s_DmeLightingMats[8][kMaxRenderItems]{};
		static matrix3x4_t s_DmeBoneMats[8][kMaxRenderItems][8]{};
		static uint32_t s_DmeRingCounter = 0u;
		const uint32_t ringSlot = (s_DmeRingCounter++) & 7u;

		for (int i = 0; i < itemCount; ++i)
		{
			const RenderItem& item = localItems[i];
			const char* path = ModelKindToPath(item.model);
			if (!path)
				continue;

			ResolvedWorldModel resolved = ResolveWorldModel(game, path);
			if (!resolved.valid && item.model == ModelKind::SuperFlashlight)
				resolved = ResolveWorldModel(game, kModelFlashlight);
			else if (!resolved.valid && item.model == ModelKind::DBatterySingle)
				resolved = ResolveWorldModel(game, kModelDBattery);
			else if (!resolved.valid && item.model == ModelKind::AABatterySingle)
				resolved = ResolveWorldModel(game, kModelAABattery);
			if (!resolved.valid)
				continue;

			StubClientRenderable& stub = s_DmeStubs[ringSlot][i];
			stub = StubClientRenderable{};
			stub.m_unknown.m_pRenderable = &stub;
			stub.m_origin = item.worldCenter;
			stub.m_angles = item.worldAngles;
			stub.m_pModel = resolved.pModel;
			stub.m_transform = item.lightingM2W;

			s_DmeLightingMats[ringSlot][i] = item.lightingM2W;

			matrix3x4_t stackBoneMats[128];
			matrix3x4_t* boneMats = (resolved.numBones <= 8) ? s_DmeBoneMats[ringSlot][i] : stackBoneMats;
			for (int b = 0; b < resolved.numBones; ++b)
				boneMats[b] = item.boneToWorld;

			LocalDrawModelState localState{};
			localState.m_pStudioHdr = resolved.pStudioHdr;
			localState.m_pStudioHWData = resolved.pStudioHWData;
			localState.m_pRenderable = &stub;
			localState.m_pModelToWorld = &s_DmeLightingMats[ringSlot][i];
			localState.m_decals = 0;
			localState.m_drawFlags = 0;
			localState.m_lod = 0;

			EngineModelRenderInfo mri{};
			mri.info.origin = item.worldCenter;
			mri.info.angles = item.worldAngles;
			mri.info.pRenderable = &stub;
			mri.info.pModel = resolved.pModel;
			mri.info.pModelToWorld = &s_DmeLightingMats[ringSlot][i];
			mri.info.pLightingOffset = nullptr;
			mri.info.pLightingOrigin = nullptr;
			mri.info.flags = 0x1 | 0x400;
			mri.info.entity_index = -1;
			mri.info.skin = 0;
			mri.info.body = 0;
			mri.info.hitboxset = 0;
			const float one = 1.0f;
			std::memcpy(&mri.info.instance, &one, sizeof(float));
			mri.instanceExt = 0xFFFFu;

			Hooks::hkDrawModelExecute.fOriginal(ecx, context, &localState, mri.info, boneMats);
		}
	}

	void DrawEyeOverlays(const CViewSetup& eyeView, ITexture* eyeTexture)
	{
		Game* game = g_Game;
		VR* vr = game ? game->m_VR : nullptr;
		if (!vr || !vr->m_IsVREnabled || vr->m_In3DViewControlMenu ||
		    !game->m_MaterialSystem || !game->m_EngineClient || !eyeTexture)
		{
			return;
		}
		const bool isPaused = game->m_EngineClient->IsPaused();
		const bool phonePauseHolsterOnly = isPaused && vr->m_PhoneEquipped && !vr->Is2DSubMenuVisible();
		if (!game->m_EngineClient->IsInGame() ||
		    game->m_EngineClient->IsLevelMainMenuBackground() ||
		    (isPaused && !phonePauseHolsterOnly))
		{
			return;
		}

		static PinnedMaterialSlot s_AtlasMatSlot;
		IMaterial* atlasMat = s_AtlasMatSlot.Get(game->m_MaterialSystem, "vr/vr_ui_atlas", "Other textures");
		if (!atlasMat)
			return;

		IMatRenderContext* ctx = game->m_MaterialSystem->GetRenderContext();
		if (!ctx)
			return;

		const int W = (eyeView.width > 0) ? eyeView.width : static_cast<int>(vr->m_RenderWidth);
		const int H = (eyeView.height > 0) ? eyeView.height : static_cast<int>(vr->m_RenderHeight);
		if (W <= 0 || H <= 0)
		{
			ctx->Release();
			return;
		}

		ctx->SetRenderTarget(eyeTexture);
		ctx->Viewport(0, 0, W, H);

		typedef void (__thiscall* tDrawScreenSpaceRectangle)(
			void* thisptr, IMaterial* mat,
			int destX, int destY, int width, int height,
			float srcX0, float srcY0, float srcX1, float srcY1,
			int srcTexW, int srcTexH, void* pClientRenderable,
			int nXDice, int nYDice);
		auto fnDrawRect = (*reinterpret_cast<tDrawScreenSpaceRectangle**>(ctx))[105];

		const float fw = static_cast<float>(W);
		const float fh = static_cast<float>(H);
		const float tanX = std::tan(eyeView.fov * 0.5f * kDeg2Rad);
		const ServerSnapshot snap = GetSnapshot();

		const bool allowBeltUiOverlays = vr->m_ShowHands && s_SawViewModelThisEye;

		// 0. Hollow empty-holster / hover rings
		if (allowBeltUiOverlays)
		{
			HolsterMarker localMarkers[kMaxMarkers]{};
			int markerCount = 0;
			{
				std::lock_guard<std::mutex> lock(s_FrameMutex);
				markerCount = std::min(s_MarkerCount, kMaxMarkers);
				for (int i = 0; i < markerCount; ++i)
					localMarkers[i] = s_Markers[i];
			}

			for (int i = 0; i < markerCount; ++i)
			{
				const HolsterMarker& m = localMarkers[i];
				if (!m.active)
					continue;

				float sx = 0.0f, sy = 0.0f, z = 0.0f;
				if (!ProjectWorldToEyeScreen(eyeView, fw, fh, m.worldPos, sx, sy, &z))
					continue;

				const float worldToPix = (tanX > 1e-4f) ? ((fw * 0.5f) / (z * tanX)) : 1.0f;
				const float baseWorldDiam = m.hovered ? 3.2f : 2.3f;
				const int ringSize = std::clamp(static_cast<int>(std::lround(baseWorldDiam * worldToPix)), 16, static_cast<int>(fh * 0.085f));
				const int cx = static_cast<int>(std::lround(sx));
				const int cy = static_cast<int>(std::lround(sy));

				const int ringColor = m.hovered ? Color_AltSlot : std::clamp(m.colorIdx, 0, 7);
				DrawAtlasCell(fnDrawRect, ctx, atlasMat, W, H,
				              cx - ringSize / 2, cy - ringSize / 2, ringSize, ringSize,
				              ringColor * 64, 384, 64);
			}
		}

		// 1. Battery count badges at belt / wrist slots
		if (allowBeltUiOverlays && vr->m_ShowBatteryCounts)
		{
			BadgeItem localBadges[kMaxBadges]{};
			int badgeCount = 0;
			{
				std::lock_guard<std::mutex> lock(s_FrameMutex);
				badgeCount = std::min(s_BadgeCount, kMaxBadges);
				for (int i = 0; i < badgeCount; ++i)
					localBadges[i] = s_Badges[i];
			}

			for (int i = 0; i < badgeCount; ++i)
			{
				const BadgeItem& b = localBadges[i];
				if (!b.active || b.count <= 0)
					continue;

				float sx = 0.0f, sy = 0.0f, z = 0.0f;
				if (!ProjectWorldToEyeScreen(eyeView, fw, fh, b.worldPos, sx, sy, &z))
					continue;

				const float worldToPix = (tanX > 1e-4f) ? ((fw * 0.5f) / (z * tanX)) : 1.0f;
				const int glyphH = std::clamp(static_cast<int>(std::lround(0.95f * worldToPix)), 10, static_cast<int>(fh * 0.052f));
				const int glyphW = glyphH;

				char text[12]{};
				std::snprintf(text, sizeof(text), "x%d", std::clamp(b.count, 0, 999));
				const int len = static_cast<int>(std::strlen(text));
				const int stepX = static_cast<int>(glyphW * 0.65f);
				const int iconSz = std::max(10, static_cast<int>(glyphH * 0.95f));
				const int padX = std::max(4, glyphH / 4);
				const int padY = std::max(3, glyphH / 6);
				const int totalW = padX * 2 + iconSz + 2 + (len - 1) * stepX + glyphW;
				const int totalH = glyphH + padY * 2;
				const int startX = static_cast<int>(std::lround(sx)) - totalW / 2;
				const int startY = static_cast<int>(std::lround(sy)) - totalH / 2;

				// Subtle pill backdrop
				DrawAtlasCell(fnDrawRect, ctx, atlasMat, W, H, startX, startY, totalW, totalH, 384, 256, 128);

				// Tool icon (row y=448, cell 4 = Flashlight, cell 5 = Camera)
				const int iconCell = std::clamp(b.iconCellIdx, static_cast<int>(Icon_Flashlight), static_cast<int>(Icon_Camera));
				DrawAtlasCell(fnDrawRect, ctx, atlasMat, W, H,
				              startX + padX, startY + (totalH - iconSz) / 2, iconSz, iconSz,
				              iconCell * 64, 448, 64);

				// Digits "xN"
				int curX = startX + padX + iconSz + 1;
				const int glyphY = startY + padY;
				for (int ci = 0; ci < len; ++ci)
				{
					int gx = 0, gy = 0;
					GetDigitCell(text[ci], gx, gy);
					DrawAtlasCell(fnDrawRect, ctx, atlasMat, W, H, curX, glyphY, glyphW, glyphH, gx, gy, 128);
					curX += stepX;
				}
			}
		}

		// 2. Flashlight Bottom Battery Ring (standard flashlight only; v_superlight has native backplate LEDs & infinite battery)
		if (vr->m_ShowFlashlightBatteryRing && vr->m_FlashlightEquipped &&
		    !snap.hasSuperFlashlight && !VRHands::g_ViewModelLens.isSuperLight &&
		    VRHands::g_ViewModelLens.tailUpdated && snap.valid)
		{
			const Vector& tailPos    = VRHands::g_ViewModelLens.tailPos;
			const Vector& tailNormal = VRHands::g_ViewModelLens.tailNormal;
			const Vector& tailU      = VRHands::g_ViewModelLens.tailAxisU;
			const Vector& tailV      = VRHands::g_ViewModelLens.tailAxisV;
			const Vector ringCenter  = tailPos + tailNormal * 0.002f;
			const float ringRadius   = std::clamp(VRHands::g_ViewModelLens.tailCapRadius * 0.56f, 0.20f, 1.00f);

			const Vector toEye = eyeView.origin - ringCenter;
			const float dist = Len3(toEye);
			if (dist >= 1.0f && (Dot3(toEye, tailNormal) / dist) > 0.12f)
			{
				if (IsLineOfSightClear(game->m_EngineTrace, eyeView.origin, ringCenter + tailNormal * 0.25f))
				{
					constexpr int kSegments = 24;
					const int pct = snap.flashlightBroken ? 0 : std::clamp(snap.flashlightBattery, 0, 100);
					const int litCount = (pct <= 0) ? 0 : std::clamp((pct * kSegments + 99) / 100, 1, kSegments);

					int activeColor = Color_Green;
					if (pct < 25)
					{
						if (pct < 15 || snap.flashlightBroken)
						{
							const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
								std::chrono::steady_clock::now().time_since_epoch()).count();
							const bool pulseBright = ((ms % 420LL) < 220LL);
							activeColor = pulseBright ? Color_Red : Color_AltSlot; // Red <-> Dim Red
						}
						else
						{
							activeColor = Color_Red;
						}
					}
					else if (pct < 60)
					{
						activeColor = Color_Yellow;
					}

					const float dotWorldDiam = ringRadius * 0.25f;
					for (int seg = 0; seg < kSegments; ++seg)
					{
						const float ang = (static_cast<float>(seg) / static_cast<float>(kSegments)) * (2.0f * kPi);
						const Vector segWorld = ringCenter + (tailV * std::cos(ang) + tailU * std::sin(ang)) * ringRadius;
						float sx = 0.0f, sy = 0.0f, z = 0.0f;
						if (!ProjectWorldToEyeScreen(eyeView, fw, fh, segWorld, sx, sy, &z))
							continue;

						const float worldToPix = (tanX > 1e-4f) ? ((fw * 0.5f) / (z * tanX)) : 1.0f;
						const int dotSize = std::clamp(static_cast<int>(std::lround(dotWorldDiam * worldToPix)), 3, 18);
						const bool isLit = (seg < litCount) || (snap.flashlightBroken && (seg % 2 == 0));
						const int colorCell = isLit ? activeColor : Color_Track;

						DrawAtlasCell(fnDrawRect, ctx, atlasMat, W, H,
						              static_cast<int>(std::lround(sx)) - dotSize / 2,
						              static_cast<int>(std::lround(sy)) - dotSize / 2,
						              dotSize, dotSize,
						              colorCell * 64, 448, 64);
					}
				}
			}
		}

		ctx->Release();
	}
}
