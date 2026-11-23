#include "hooks.h"
#include "game.h"
#include "texture.h"
#include "sdk.h"
#include "sdk_server.h"
#include "trace.h"
#include "vr.h"
#include "vr_audio.h"
#include "vr_hands.h"
#include "vr_grab.h"
#include "vr_inventory.h"
#include "vr_camera_flash.h"
#include "vr_viewmodel_depth.h"
#include "offsets.h"
#include <iostream>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstring>

// =============================================================================================
// Hand-based use selection ("virtual fingertip")
// ---------------------------------------------------------------------------------------------
// INFRA's own C_INFRA_Player::FindUseEntity scores candidates for a head/mouse view; with VR hands
// it picked effectively random neighbours on tightly packed panels (e.g. c2_m1 generator switches
// 4 units / ~9 cm apart). We select the use target ourselves, once per frame on the client:
//   1. Close range: usable entity whose oriented bounding box is nearest to a virtual fingertip
//      (UseTipDistance cm in front of the controller), within UseTouchRadius cm.
//   2. Far range: narrow pointing ray from the controller (~8 deg, up to ~1.9 m).
//   3. Hysteresis: the current target is kept unless another one is clearly closer (2 cm).
//   4. Haptic tick on the selecting controller whenever the target changes.
// The selection drives INFRA's selection outline (client) and the actual +use on the server
// (singleplayer: same process; mapped via the shared edict index), so what glows is what you use.
// Verified client.dll functions:
//   +0x1C8AF0  C_INFRA_Player::IsUseCandidateValid(C_BaseEntity*)  (use caps + parent visibility)
//   +0x0B7990  CCollisionProperty::CalcNearestPoint(const Vector&, Vector*)  (entity + 0x410)
// =============================================================================================
namespace
{
	enum class HandSelState { Skipped, None, Valid };

	struct HandUseSelection
	{
		HandSelState state = HandSelState::Skipped;
		void* clientEnt = nullptr;
		int entIndex = -1;
		bool leftHand = false; // logical hand (same semantics as VR::m_UseWithLeftHand)
		std::chrono::steady_clock::time_point time {};
	};

	HandUseSelection s_HandSel;
	int s_HandSelFrame = -1;
	int s_RenderFrameId = 0;

	// Flashlight lens position and outward normal captured while drawing the flashlight viewmodel (see DrawFlashlightLensGlow)
	Vector s_LensPos = { 0.0f, 0.0f, 0.0f };
	Vector s_LensNormal = { 1.0f, 0.0f, 0.0f };
	int s_LensFrame = -1;

	class CTraceFilterWorldOnlyUse : public CTraceFilter
	{
	public:
		CTraceFilterWorldOnlyUse() : CTraceFilter(nullptr, 0) {}
		virtual bool ShouldHitEntity(IHandleEntity*, int) override { return false; }
		virtual TraceType GetTraceType() const override { return TraceType::TRACE_WORLD_ONLY; }
	};

	inline float Dot3(const Vector& a, const Vector& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

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
			unsigned long long nowMs = GetTickCount64();
			if (lastRetryMs != 0 && (nowMs - lastRetryMs) < 1000ULL)
				return nullptr;
			lastRetryMs = nowMs;
			IMaterial* found = matSys->FindMaterial(name, group, false);
			if (found && !found->IsErrorMaterial())
			{
				// Pin reference count so CMaterialSystem::UncacheUnusedMaterials() never frees
				// cached materials across level transitions or returning to the main menu.
				found->IncrementReferenceCount();
				mat = found;
				return mat;
			}
			return nullptr;
		}
	};

	struct StudioLodDataLocal
	{
		void*       m_pMeshData;
		float       m_SwitchPoint;
		int         numMaterials;
		IMaterial** ppMaterials;
		int*        pMaterialFlags;
		int*        m_pHWMorphDecalBoneRemap;
		int         m_nDecalBoneCount;
		uint32_t    _pad;
	};
	static_assert(sizeof(StudioLodDataLocal) == 32, "studioloddata_t size mismatch");

	struct StudioHWDataLocal
	{
		int                 m_RootLOD;
		int                 m_NumLODs;
		StudioLodDataLocal* m_pLODs;
		int                 m_NumStudioMeshes;
		uint32_t            m_Reserved[4];
	};

	struct DrawModelStateLocal
	{
		void*              m_pStudioHdr;
		StudioHWDataLocal* m_pStudioHWData;
		void*              m_pRenderable;
		const matrix3x4_t* m_pModelToWorld;
		void*              m_decals;
		int                m_drawFlags;
		int                m_lod;
	};
}

static bool ContainsCaseInsensitive(const char* haystack, const char* needle);

namespace
{
	void DrawHandInteriorBlackPass(
		void* modelRenderThis,
		void* ctx,
		void* state,
		const ModelRenderInfo_t& info,
		void* pCustomBoneToWorld,
		Game* game,
		VR* vr,
		bool alreadyFlipped = false)
	{
		if (!vr || !vr->m_IsVREnabled || !vr->m_ShowHands || !game || !game->m_MaterialSystem ||
		    !modelRenderThis || !ctx || !state || !pCustomBoneToWorld || !Hooks::hkDrawModelExecute.fOriginal)
		{
			return;
		}

		// Skip shadow depth / SSAO depth-only render passes
		if ((info.flags & 0x48000000) != 0)
			return;

		static PinnedMaterialSlot s_BlackInteriorMat;
		static PinnedMaterialSlot s_NoDrawMat;
		IMaterial* blackMat = s_BlackInteriorMat.Get(game->m_MaterialSystem, "vr/hand_interior_black", "Model textures");
		IMaterial* noDrawMat = s_NoDrawMat.Get(game->m_MaterialSystem, "vr/hand_nodraw", "Model textures");
		if (!blackMat || !noDrawMat)
			return;

		noDrawMat->SetMaterialVarFlag(MATERIAL_VAR_NO_DRAW, true);
		blackMat->SetMaterialVarFlag(MATERIAL_VAR_NO_DRAW, false);
		blackMat->SetMaterialVarFlag(MATERIAL_VAR_NOCULL, false);

		const auto* srcState = reinterpret_cast<const DrawModelStateLocal*>(state);
		const StudioHWDataLocal* srcHW = srcState->m_pStudioHWData;
		if (!srcHW || !srcHW->m_pLODs || srcHW->m_NumLODs <= 0 || srcHW->m_NumLODs > 8)
			return;

		// Keep 16-slot ring buffer alive across mat_queue_mode 2 queued frames
		static DrawModelStateLocal s_InteriorState[16]{};
		static StudioHWDataLocal   s_InteriorHWData[16]{};
		static StudioLodDataLocal  s_InteriorLODs[16][8]{};
		static IMaterial*          s_InteriorMats[16][8][32]{};
		static uint32_t            s_InteriorRingCounter = 0u;

		const uint32_t slot = (s_InteriorRingCounter++) & 15u;
		DrawModelStateLocal& dstState = s_InteriorState[slot];
		StudioHWDataLocal& dstHW = s_InteriorHWData[slot];
		dstState = *srcState;
		dstHW = *srcHW;
		dstState.m_pStudioHWData = &dstHW;
		dstHW.m_pLODs = s_InteriorLODs[slot];

		bool foundHandMat = false;
		const int numLODs = srcHW->m_NumLODs;
		for (int l = 0; l < numLODs; ++l)
		{
			const StudioLodDataLocal& srcLod = srcHW->m_pLODs[l];
			StudioLodDataLocal& dstLod = s_InteriorLODs[slot][l];
			dstLod = srcLod;
			if (srcLod.numMaterials > 0 && srcLod.numMaterials <= 32 && srcLod.ppMaterials != nullptr)
			{
				for (int m = 0; m < srcLod.numMaterials; ++m)
				{
					IMaterial* origMat = srcLod.ppMaterials[m];
					const char* matName = origMat ? origMat->GetName() : nullptr;
					if (ContainsCaseInsensitive(matName, "player_hands"))
					{
						s_InteriorMats[slot][l][m] = blackMat;
						foundHandMat = true;
					}
					else
					{
						s_InteriorMats[slot][l][m] = noDrawMat;
					}
				}
				dstLod.ppMaterials = s_InteriorMats[slot][l];
			}
		}

		if (!foundHandMat)
			return;

		void** ctxVt = *reinterpret_cast<void***>(ctx);
		if (!ctxVt || !ctxVt[41])
			return;

		typedef void(__thiscall* tFlipCullMode)(void* thisptr);
		auto fnFlipCullMode = reinterpret_cast<tFlipCullMode>(ctxVt[41]);

		if (!alreadyFlipped)
			fnFlipCullMode(ctx);
		Hooks::hkDrawModelExecute.fOriginal(modelRenderThis, ctx, &dstState, info, pCustomBoneToWorld);
		if (!alreadyFlipped)
			fnFlipCullMode(ctx);
	}
}

static void* ServerEntityByIndex(int entIndex);
static bool s_HasCustomUseAngle = false;
static QAngle s_CustomUseAngle(0.0f, 0.0f, 0.0f);
static void* s_LastUseServerEnt = nullptr;
static void* s_PrevServerPlayer = nullptr;
static std::chrono::steady_clock::time_point s_LastCameraPhotoTime{};
static void* s_LastPhotoCam = nullptr;
static bool s_PendingFlashFired = false;
static std::chrono::steady_clock::time_point s_PendingFlashFireTime{};
static std::atomic<uint32_t> s_ServerPhotoTakenSeq{ 0u };
static uint32_t s_ConsumedPhotoTakenSeq = 0u;
static std::atomic<bool> s_CameraScreenResetPending{ false };
static void* s_LastCamScreen = nullptr;
static int s_LastFlashMode = -1;
static uint32_t s_CameraScreenVerifyCounter = 0u;

static void ResetCameraScreenCache()
{
	s_CameraScreenResetPending.store(true, std::memory_order_release);
	s_LastPhotoCam = nullptr;
	s_PendingFlashFired = false;
	s_ConsumedPhotoTakenSeq = s_ServerPhotoTakenSeq.load(std::memory_order_acquire);
}

static QAngle DirToQAngle(const Vector& dir)
{
	const float horiz = std::sqrt(dir.x * dir.x + dir.y * dir.y);
	constexpr float kRadToDeg = 180.0f / 3.14159265358979323846f;
	float pitch = -std::atan2(dir.z, horiz) * kRadToDeg;
	float yaw = (horiz > 1e-5f) ? (std::atan2(dir.y, dir.x) * kRadToDeg) : 0.0f;
	return QAngle(pitch, yaw, 0.0f);
}

static void NotifyServerFieldChanged(uintptr_t baseSrv, void* entity, int fieldOffset)
{
	if (!entity || !baseSrv)
		return;
	if (*(uint8_t*)((uintptr_t)entity + 0x58) != 0)
	{
		*(uint8_t*)((uintptr_t)entity + 0x5C) |= 1u;
		return;
	}
	void* pEdict = *(void**)((uintptr_t)entity + 0x1C);
	if (pEdict)
	{
		typedef void(__thiscall* tNetworkStateChanged)(void* edictPtr, int offset);
		auto fnNetChanged = (tNetworkStateChanged)(baseSrv + 0xFCE0);
		fnNetChanged(pEdict, fieldOffset);
	}
}

// ---------------------------------------------------------------------------------------------
// Flashlight lens glow & blinding glare: drawn in screen space into each eye texture.
// IMatRenderContext::DrawScreenSpaceRectangle = vtable index 105 (+0x1A4), verified against
// CGlowObjectManager::ApplyEntityGlowEffects (client.dll+0x100E13B8):
//   (IMaterial*, destX, destY, width, height, srcX0, srcY0, srcX1, srcY1, srcTexW, srcTexH,
//    pClientRenderable, nXDice, nYDice)
// Note: DrawScreenSpaceRectangle does NOT write vertex colors into CMeshBuilder, so
// materials/vr/flashlight_glow.vmt must NOT set $vertexcolor or $vertexalpha.
// ---------------------------------------------------------------------------------------------
static void DrawFlashlightLensGlow(const CViewSetup& eyeView, ITexture* eyeTexture)
{
	VR* vr = Hooks::m_VR;
	Game* game = Hooks::m_Game;
	if (!vr || !game || !eyeTexture || s_LensFrame != s_RenderFrameId)
		return;
	if (!vr->m_FlashlightEquipped || !vr->m_FlashlightBeamOn)
		return;

	static PinnedMaterialSlot s_GlowMatSlot;
	IMaterial* s_GlowMat = s_GlowMatSlot.Get(game->m_MaterialSystem, "vr/flashlight_glow", "Other textures");
	if (!s_GlowMat)
		return;

	Vector lensDir = s_LensNormal;
	float nLen = VectorLength(lensDir);
	if (nLen > 0.0001f)
		lensDir = lensDir * (1.0f / nLen);
	else
	{
		QAngle beamAng = vr->m_LeftHanded ? vr->GetRightControllerAbsAngle() : vr->GetLeftControllerAbsAngle();
		Vector beamRight, beamUp;
		QAngle::AngleVectors(beamAng, &lensDir, &beamRight, &beamUp);
	}

	Vector toEye = eyeView.origin - s_LensPos;
	float dist = VectorLength(toEye);
	if (dist < 0.5f)
		return;
	float facing = Dot3(lensDir, toEye) / dist;          // 1.0 = looking straight down the flashlight barrel into the reflector
	const float facingStart = 0.15f;                     // ~81 deg off-axis: front glass lens becomes visible
	if (facing <= facingStart)
		return;

	// Occlusion by world geometry between eye and lens
	{
		CTraceFilterWorldOnlyUse filter;
		CGameTrace tr;
		Ray_t ray;
		ray.Init(eyeView.origin, s_LensPos);
		game->m_EngineTrace->TraceRay(ray, CONTENTS_SOLID, &filter, &tr);
		if (!tr.startsolid && (1.0f - tr.fraction) * dist > 1.0f)
			return;
	}

	// Project the lens center into this eye's view frustum using the same aspect ratio (W / H)
	// that CViewRender::DrawViewModels (client.dll+0x19897C) sets via engine->GetScreenAspectRatio(width, height).
	Vector vf, vr_, vu;
	QAngle::AngleVectors(QAngle(eyeView.angles.x, eyeView.angles.y, eyeView.angles.z), &vf, &vr_, &vu);
	Vector v = s_LensPos - eyeView.origin;
	float z = Dot3(v, vf);
	if (z < 1.0f)
		return;
	const float W = (float)vr->m_RenderWidth;
	const float H = (float)vr->m_RenderHeight;
	float tanX = tanf(eyeView.fov * 0.5f * (3.14159265f / 180.0f));
	float aspect = (H > 1.0f) ? (W / H) : 1.0f;
	float tanY = tanX / aspect;
	float sx = (0.5f + 0.5f * Dot3(v, vr_) / (z * tanX)) * W;
	float sy = (0.5f - 0.5f * Dot3(v, vu) / (z * tanY)) * H;

	IMatRenderContext* ctx = game->m_MaterialSystem->GetRenderContext();
	if (!ctx)
		return;
	ctx->SetRenderTarget(eyeTexture);
	ctx->Viewport(0, 0, (int)W, (int)H);
	typedef void (__thiscall* tDrawScreenSpaceRectangle)(void* thisptr, IMaterial* mat, int destX, int destY, int width, int height,
	                                                     float srcX0, float srcY0, float srcX1, float srcY1,
	                                                     int srcTexW, int srcTexH, void* pClientRenderable, int nXDice, int nYDice);
	auto fnDrawRect = (*(tDrawScreenSpaceRectangle**)ctx)[105];
	const int texSize = 256;

	auto drawGlowPass = [&](float pixRadius, int passes)
	{
		if (pixRadius < 2.0f || passes <= 0)
			return;
		int size = (int)(pixRadius * 2.0f);
		int x = (int)(sx - pixRadius);
		int y = (int)(sy - pixRadius);
		if (x + size < 0 || y + size < 0 || x > (int)W || y > (int)H)
			return;
		for (int p = 0; p < passes; ++p)
		{
			fnDrawRect(ctx, s_GlowMat, x, y, size, size,
			           0.0f, 0.0f, (float)(texSize - 1), (float)(texSize - 1),
			           texSize, texSize, nullptr, 1, 1);
		}
	};

	const float cmToUnits = 0.01f * vr->m_VRScale;
	const float worldToPix = (W * 0.5f) / (z * tanX);

	// 1. Inner lens / reflector bulb hotspot (always centered on the physical glass lens)
	float coreFactor = std::clamp((facing - facingStart) / (1.0f - facingStart), 0.0f, 1.0f);
	float coreWorldRad = (2.8f + 3.8f * coreFactor) * cmToUnits;
	float corePixRad = std::clamp(coreWorldRad * worldToPix, 4.0f, W * 0.22f);
	drawGlowPass(corePixRad, (facing > 0.60f) ? 2 : 1);

	// 2. Medium reflector corona when angled towards the eye (facing > 0.50, within ~60 deg of beam axis)
	if (facing > 0.50f)
	{
		float halo = std::clamp((facing - 0.50f) / 0.50f, 0.0f, 1.0f);
		float haloWorldRad = (8.0f + 26.0f * halo * halo) * cmToUnits;
		float haloPixRad = std::clamp(haloWorldRad * worldToPix, corePixRad * 1.5f, W * 0.42f);
		drawGlowPass(haloPixRad, (facing > 0.82f) ? 2 : 1);
	}

	// 3. Blinding eye-dazzle glare when looking directly into the beam (facing > 0.74, within ~42 deg of axis)
	if (facing > 0.74f)
	{
		float blind = std::clamp((facing - 0.74f) / 0.26f, 0.0f, 1.0f);
		float blindSmooth = blind * blind * (3.0f - 2.0f * blind);
		float dazzlePixRad = (0.14f + 0.46f * blindSmooth) * W;
		int dazzlePasses = 1 + (int)(blindSmooth * 2.5f); // up to 3 additive full-glare passes dead-on
		drawGlowPass(dazzlePixRad, dazzlePasses);
	}

	ctx->Release();
}

struct VRScreenFadeState
{
	float r = 0.0f;
	float g = 0.0f;
	float b = 0.0f;
	float a = 0.0f;
	uint8_t blend = 0;
	bool active = false;
};

// Reads the live env_fade color/alpha written every frame by CViewRender::PerformScreenOverlay
// (client.dll+0x18EE40 -> SetPostProcessFadeParams @ +0x186800), which DoEnginePostProcessing
// skips when mat_postprocess_enable = 0 in VR.
static VRScreenFadeState ReadVRScreenFadeState(Game* game)
{
	VRScreenFadeState out{};
	if (!game || !game->m_BaseClient)
		return out;

	// Validate opcode sites inside SetPostProcessFadeParams (+0x186800) and read the loader-rebased
	// global addresses of g_FadeColor[0] (+0x6CD210) and g_FadeBlend[0] (+0x6CC578).
	const uint8_t* pColorOp = (const uint8_t*)(game->m_BaseClient + 0x186833);
	const uint8_t* pBlendOp = (const uint8_t*)(game->m_BaseClient + 0x186878);
	if (pColorOp[0] != 0x81 || pColorOp[1] != 0xC1 || pBlendOp[0] != 0x88 || pBlendOp[1] != 0x88)
		return out;

	const float* fadeColor = *(const float* const*)(game->m_BaseClient + 0x186835);
	const uint8_t* fadeBlend = *(const uint8_t* const*)(game->m_BaseClient + 0x18687A);
	if (!fadeColor || !fadeBlend)
		return out;

	const float r = fadeColor[0];
	const float g = fadeColor[1];
	const float b = fadeColor[2];
	const float a = fadeColor[3];
	if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b) || !std::isfinite(a))
		return out;

	out.r = std::clamp(r, 0.0f, 1.0f);
	out.g = std::clamp(g, 0.0f, 1.0f);
	out.b = std::clamp(b, 0.0f, 1.0f);
	out.a = std::clamp(a, 0.0f, 1.0f);
	out.blend = *fadeBlend;
	out.active = (out.a > 0.002f);
	return out;
}

static void DrawVRScreenFade(ITexture* eyeTexture, const VRScreenFadeState& fade)
{
	VR* vr = Hooks::m_VR;
	Game* game = Hooks::m_Game;
	if (!fade.active || !vr || !game || !game->m_MaterialSystem || !eyeTexture)
		return;

	const int W = (int)vr->m_RenderWidth;
	const int H = (int)vr->m_RenderHeight;
	if (W <= 0 || H <= 0)
		return;

	IMatRenderContext* ctx = game->m_MaterialSystem->GetRenderContext();
	if (!ctx)
		return;

	ctx->SetRenderTarget(eyeTexture);
	ctx->Viewport(0, 0, W, H);

	const bool isOpaqueSolid = (fade.a >= 0.996f) &&
		(fade.blend == 0 || (fade.r <= 0.01f && fade.g <= 0.01f && fade.b <= 0.01f));
	if (isOpaqueSolid)
	{
		const unsigned char cr = (unsigned char)std::clamp((int)std::lround(fade.r * 255.0f), 0, 255);
		const unsigned char cg = (unsigned char)std::clamp((int)std::lround(fade.g * 255.0f), 0, 255);
		const unsigned char cb = (unsigned char)std::clamp((int)std::lround(fade.b * 255.0f), 0, 255);
		ctx->ClearColor4ub(cr, cg, cb, 255);
		ctx->ClearBuffers(true, false, false);
		ctx->Release();
		return;
	}

	static PinnedMaterialSlot s_FadeMatSlot;
	IMaterial* fadeMat = s_FadeMatSlot.Get(game->m_MaterialSystem, "vr/screen_fade", "Other textures");
	if (!fadeMat)
	{
		ctx->Release();
		return;
	}

	static IMaterial* s_LastFadeMat = nullptr;
	static float s_LastFadeR = -1.0f, s_LastFadeG = -1.0f, s_LastFadeB = -1.0f, s_LastFadeA = -1.0f;
	if (fadeMat != s_LastFadeMat ||
	    fade.r != s_LastFadeR || fade.g != s_LastFadeG || fade.b != s_LastFadeB || fade.a != s_LastFadeA)
	{
		fadeMat->ColorModulate(fade.r, fade.g, fade.b);
		fadeMat->AlphaModulate(fade.a);
		s_LastFadeMat = fadeMat;
		s_LastFadeR = fade.r;
		s_LastFadeG = fade.g;
		s_LastFadeB = fade.b;
		s_LastFadeA = fade.a;
	}

	typedef void(__thiscall* tDrawScreenSpaceRectangle)(void* thisptr, IMaterial* mat, int destX, int destY, int width, int height,
	                                                    float srcX0, float srcY0, float srcX1, float srcY1,
	                                                    int srcTexW, int srcTexH, void* pClientRenderable, int nXDice, int nYDice);
	auto fnDrawRect = (*(tDrawScreenSpaceRectangle**)ctx)[105];
	const int tw = std::max(fadeMat->GetMappingWidth(), 1);
	const int th = std::max(fadeMat->GetMappingHeight(), 1);
	fnDrawRect(ctx, fadeMat, 0, 0, W, H,
	           0.0f, 0.0f, (float)(tw - 1), (float)(th - 1),
	           tw, th, nullptr, 1, 1);
	ctx->Release();
}

static VRScreenFadeState CombineVRScreenFades(const VRScreenFadeState& envFade, float headClipFade)
{
	const float headAlpha = std::clamp(headClipFade, 0.0f, 1.0f);
	if (headAlpha <= 0.002f)
		return envFade;

	VRScreenFadeState out{};
	if (!envFade.active || envFade.a <= 0.002f)
	{
		out.r = 0.0f;
		out.g = 0.0f;
		out.b = 0.0f;
		out.a = headAlpha;
		out.blend = 0;
		out.active = true;
		return out;
	}

	// Composite black head-clip fade OVER existing env_fade using Porter-Duff SRC_OVER.
	const float envAlpha = std::clamp(envFade.a, 0.0f, 1.0f);
	const float outAlpha = std::clamp(envAlpha + headAlpha - (envAlpha * headAlpha), 0.0f, 1.0f);
	if (outAlpha > 1e-5f)
	{
		const float scale = (envAlpha * (1.0f - headAlpha)) / outAlpha;
		out.r = std::clamp(envFade.r * scale, 0.0f, 1.0f);
		out.g = std::clamp(envFade.g * scale, 0.0f, 1.0f);
		out.b = std::clamp(envFade.b * scale, 0.0f, 1.0f);
	}
	else
	{
		out.r = 0.0f;
		out.g = 0.0f;
		out.b = 0.0f;
	}
	out.a = outAlpha;
	out.blend = (headAlpha >= 0.996f) ? 0 : envFade.blend;
	out.active = (out.a > 0.002f);
	return out;
}

static VRScreenFadeState CompositeWhiteFadeOver(const VRScreenFadeState& baseFade, float whiteAlpha)
{
	const float a = std::clamp(whiteAlpha, 0.0f, 1.0f);
	if (a <= 0.002f)
		return baseFade;

	VRScreenFadeState out{};
	if (!baseFade.active || baseFade.a <= 0.002f)
	{
		out.r = 1.0f;
		out.g = 0.98f;
		out.b = 0.94f;
		out.a = a;
		out.blend = 0;
		out.active = true;
		return out;
	}

	const float baseA = std::clamp(baseFade.a, 0.0f, 1.0f);
	const float outA = std::clamp(baseA + a - (baseA * a), 0.0f, 1.0f);
	if (outA > 1e-5f)
	{
		const float baseScale = baseA * (1.0f - a);
		out.r = std::clamp((baseFade.r * baseScale + 1.0f * a) / outA, 0.0f, 1.0f);
		out.g = std::clamp((baseFade.g * baseScale + 0.98f * a) / outA, 0.0f, 1.0f);
		out.b = std::clamp((baseFade.b * baseScale + 0.94f * a) / outA, 0.0f, 1.0f);
	}
	else
	{
		out.r = 1.0f;
		out.g = 0.98f;
		out.b = 0.94f;
	}
	out.a = outA;
	out.blend = baseFade.blend;
	out.active = (out.a > 0.002f);
	return out;
}

static float ComputeHeadClipFadeRamp(float penetrationUnits, float rampUnits)
{
	if (penetrationUnits <= 0.0f || rampUnits <= 0.0f)
		return 0.0f;
	const float t = std::clamp(penetrationUnits / rampUnits, 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

static float AdvanceHeadClipFade(float currentFade, float targetFade, float dtSec)
{
	const float cur = std::clamp(currentFade, 0.0f, 1.0f);
	const float tgt = std::clamp(targetFade, 0.0f, 1.0f);
	const float dt = std::clamp(dtSec, 0.001f, 0.05f);
	constexpr float kFadeInRate = 8.5f;  // ~118 ms to full black (HL:Alyx feel)
	constexpr float kFadeOutRate = 5.0f; // ~200 ms back to clear
	if (tgt > cur)
		return std::min(tgt, cur + kFadeInRate * dt);
	if (tgt < cur)
		return std::max(tgt, cur - kFadeOutRate * dt);
	return cur;
}

class CTraceFilterHeadClip : public CTraceFilter
{
public:
	CTraceFilterHeadClip(void* localPlayer, void* attachedObj, void* heldObj, void* vehicleEnt)
		: CTraceFilter((IHandleEntity*)localPlayer, 0)
		, m_pLocalPlayer(localPlayer)
		, m_pAttachedObj(attachedObj)
		, m_pHeldObj(heldObj)
		, m_pVehicleEnt(vehicleEnt)
	{}

	virtual bool ShouldHitEntity(IHandleEntity* pServerEntity, int contentsMask) override
	{
		if (!pServerEntity)
			return false;
		if (IsIgnoredPtr(pServerEntity))
			return false;

		IClientUnknown* pUnk = (IClientUnknown*)pServerEntity;
		void* pBaseVoid = pUnk->GetBaseEntity();
		if (!pBaseVoid)
			return true; // Static prop or non-C_BaseEntity solid: treat as world geometry.
		if (IsIgnoredPtr(pBaseVoid))
			return false;

		C_BasePlayer* pEntity = (C_BasePlayer*)pBaseVoid;
		if (pEntity->IsNPC() || pEntity->IsPlayer())
			return false;

		if (void* pNet = pUnk->GetClientNetworkable())
		{
			typedef void* (__thiscall* tGetClientClass)(void*);
			auto fnGetClientClass = (*(tGetClientClass**)pNet)[2];
			if (fnGetClientClass)
			{
				if (void* pClientClass = fnGetClientClass(pNet))
				{
					const char* netName = *(const char* const*)((const char*)pClientClass + 0x08);
					if (ShouldSkipNetworkClass(netName))
						return false;
				}
			}
		}
		return true;
	}

	virtual TraceType GetTraceType() const override
	{
		return TraceType::TRACE_EVERYTHING;
	}

private:
	bool IsIgnoredPtr(const void* ptr) const
	{
		return ptr == m_pLocalPlayer ||
		       (m_pAttachedObj && ptr == m_pAttachedObj) ||
		       (m_pHeldObj && ptr == m_pHeldObj) ||
		       (m_pVehicleEnt && ptr == m_pVehicleEnt);
	}

	static bool ContainsAsciiCI(const char* haystack, const char* needle)
	{
		if (!haystack || !needle || !*needle)
			return false;
		for (const char* h = haystack; *h; ++h)
		{
			const char* a = h;
			const char* b = needle;
			while (*a && *b)
			{
				unsigned char ca = (unsigned char)*a;
				unsigned char cb = (unsigned char)*b;
				if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
				if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
				if (ca != cb)
					break;
				++a;
				++b;
			}
			if (*b == '\0')
				return true;
		}
		return false;
	}

	static bool ShouldSkipNetworkClass(const char* netName)
	{
		if (!netName)
			return false;
		static const char* const kSkipTokens[] = {
			"ViewModel",
			"Weapon",
			"Tool",
			"Ragdoll",
			"Collectible",
			"Pickup",
			"Flashlight",
			"Trigger",
		};
		for (const char* tok : kSkipTokens)
		{
			if (ContainsAsciiCI(netName, tok))
				return true;
		}
		return false;
	}

	void* m_pLocalPlayer;
	void* m_pAttachedObj;
	void* m_pHeldObj;
	void* m_pVehicleEnt;
};

static float ComputeHeadClipTargetFade(
	Game* game,
	VR* vr,
	C_BasePlayer* localPlayer,
	const Vector& bodyEyePos,
	const Vector& hmdCenter)
{
	if (!game || !vr || !localPlayer || !game->m_EngineTrace)
		return 0.0f;

	// Check 1: Out-of-map solid BSP void (covers leaning or stepping outside map bounds).
	const Vector leftEye = vr->GetViewOriginLeft(bodyEyePos);
	const Vector rightEye = vr->GetViewOriginRight(bodyEyePos);
	if ((game->m_EngineTrace->GetPointContents_WorldOnly(hmdCenter, CONTENTS_SOLID) & CONTENTS_SOLID) != 0 ||
	    (game->m_EngineTrace->GetPointContents_WorldOnly(leftEye, CONTENTS_SOLID) & CONTENTS_SOLID) != 0 ||
	    (game->m_EngineTrace->GetPointContents_WorldOnly(rightEye, CONTENTS_SOLID) & CONTENTS_SOLID) != 0)
	{
		return 1.0f;
	}

	auto resolvePlayerHandle = [&](uintptr_t offset) -> void*
	{
		if (!game->m_ClientEntityList)
			return nullptr;
		const uint32_t handle = *(const uint32_t*)((const char*)localPlayer + offset);
		if (handle == 0 || handle == 0xFFFFFFFFu)
			return nullptr;
		return game->m_ClientEntityList->GetClientEntityFromHandle((int)handle);
	};

	void* attachedObj = resolvePlayerHandle(0x2160); // m_hAttachedObject
	void* heldObj = resolvePlayerHandle(0x2514);     // m_hHeldObject
	void* vehicleEnt = resolvePlayerHandle(0x1708);  // m_hVehicle (e.g. intro chair)

	CTraceFilterHeadClip filter(localPlayer, attachedObj, heldObj, vehicleEnt);
	constexpr unsigned int kHeadClipMask = CONTENTS_SOLID | CONTENTS_MOVEABLE | CONTENTS_WINDOW;
	constexpr float kProbeHalf = 2.6f;
	constexpr float kFadeRamp = 2.6f;

	float targetFade = 0.0f;

	// Check 2: Sweep from safe player hull eye to roomscale HMD center against walls and solid objects.
	const float segLen = VectorLength(hmdCenter - bodyEyePos);
	if (segLen > 0.5f)
	{
		CGameTrace boxTrace;
		Ray_t boxRay;
		boxRay.Init(bodyEyePos, hmdCenter,
		            Vector(-kProbeHalf, -kProbeHalf, -kProbeHalf),
		            Vector(kProbeHalf, kProbeHalf, kProbeHalf));
		game->m_EngineTrace->TraceRay(boxRay, kHeadClipMask, &filter, &boxTrace);

		if (!boxTrace.startsolid)
		{
			if (boxTrace.fraction < 1.0f)
			{
				const float pen = (1.0f - boxTrace.fraction) * segLen;
				targetFade = std::max(targetFade, ComputeHeadClipFadeRamp(pen, kFadeRamp));
			}
		}
		else
		{
			CGameTrace lineTrace;
			Ray_t lineRay;
			lineRay.Init(bodyEyePos, hmdCenter);
			game->m_EngineTrace->TraceRay(lineRay, kHeadClipMask, &filter, &lineTrace);
			if (lineTrace.startsolid || lineTrace.fraction < 1.0f)
				targetFade = 1.0f;
		}
	}

	// Check 3: Direct inner solid containment check around HMD center.
	{
		CGameTrace centerTrace;
		Ray_t centerRay;
		centerRay.Init(hmdCenter, hmdCenter,
		               Vector(-1.4f, -1.4f, -1.4f),
		               Vector(1.4f, 1.4f, 1.4f));
		game->m_EngineTrace->TraceRay(centerRay, kHeadClipMask, &filter, &centerTrace);
		if (centerTrace.startsolid || centerTrace.fraction < 1.0f)
			targetFade = 1.0f;
	}

	// Check 4: Per-eye lateral probes from HMD center so tilting into edges/frames fades smoothly.
	const Vector eyePositions[2] = { leftEye, rightEye };
	for (const Vector& eyePos : eyePositions)
	{
		const float eyeSegLen = VectorLength(eyePos - hmdCenter);
		if (eyeSegLen > 0.25f)
		{
			CGameTrace eyeTrace;
			Ray_t eyeRay;
			eyeRay.Init(hmdCenter, eyePos,
			            Vector(-1.2f, -1.2f, -1.2f),
			            Vector(1.2f, 1.2f, 1.2f));
			game->m_EngineTrace->TraceRay(eyeRay, kHeadClipMask, &filter, &eyeTrace);
			if (!eyeTrace.startsolid)
			{
				if (eyeTrace.fraction < 1.0f)
				{
					const float pen = (1.0f - eyeTrace.fraction) * eyeSegLen;
					targetFade = std::max(targetFade, ComputeHeadClipFadeRamp(pen, 1.2f));
				}
			}
			else
			{
				CGameTrace eyeLineTrace;
				Ray_t eyeLineRay;
				eyeLineRay.Init(hmdCenter, eyePos);
				game->m_EngineTrace->TraceRay(eyeLineRay, kHeadClipMask, &filter, &eyeLineTrace);
				if (eyeLineTrace.startsolid || eyeLineTrace.fraction < 1.0f)
					targetFade = 1.0f;
			}
		}
	}

	return std::clamp(targetFade, 0.0f, 1.0f);
}

Hooks::Hooks(Game *game)
{
	if (MH_Initialize() != MH_OK)
	{
		Game::errorMsg("Failed to init MinHook");
	}

	m_Game = game;
	m_VR = m_Game->m_VR;

	m_PushHUDStep = -999;
	m_PushedHud = true;

	initSourceHooks();

	try
	{
		hkCalcViewModelView.enableHook();

		hkProcessUsercmds.enableHook();
		hkReadUsercmd.enableHook();

		hkWriteUsercmd.enableHook();

		hkCreateMove.enableHook();
		hkEyePosition.enableHook();
		hkRenderView.enableHook();
		if (hkDrawViewModels.pTarget)
			hkDrawViewModels.enableHook();

		hkWeapon_ShootPosition.enableHook();

		// Do NOT enable hkDrawSelf: Offsets::DrawSelf hooks BitmapImage::DoPaint (client.dll+0xE6290),
		// which is used by all 3D C_VGuiScreen panels (main menu laptop, options binder, chapters projector,
		// and in-game monitors). Shifting coordinates by AimPos pushes all 3D VGUI images off-screen.
		// hkDrawSelf.enableHook();

		hkUpdateObject.enableHook();
		hkUpdateObjectVM.enableHook();
		hkEyeAngles.enableHook();

		hkGetDefaultFOV.enableHook();
		hkGetFOV.enableHook();
		hkGetViewModelFOV.enableHook();

		hkCHudCrosshair_ShouldDraw.enableHook();

		// INFRA-specific VR Mechanics (Flashlight, Camera, +use Hand Interaction, Pause Menu)
		if (hkUpdateFlashlight.pTarget)
			hkUpdateFlashlight.enableHook();
		if (hkClientEyeAngles.pTarget)
			hkClientEyeAngles.enableHook();
		if (hkClientEyePosition.pTarget)
			hkClientEyePosition.enableHook();
		if (hkClientCameraThink.pTarget)
			hkClientCameraThink.enableHook();
		if (hkShouldUsePhonePause.pTarget)
			hkShouldUsePhonePause.enableHook();
		if (hkClientFindUseEntity.pTarget)
			hkClientFindUseEntity.enableHook();
		if (hkFindUseEntity.pTarget)
			hkFindUseEntity.enableHook();
		if (hkCameraPrimaryAttack.pTarget)
			hkCameraPrimaryAttack.enableHook();
		if (hkCameraSecondaryAttack.pTarget)
			hkCameraSecondaryAttack.enableHook();
		if (hkCameraItemPostFrame.pTarget)
			hkCameraItemPostFrame.enableHook();
		if (hkCameraShouldFlash.pTarget)
			hkCameraShouldFlash.enableHook();
		if (hkCameraOnPhotoTaken.pTarget)
			hkCameraOnPhotoTaken.enableHook();
		if (hkCameraFlashlightUpdateLight.pTarget)
			hkCameraFlashlightUpdateLight.enableHook();
		if (hkCameraScreenOnTick.pTarget)
			hkCameraScreenOnTick.enableHook();
		if (hkHandsItemPostFrame.pTarget)
			hkHandsItemPostFrame.enableHook();
		if (hkGetCarryDistance.pTarget)
			hkGetCarryDistance.enableHook();
		if (hkGetCarryVerticalOffset.pTarget)
			hkGetCarryVerticalOffset.enableHook();
		if (hkFlashlightHideThink.pTarget)
			hkFlashlightHideThink.enableHook();
		if (hkViewModelShouldDraw.pTarget)
			hkViewModelShouldDraw.enableHook();
		if (hkReloadFlashlight.pTarget)
			hkReloadFlashlight.enableHook();
		if (hkGetCursorPosition.pTarget)
			hkGetCursorPosition.enableHook();
		if (hkVGuiScreenGetCursorPos.pTarget)
			hkVGuiScreenGetCursorPos.enableHook();
		if (hkFindVGuiScreenUnderCursor.pTarget)
			hkFindVGuiScreenUnderCursor.enableHook();
		if (hkGrabControllerSetTargetPosition.pTarget)
			hkGrabControllerSetTargetPosition.enableHook();
		if (hkDrawModelExecute.pTarget)
			hkDrawModelExecute.enableHook();

		VRAudio::Init(m_Game);
	}
	catch (const std::exception &e)
	{
		char msg[512];
		sprintf_s(msg, sizeof(msg), "Exception enabling VR hooks: %s", e.what());
		Game::errorMsg(msg);
	}
	catch (...)
	{
		Game::errorMsg("Unknown exception enabling VR hooks");
	}
}

Hooks::~Hooks()
{
	VRAudio::Shutdown();
	VRViewModelDepth::Shutdown();
	VRInventory::Reset();
	VRCameraFlash::Reset();
	ResetCameraScreenCache();
	VRHands::ResetFlashlightGripAnim();
	VRGrab::ResetOpenHandPhysics();
	VRHands::ResetVisualHandPhysics();
	if (MH_Uninitialize() != MH_OK)
	{
		Game::errorMsg("Failed to uninitialize MinHook");
	}
}


int Hooks::initSourceHooks()
{
	LPVOID pRenderViewVFunc = (LPVOID)(m_Game->m_Offsets->RenderView.address);
	hkRenderView.createHook(pRenderViewVFunc, &dRenderView);

	LPVOID calcViewModelViewAddr = (LPVOID)(m_Game->m_Offsets->CalcViewModelView.address);
	hkCalcViewModelView.createHook(calcViewModelViewAddr, &dCalcViewModelView);

	LPVOID ProcessUsercmdsAddr = (LPVOID)(m_Game->m_Offsets->ProcessUsercmds.address);
	hkProcessUsercmds.createHook(ProcessUsercmdsAddr, &dProcessUsercmds);

	LPVOID ReadUserCmdAddr = (LPVOID)(m_Game->m_Offsets->ReadUserCmd.address);
	hkReadUsercmd.createHook(ReadUserCmdAddr, &dReadUsercmd);

	LPVOID WriteUsercmdAddr = (LPVOID)(m_Game->m_Offsets->WriteUsercmd.address);
	hkWriteUsercmd.createHook(WriteUsercmdAddr, &dWriteUsercmd);

	LPVOID EyePositionAddr = (LPVOID)(m_Game->m_Offsets->EyePosition.address);
	hkEyePosition.createHook(EyePositionAddr, &dEyePosition);

	// Do not create unused Portal 2 / L4D2 hooks (PushRenderTargetAndViewport, PopRenderTargetAndViewport,
	// VGui_Paint, PrePushRenderTarget, DrawSelf, ClipTransform, ComputeError, RotateObject).

	LPVOID Weapon_ShootPositionAddr = (LPVOID)(m_Game->m_Offsets->Weapon_ShootPosition.address);
	hkWeapon_ShootPosition.createHook(Weapon_ShootPositionAddr, &dWeapon_ShootPosition);

	LPVOID CreateMoveAddr = (LPVOID)(m_Game->m_Offsets->CreateMove.address);
	hkCreateMove.createHook(CreateMoveAddr, &dCreateMove);

	// Grabbables
	hkUpdateObject.createHook((LPVOID)(m_Game->m_Offsets->UpdateObject.address), &dUpdateObject);
	hkUpdateObjectVM.createHook((LPVOID)(m_Game->m_Offsets->UpdateObjectVM.address), &dUpdateObjectVM);
	hkEyeAngles.createHook((LPVOID)(m_Game->m_Offsets->EyeAngles.address), &dEyeAngles);

	// FOV
	hkGetDefaultFOV.createHook((LPVOID)(m_Game->m_Offsets->GetDefaultFOV.address), &dGetDefaultFOV);
	hkGetFOV.createHook((LPVOID)(m_Game->m_Offsets->GetFOV.address), &dGetFOV);
	hkGetViewModelFOV.createHook((LPVOID)(m_Game->m_Offsets->GetViewModelFOV.address), &dGetViewModelFOV);
	
	// Crosshair
	hkCHudCrosshair_ShouldDraw.createHook((LPVOID)m_Game->m_Offsets->CHudCrosshair_ShouldDraw.address, &dCHudCrosshair_ShouldDraw);

	// INFRA-specific VR Mechanics
	hkUpdateFlashlight.createHook((LPVOID)(m_Game->m_Offsets->UpdateFlashlight.address), &dUpdateFlashlight);
	hkClientEyeAngles.createHook((LPVOID)(m_Game->m_Offsets->ClientEyeAngles.address), &dClientEyeAngles);
	hkClientEyePosition.createHook((LPVOID)(m_Game->m_Offsets->ClientEyePosition.address), &dClientEyePosition);
	hkClientCameraThink.createHook((LPVOID)(m_Game->m_Offsets->ClientCameraThink.address), &dClientCameraThink);
	if (m_Game->m_Offsets->ShouldUsePhonePause.address)
		hkShouldUsePhonePause.createHook((LPVOID)(m_Game->m_Offsets->ShouldUsePhonePause.address), &dShouldUsePhonePause);
	if (m_Game->m_Offsets->ClientFindUseEntity.address)
		hkClientFindUseEntity.createHook((LPVOID)(m_Game->m_Offsets->ClientFindUseEntity.address), &dClientFindUseEntity);
	hkFindUseEntity.createHook((LPVOID)(m_Game->m_Offsets->FindUseEntity.address), &dFindUseEntity);
	hkCameraPrimaryAttack.createHook((LPVOID)(m_Game->m_Offsets->CameraPrimaryAttack.address), &dCameraPrimaryAttack);
	if (m_Game->m_Offsets->CameraSecondaryAttack.address)
		hkCameraSecondaryAttack.createHook((LPVOID)(m_Game->m_Offsets->CameraSecondaryAttack.address), &dCameraSecondaryAttack);
	hkCameraItemPostFrame.createHook((LPVOID)(m_Game->m_Offsets->CameraItemPostFrame.address), &dCameraItemPostFrame);
	if (m_Game->m_Offsets->CameraShouldFlash.address)
		hkCameraShouldFlash.createHook((LPVOID)(m_Game->m_Offsets->CameraShouldFlash.address), &dCameraShouldFlash);
	if (m_Game->m_Offsets->CameraOnPhotoTaken.address)
		hkCameraOnPhotoTaken.createHook((LPVOID)(m_Game->m_Offsets->CameraOnPhotoTaken.address), &dCameraOnPhotoTaken);
	if (m_Game->m_Offsets->CameraFlashlightUpdateLight.address)
		hkCameraFlashlightUpdateLight.createHook((LPVOID)(m_Game->m_Offsets->CameraFlashlightUpdateLight.address), &dCameraFlashlightUpdateLight);
	if (m_Game->m_BaseClient)
	{
		const uint8_t* pDrawVM = reinterpret_cast<const uint8_t*>(m_Game->m_BaseClient + 0x198820);
		if (pDrawVM[0] == 0x55 && pDrawVM[1] == 0x8B && pDrawVM[2] == 0xEC &&
		    pDrawVM[3] == 0x81 && pDrawVM[4] == 0xEC && pDrawVM[5] == 0x74 &&
		    pDrawVM[6] == 0x05 && pDrawVM[7] == 0x00)
		{
			hkDrawViewModels.createHook((LPVOID)pDrawVM, &dDrawViewModels);
		}
		const uint8_t* pOnTick = reinterpret_cast<const uint8_t*>(m_Game->m_BaseClient + 0x1DAA60);
		if (pOnTick[0] == 0x55 && pOnTick[1] == 0x8B && pOnTick[2] == 0xEC &&
		    pOnTick[3] == 0x51 && pOnTick[4] == 0x53 && pOnTick[5] == 0x56 &&
		    pOnTick[6] == 0x8B && pOnTick[7] == 0xF1)
		{
			hkCameraScreenOnTick.createHook((LPVOID)pOnTick, &dCameraScreenOnTick);
		}
	}
	if (m_Game->m_BaseServer)
	{
		// CINFRAHands::ItemPostFrame (server.dll + 0x2964B0) calls SetWeaponVisible(false) during SV_Physics (after dProcessUsercmds!),
		// which sets EF_NODRAW on CINFRAHands & vm0 whenever NOT holding a physics object.
		hkHandsItemPostFrame.createHook((LPVOID)(m_Game->m_BaseServer + 0x2964B0), &dHandsItemPostFrame);
		// CPlayerPickupController::GetCarryDistance (+0x3A8420) & GetCarryVerticalOffset (+0x3A82B0)
		hkGetCarryDistance.createHook((LPVOID)(m_Game->m_BaseServer + 0x3A8420), &dGetCarryDistance);
		hkGetCarryVerticalOffset.createHook((LPVOID)(m_Game->m_BaseServer + 0x3A82B0), &dGetCarryVerticalOffset);
		// CGrabController::SetTargetPosition (+0x3A7610)
		hkGrabControllerSetTargetPosition.createHook((LPVOID)(m_Game->m_BaseServer + 0x3A7610), &dGrabControllerSetTargetPosition);

		// In CGrabController::UpdateObject (server.dll+0x3A9820):
		// 0x3A9F80 loads m_flObjectRadius and 0x3AA00D adds playerRadius2D (22.63 units = ~43cm) into [ebp-30h] (minDist),
		// which pushes held props ~55cm away from the hand and radially out from the player's body center!
		// Zeroing [ebp-30h] (fldz at 0x3A9F80, nop at 0x3AA00D) and disabling column/min-distance ConVars holds props directly in the VR hand.
		DWORD oldProt = 0;
		uint8_t* pMinDistLoad = (uint8_t*)(m_Game->m_BaseServer + 0x3A9F80);
		if (VirtualProtect(pMinDistLoad, 0x95, PAGE_EXECUTE_READWRITE, &oldProt))
		{
			// 103A9F80: D9 80 0C 01 00 00 (fld dword ptr [eax+10Ch]) -> D9 EE 90 90 90 90 (fldz; nop*4)
			if (pMinDistLoad[0] == 0xD9 && pMinDistLoad[1] == 0x80)
			{
				pMinDistLoad[0] = 0xD9; pMinDistLoad[1] = 0xEE;
				pMinDistLoad[2] = 0x90; pMinDistLoad[3] = 0x90; pMinDistLoad[4] = 0x90; pMinDistLoad[5] = 0x90;
			}
			// 103AA00D: D8 45 A0 (fadd dword ptr [ebp-60h]) -> 90 90 90 (nop*3)
			uint8_t* pFaddPlayerRad = (uint8_t*)(m_Game->m_BaseServer + 0x3AA00D);
			if (pFaddPlayerRad[0] == 0xD8 && pFaddPlayerRad[1] == 0x45 && pFaddPlayerRad[2] == 0xA0)
			{
				pFaddPlayerRad[0] = 0x90; pFaddPlayerRad[1] = 0x90; pFaddPlayerRad[2] = 0x90;
			}
			VirtualProtect(pMinDistLoad, 0x95, oldProt, &oldProt);
			FlushInstructionCache(GetCurrentProcess(), pMinDistLoad, 0x95);
		}

		// Set server pickup ConVars directly:
		// player_held_object_look_down_adjustment (0x7F8100): 0.0f
		*(float*)(m_Game->m_BaseServer + 0x7F812C) = 0.0f;
		*(int*)(m_Game->m_BaseServer + 0x7F8130)   = 0;
		// player_held_object_min_distance (0x7F82B8): 2.0f (instead of 50.0f units = ~95cm)
		*(float*)(m_Game->m_BaseServer + 0x7F82E4) = 2.0f;
		*(int*)(m_Game->m_BaseServer + 0x7F82E8)   = 2;
		// player_hold_object_in_column (0x7F8310): 0 (disable cylinder projection)
		*(float*)(m_Game->m_BaseServer + 0x7F833C) = 0.0f;
		*(int*)(m_Game->m_BaseServer + 0x7F8340)   = 0;

		// In CBaseInfraWeapon::Reload (server.dll+0x2B71A0):
		// At +0x2B71AC: F6 87 A8 00 00 00 04 0F 84 88 00 00 00 (test byte ptr [edi+0xA8], 4; je 102B7240)
		// blocks reloading the DSLR camera whenever the player's flashlight is ON (EF_DIMLIGHT = 4) in the other hand!
		// Patching this check to always jump to 102B7240 allows reloading the camera while the flashlight is on.
		uint8_t* pReloadFlashlightGate = (uint8_t*)(m_Game->m_BaseServer + 0x2B71AC);
		if (VirtualProtect(pReloadFlashlightGate, 16, PAGE_EXECUTE_READWRITE, &oldProt))
		{
			if (pReloadFlashlightGate[0] == 0xF6 && pReloadFlashlightGate[1] == 0x87 &&
			    pReloadFlashlightGate[7] == 0x0F && pReloadFlashlightGate[8] == 0x84)
			{
				for (int k = 0; k < 8; ++k)
					pReloadFlashlightGate[k] = 0x90;
				pReloadFlashlightGate[8] = 0xE9; // jmp rel32 +0x88 (to 102B7240)
			}
			VirtualProtect(pReloadFlashlightGate, 16, oldProt, &oldProt);
			FlushInstructionCache(GetCurrentProcess(), pReloadFlashlightGate, 16);
		}
	}
	if (m_Game->m_BaseClient)
	{
		// Pre-unprotect client.dll 2-byte sites toggled per-eye in dRenderView:
		// - +0x198872: CopyRenderTargetToTextureEx(_rt_CameraWeapon) in CViewRender::DrawViewModels
		// - +0x198F41: C_VGuiScreen offscreen render loop in CViewRender::RenderView
		DWORD oldProt = 0;
		VirtualProtect((LPVOID)(m_Game->m_BaseClient + 0x198872), 4, PAGE_EXECUTE_READWRITE, &oldProt);
		VirtualProtect((LPVOID)(m_Game->m_BaseClient + 0x198F41), 4, PAGE_EXECUTE_READWRITE, &oldProt);
		VirtualProtect((LPVOID)(m_Game->m_BaseClient + 0x8FE18), 4, PAGE_EXECUTE_READWRITE, &oldProt);
		VirtualProtect((LPVOID)(m_Game->m_BaseClient + 0x8FF48), 4, PAGE_EXECUTE_READWRITE, &oldProt);
	}
	// C_BaseViewModel::ShouldDraw (delegates to active weapon; CINFRAHands returns false -> right VR hand hidden)
	hkViewModelShouldDraw.createHook((LPVOID)(m_Game->m_BaseClient + 0x59180), &dViewModelShouldDraw);
	if (m_Game->m_Offsets->FlashlightHideThink.address)
		hkFlashlightHideThink.createHook((LPVOID)(m_Game->m_Offsets->FlashlightHideThink.address), &dFlashlightHideThink);
	if (m_Game->m_Offsets->ReloadFlashlight.address)
		hkReloadFlashlight.createHook((LPVOID)(m_Game->m_Offsets->ReloadFlashlight.address), &dReloadFlashlight);
	if (m_Game->m_Offsets->VGuiScreenGetCursorPos.address)
		hkVGuiScreenGetCursorPos.createHook((LPVOID)(m_Game->m_Offsets->VGuiScreenGetCursorPos.address), &dVGuiScreenGetCursorPos);
	if (m_Game->m_Offsets->FindVGuiScreenUnderCursor.address)
		hkFindVGuiScreenUnderCursor.createHook((LPVOID)(m_Game->m_Offsets->FindVGuiScreenUnderCursor.address), &dFindVGuiScreenUnderCursor);

	if (m_Game->m_ModelRender)
	{
		void** modelRenderVtable = *(void***)m_Game->m_ModelRender;
		if (modelRenderVtable && modelRenderVtable[19])
		{
			hkDrawModelExecute.createHook(modelRenderVtable[19], &dDrawModelExecute);
		}
	}

	void* pInputSystem = m_Game->GetInterface("inputsystem.dll", "InputSystemVersion001");
	if (pInputSystem)
	{
		void** vtable = *(void***)pInputSystem;
		if (vtable && vtable[45])
		{
			hkGetCursorPosition.createHook(vtable[45], &dGetCursorPosition);
		}
	}

	EntityIndex = (tEntindex)m_Game->m_Offsets->CBaseEntity_entindex.address;
	GetOwner = (tGetOwner)m_Game->m_Offsets->GetOwner.address;
	return 1;
} 

bool __fastcall Hooks::dCHudCrosshair_ShouldDraw(void* ecx, void* edx) {
	bool shouldDraw = hkCHudCrosshair_ShouldDraw.fOriginal(ecx);

	m_VR->m_DrawCrosshair = shouldDraw;

	return ((m_VR->m_AimMode == 1) ? shouldDraw : false);
}

const char* Hooks::SelectBareHandModel(int* outModelIndex)
{
	return VRHands::SelectBareHandModel(m_Game, outModelIndex);
}

void Hooks::EnsureServerBareHandViewModel(void* pWeapon, void* pVm0)
{
	if (!m_Game || !m_Game->m_BaseServer)
		return;

	uintptr_t baseSrv = m_Game->m_BaseServer;
	typedef void (__thiscall *tSetWeaponVisible)(void *thisptr, bool visible);
	typedef void (__thiscall *tRemoveEffects)(void *thisptr, int effects);
	typedef void (__thiscall *tResetSequenceInfo)(void *thisptr);

	auto fnSetWeaponVisible  = (tSetWeaponVisible)(baseSrv + 0xB0D20);
	auto fnRemoveEffects     = (tRemoveEffects)(baseSrv + 0x100E0);
	auto fnResetSequenceInfo = (tResetSequenceInfo)(baseSrv + 0x9D540);

	const char* targetBareHandMdl = SelectBareHandModel();

	if (pWeapon)
	{
		auto fnServerWeaponGetViewModel = *(char*(__thiscall**)(void*, int))(*(uintptr_t*)pWeapon + 0x4F4);
		if (fnServerWeaponGetViewModel)
		{
			char* szServerViewModel = fnServerWeaponGetViewModel(pWeapon, 0);
			if (szServerViewModel && _stricmp(szServerViewModel, targetBareHandMdl) != 0)
			{
				strcpy_s(szServerViewModel, 80, targetBareHandMdl);
			}
		}
		fnSetWeaponVisible(pWeapon, true);
	}

	if (pVm0)
	{
		if ((*(uint32_t*)((uintptr_t)pVm0 + 0xA8) & 0x20) != 0)
		{
			fnRemoveEffects(pVm0, 0x20);
		}
		const char* vm0Mdl = *(const char**)((uintptr_t)pVm0 + 0xAC);
		if (!vm0Mdl || _stricmp(vm0Mdl, targetBareHandMdl) != 0 || *(void**)((uintptr_t)pVm0 + 0x4A0) == nullptr)
		{
			auto fnSetWeaponModel = *(void(__thiscall**)(void*, const char*, void*))(*(uintptr_t*)pVm0 + 0x3A8);
			if (fnSetWeaponModel)
			{
				fnSetWeaponModel(pVm0, targetBareHandMdl, pWeapon);
				fnResetSequenceInfo(pVm0);
			}
		}
		if (*(int*)((uintptr_t)pVm0 + 0x3C0) != 0)
		{
			auto fnSendVmMatchingSeq = *(void(__thiscall**)(void*, int))(*(uintptr_t*)pVm0 + 0x3A4);
			if (fnSendVmMatchingSeq)
			{
				fnSendVmMatchingSeq(pVm0, 0);
			}
		}
	}
}

static bool ContainsCaseInsensitive(const char* haystack, const char* needle)
{
	if (!haystack || !needle || !needle[0])
		return false;
	const size_t nlen = std::strlen(needle);
	for (const char* p = haystack; *p != '\0'; ++p)
	{
		size_t i = 0;
		while (i < nlen && p[i] != '\0' &&
		       std::tolower((unsigned char)p[i]) == std::tolower((unsigned char)needle[i]))
		{
			++i;
		}
		if (i == nlen)
			return true;
	}
	return false;
}

static bool IsLocalPlayerInIntroChair(Game* game, C_BasePlayer* localPlayer)
{
	if (!game || !game->m_ClientEntityList || !game->m_ModelInfo || !localPlayer)
		return false;
	const uint32_t hVehicle = *(const uint32_t*)((uintptr_t)localPlayer + 0x1708);
	if (hVehicle == 0xFFFFFFFFu)
		return false;
	void* vehEnt = game->m_ClientEntityList->GetClientEntityFromHandle((int)hVehicle);
	if (!vehEnt)
		return false;
	const uintptr_t renderable = (uintptr_t)vehEnt + 0x4;
	const uintptr_t renderVtable = *(const uintptr_t*)renderable;
	if (!renderVtable)
		return false;
	auto fnGetModel = *(model_t*(__thiscall**)(void*))(renderVtable + 0x20);
	if (!fnGetModel)
		return false;
	model_t* mdl = fnGetModel((void*)renderable);
	const char* mdlName = mdl ? game->m_ModelInfo->GetModelName(mdl) : nullptr;
	return ContainsCaseInsensitive(mdlName, "office_chair_001");
}

static QAngle ConvertWorldAnglesForPlayerMoveParent(Game* game, C_BasePlayer* localPlayer, const QAngle& worldAngles)
{
	if (!game || !game->m_BaseClient || !game->m_ClientEntityList || !localPlayer)
		return worldAngles;
	const uint32_t hMoveParent = *(const uint32_t*)((uintptr_t)localPlayer + 0x3F8);
	if (hMoveParent == 0xFFFFFFFFu)
		return worldAngles;
	void* moveParent = game->m_ClientEntityList->GetClientEntityFromHandle((int)hMoveParent);
	if (!moveParent)
		return worldAngles;

	typedef void(__thiscall* tCalcAbsolutePosition)(void* ecx);
	auto fnCalcAbsPos = (tCalcAbsolutePosition)(game->m_BaseClient + 0x37B30);
	fnCalcAbsPos(moveParent);
	const matrix3x4_t& parentToWorld = *(const matrix3x4_t*)((uintptr_t)moveParent + 0x538);
	return TransformAnglesToLocalSpace(worldAngles, parentToWorld);
}

static AudioState_t BuildVRAudioState(Game* game, C_BasePlayer* localPlayer, const Vector& hmdOrigin, const Vector& hmdAngle)
{
	AudioState_t audioState{};
	audioState.m_Origin = hmdOrigin;
	audioState.m_Angles = QAngle(hmdAngle.x, hmdAngle.y, hmdAngle.z);
	audioState.m_bIsUnderwater = false;

	if (game && game->m_EngineTrace)
	{
		// MASK_WATER (0x4030), matching client.dll C_BasePlayer::IsEyeUnderWater
		constexpr int kMaskWater = 0x4030;
		audioState.m_bIsUnderwater = (game->m_EngineTrace->GetPointContents_WorldOnly(hmdOrigin, kMaskWater) & kMaskWater) != 0;
	}
	else if (localPlayer)
	{
		// Fallback to C_BaseEntity::m_nWaterLevel (+0x356) >= WL_Eyes (3)
		audioState.m_bIsUnderwater = (*(const uint8_t*)((uintptr_t)localPlayer + 0x356) >= 3);
	}
	return audioState;
}

static bool s_UpdateCameraScreenOnEye1 = false;

void __fastcall Hooks::dRenderView(void *ecx, void *edx, CViewSetup &setup, CViewSetup &hudViewSetup, int nClearFlags, int whatToDraw)
{
	if (!m_Game || !m_VR || !m_Game->m_Initialized)
	{
		hkRenderView.fOriginal(ecx, setup, hudViewSetup, nClearFlags, whatToDraw);
		return;
	}
	struct RenderViewScopeGuard
	{
		VR* vr;
		explicit RenderViewScopeGuard(VR* v) : vr(v) { vr->m_InRenderView.store(true, std::memory_order_release); }
		~RenderViewScopeGuard() { vr->m_InRenderView.store(false, std::memory_order_release); }
		RenderViewScopeGuard(const RenderViewScopeGuard&) = delete;
		RenderViewScopeGuard& operator=(const RenderViewScopeGuard&) = delete;
	} renderViewGuard(m_VR);
	const auto tRenderViewStart = VRProfiler::Clock::now();
	static VRProfiler::Clock::time_point s_LastRenderViewStart {};
	if (s_LastRenderViewStart.time_since_epoch().count() != 0)
		m_VR->m_Profiler.Add(VRProfiler::FrameInterval, VRProfiler::Ms(s_LastRenderViewStart, tRenderViewStart));
	s_LastRenderViewStart = tRenderViewStart;
	++s_RenderFrameId; // hand use selection is recomputed once per rendered frame
	if (m_VR)
		m_VR->m_LastRenderViewTickMs.store(GetTickCount64());

	if (!m_VR->m_CreatedVRTextures) {
		m_VR->CreateVRTextures();
	}

	m_VR->MainThreadUpdate();

	if (m_Game->m_BaseClient)
	{
		// Disable mat_dynamic_tonemapping (client.dll+0x6CC768) in VR so CHistogramSystem::Update (client.dll+0x187B00)
		// never cycles 64 occlusion query boxes across alternating Left/Right eye views, eliminating the ~1Hz HDR/Bloom brightness flicker!
		*(float*)(m_Game->m_BaseClient + 0x6CC794) = 0.0f;
		*(int*)(m_Game->m_BaseClient + 0x6CC798)   = 0;

		// Disable Bloom (mat_disable_bloom at client.dll+0x6CCA28, m_fValue +0x2C, m_nValue +0x30)
		*(float*)(m_Game->m_BaseClient + 0x6CCA54) = 1.0f;
		*(int*)(m_Game->m_BaseClient + 0x6CCA58)   = 1;
	}

	bool cursorVisible = m_Game->m_VguiSurface->IsCursorVisible();
	CViewSetup origSetup = setup;
	CViewSetup origHudSetup = hudViewSetup;

	IMaterialSystem* matSystem = m_Game->m_MaterialSystem;

	int playerIndex = m_Game->m_EngineClient->GetLocalPlayer();
	C_BasePlayer* localPlayer = (C_BasePlayer*)m_Game->GetClientEntity(playerIndex);

	bool isMainMenuLevel = m_Game->m_EngineClient->IsLevelMainMenuBackground();
	bool hasViewEntity = (localPlayer != nullptr) && (*(uint32_t*)((uintptr_t)localPlayer + 0x1738) != 0xFFFFFFFF);
	bool in3DViewControlMenu = isMainMenuLevel && !m_Game->m_EngineClient->Con_IsVisible();
	bool wasIn3DMenu = m_VR->m_In3DViewControlMenu;
	m_VR->m_In3DViewControlMenu = in3DViewControlMenu;

	static Vector s_LastOrigSetupOrigin = { 0.0f, 0.0f, 0.0f };
	Vector originDelta = origSetup.origin - s_LastOrigSetupOrigin;
	float originJump = VectorLength(originDelta);
	s_LastOrigSetupOrigin = origSetup.origin;

	static bool s_WasInViewEntity = false;
	static float s_PrevViewEntityYaw = 0.0f;
	bool inGameViewEntity = hasViewEntity && !in3DViewControlMenu;

	// Lock the VR viewpoint in the 3D main menu to the laptop camera (menu_viewcontrol).
	// The engine still glides to the Options binder / Chapters projector cameras for input raycasts
	// (m_MenuViewOrigin/Angles), but rendering keeps the player seated at the laptop.
	// Re-anchor only on entering the menu level, or during the first 2 s if the camera is still settling.
	static bool s_WasMainMenuLevel = false;
	static auto s_MenuAnchorTime = std::chrono::steady_clock::now();
	static int s_MenuFramesSinceEntry = 0;
	bool menuAnchorChanged = false;
	if (isMainMenuLevel)
	{
		auto nowAnchor = std::chrono::steady_clock::now();
		if (!s_WasMainMenuLevel)
		{
			s_MenuAnchorTime = nowAnchor;
			s_MenuFramesSinceEntry = 0;
		}
		++s_MenuFramesSinceEntry;
		float sinceAnchor = std::chrono::duration<float>(nowAnchor - s_MenuAnchorTime).count();
		bool settling = (sinceAnchor < 2.0f) || (s_MenuFramesSinceEntry < 90);
		Vector anchorDelta = origSetup.origin - m_VR->m_MenuAnchorOrigin;
		if (!s_WasMainMenuLevel || (settling && VectorLength(anchorDelta) > 10.0f))
		{
			m_VR->m_MenuAnchorOrigin = origSetup.origin;
			m_VR->m_MenuAnchorYaw = origSetup.angles.y;
			menuAnchorChanged = true;
		}
	}
	s_WasMainMenuLevel = isMainMenuLevel;

	bool hmdPoseValid = m_VR->m_Poses[vr::k_unTrackedDeviceIndex_Hmd].bPoseIsValid;
	bool didRecenter = false;
	if (hmdPoseValid)
	{
		if (!m_VR->m_HasCenteredHMD || (wasIn3DMenu != in3DViewControlMenu) || (in3DViewControlMenu && menuAnchorChanged) ||
		    (!in3DViewControlMenu && !inGameViewEntity && !s_WasInViewEntity && originJump > 150.0f))
		{
			float targetYaw = in3DViewControlMenu ? m_VR->m_MenuAnchorYaw : origSetup.angles.y;
			m_VR->m_Center = m_VR->m_HmdPose.TrackedDevicePos;
			m_VR->m_RotationOffset.y = targetYaw - m_VR->m_HmdPose.TrackedDeviceAng.y;
			m_VR->m_RotationOffset.y -= 360.0f * std::floor(m_VR->m_RotationOffset.y / 360.0f);
			m_VR->UpdateTracking();
			m_VR->m_HasCenteredHMD = true;
			didRecenter = true;
		}
		else if (in3DViewControlMenu)
		{
			// Keep the VR view fixed: do NOT follow the engine's Options/Chapters camera glides in VR.
		}
		else if (inGameViewEntity)
		{
			if (!s_WasInViewEntity)
			{
				// Entering a vehicle / scripted camera (e.g. Chapter 1 intro car ride): align VR forward with the vehicle's forward
				m_VR->m_Center = m_VR->m_HmdPose.TrackedDevicePos;
				m_VR->m_RotationOffset.y = origSetup.angles.y - m_VR->m_HmdPose.TrackedDeviceAng.y;
				m_VR->m_RotationOffset.y -= 360.0f * std::floor(m_VR->m_RotationOffset.y / 360.0f);
				m_VR->UpdateTracking();
				didRecenter = true;
			}
			else
			{
				// Continuing inside a vehicle / scripted camera: rotate VR reference frame smoothly with the vehicle's yaw delta
				float deltaYaw = origSetup.angles.y - s_PrevViewEntityYaw;
				while (deltaYaw > 180.0f)  deltaYaw -= 360.0f;
				while (deltaYaw < -180.0f) deltaYaw += 360.0f;
				if (std::fabs(deltaYaw) > 0.001f)
				{
					m_VR->m_RotationOffset.y += deltaYaw;
					m_VR->m_RotationOffset.y -= 360.0f * std::floor(m_VR->m_RotationOffset.y / 360.0f);
					m_VR->UpdateTracking();
				}
			}
			s_PrevViewEntityYaw = origSetup.angles.y;
		}
		else if (s_WasInViewEntity && !inGameViewEntity)
		{
			// Exiting a vehicle / scripted camera: re-center HMD position without snapping yaw!
			m_VR->m_Center = m_VR->m_HmdPose.TrackedDevicePos;
			m_VR->UpdateTracking();
			didRecenter = true;
		}
	}
	const bool wasInViewEntityPrev = s_WasInViewEntity;
	s_WasInViewEntity = inGameViewEntity;

	const bool hasMoveParent = (localPlayer != nullptr) &&
		(*(const uint32_t*)((uintptr_t)localPlayer + 0x3F8) != 0xFFFFFFFFu);
	const uint8_t moveType = (localPlayer != nullptr)
		? *(const uint8_t*)((uintptr_t)localPlayer + 0x354)
		: 0u; // C_BaseEntity::m_MoveType (+0x354), MOVETYPE_LADDER == 9
	const bool onLadder = (moveType == 9u);
	const bool inIntroChair = (!in3DViewControlMenu && IsLocalPlayerInIntroChair(m_Game, localPlayer));
	const bool inPausedOrMenu = cursorVisible ||
		(m_Game->m_EngineClient->IsInGame() && m_Game->m_EngineClient->IsPaused());
	const bool roomscaleSyncAllowed =
		m_VR->m_6DOF && m_VR->m_RoomscaleActive && hmdPoseValid &&
		(localPlayer != nullptr) && !in3DViewControlMenu && !isMainMenuLevel &&
		!inGameViewEntity && !wasInViewEntityPrev && !inIntroChair &&
		!hasMoveParent && !onLadder && !m_VR->m_IsCalibrating && !inPausedOrMenu &&
		(originJump <= 40.0f);
	m_VR->m_RoomscaleSyncAllowed = roomscaleSyncAllowed;

	if (roomscaleSyncAllowed && !didRecenter && m_VR->m_RoomscaleCmdActive && m_VR->m_VRScale > 1.0f)
	{
		const float errX = m_VR->m_HmdPosRelative.x;
		const float errY = m_VR->m_HmdPosRelative.y;
		const float errLen = std::hypot(errX, errY);
		if (errLen > 0.05f)
		{
			const float dirX = errX / errLen;
			const float dirY = errY / errLen;
			const float hullAlongErr = originDelta.x * dirX + originDelta.y * dirY;
			if (hullAlongErr > 0.0f)
			{
				const float vStickAlong = std::max(0.0f, m_VR->m_RoomscaleStickVelXY.x * dirX + m_VR->m_RoomscaleStickVelXY.y * dirY);
				const float vRoomAlong  = std::max(0.0f, m_VR->m_RoomscalePullVelXY.x * dirX + m_VR->m_RoomscalePullVelXY.y * dirY);
				const float denom = vStickAlong + vRoomAlong;
				const float roomFrac = (denom > 1.0f) ? (vRoomAlong / denom) : 0.0f;
				const float absorb = std::clamp(hullAlongErr * roomFrac, 0.0f, errLen);
				if (absorb > 0.001f)
				{
					Vector deltaTracker(dirX * (absorb / m_VR->m_VRScale), dirY * (absorb / m_VR->m_VRScale), 0.0f);
					VectorPivotXY(deltaTracker, { 0.0f, 0.0f, 0.0f }, -m_VR->m_RotationOffset.y);
					m_VR->m_Center.x += deltaTracker.x;
					m_VR->m_Center.y += deltaTracker.y;
					m_VR->UpdateTracking();
				}
			}
		}
	}
	else if (!roomscaleSyncAllowed || didRecenter)
	{
		m_VR->m_RoomscaleCmdActive = false;
		m_VR->m_RoomscalePullVelXY = { 0.0f, 0.0f, 0.0f };
	}

	hudViewSetup.width = m_VR->m_RenderWidth;
	hudViewSetup.height = m_VR->m_RenderHeight;
	hudViewSetup.fov = m_VR->m_Fov;
	hudViewSetup.m_flAspectRatio = m_VR->m_Aspect;

	Vector position = setup.origin;

	if (in3DViewControlMenu) {
		m_VR->m_MenuViewOrigin = origSetup.origin;
		m_VR->m_MenuViewAngles = QAngle(origSetup.angles.x, origSetup.angles.y, origSetup.angles.z);
		m_VR->m_MenuViewFov = origSetup.fov;
		m_Game->m_EngineClient->SetViewAngles(m_VR->m_MenuViewAngles);

		// Render from the fixed laptop anchor (never from the Options/Chapters cameras).
		// Seat offsets are configurable in VR\config.txt (MenuSeatHeight / MenuSeatForward, in cm).
		const float cmUnits = 0.01f * m_VR->m_VRScale;
		const float yawRad = m_VR->m_MenuAnchorYaw * (3.14159265f / 180.0f);
		position = m_VR->m_MenuAnchorOrigin;
		position.x += cosf(yawRad) * (m_VR->m_MenuSeatForwardCm * cmUnits);
		position.y += sinf(yawRad) * (m_VR->m_MenuSeatForwardCm * cmUnits);
		position.z += m_VR->m_MenuSeatHeightCm * cmUnits;
	}

	static float s_IntroChairSitZ = 0.0f;
	static bool s_HasIntroChairSitZ = false;
	if (!in3DViewControlMenu && IsLocalPlayerInIntroChair(m_Game, localPlayer))
	{
		const float cmUnits = 0.01f * m_VR->m_VRScale;
		if (!inGameViewEntity)
		{
			s_IntroChairSitZ = origSetup.origin.z;
			s_HasIntroChairSitZ = true;
			position.z += m_VR->m_IntroSeatHeightCm * cmUnits;
		}
		else
		{
			const float baseZ = s_HasIntroChairSitZ ? s_IntroChairSitZ : origSetup.origin.z;
			const float riseFrac = std::clamp((origSetup.origin.z - baseZ) / 14.0f, 0.0f, 1.0f);
			position.z += (1.0f - riseFrac) * (m_VR->m_IntroSeatHeightCm * cmUnits);
		}
	}
	else
	{
		s_HasIntroChairSitZ = false;
	}

	static float s_CarSitZ = 0.0f;
	static bool s_HasCarSitZ = false;
	bool inChapter2Car = false;
	if (!in3DViewControlMenu && inGameViewEntity && m_Game->m_EngineClient)
	{
		const char* levelName = m_Game->m_EngineClient->GetLevelNameShort();
		if (levelName && ContainsCaseInsensitive(levelName, "infra_c2_m1_reserve1"))
			inChapter2Car = true;
	}
	if (inChapter2Car)
	{
		const float cmUnits = 0.01f * m_VR->m_VRScale;
		if (!s_HasCarSitZ)
		{
			s_CarSitZ = origSetup.origin.z;
			s_HasCarSitZ = true;
		}
		const float riseFrac = std::clamp((origSetup.origin.z - (s_CarSitZ + 0.5f)) / 7.0f, 0.0f, 1.0f);
		position.z += (1.0f - riseFrac) * (m_VR->m_CarSeatHeightCm * cmUnits);
	}
	else
	{
		s_HasCarSitZ = false;
	}

	if (m_VR->m_ApplyPortalRotationOffset) {
		Vector vec = position - m_VR->m_SetupOrigin;
		float distance = sqrt(vec.x * vec.x + vec.y * vec.y + vec.z * vec.z);

		// Rudimentary portalling detection
		if (distance > 35) {
			m_VR->m_RotationOffset.y += m_VR->m_PortalRotationOffset.y;
			m_VR->UpdateHMDAngles();
			m_VR->m_ApplyPortalRotationOffset = false;
		}
	}

	m_VR->m_SetupOrigin = position;
	if (!m_VR->m_IsVREnabled || isMainMenuLevel || !localPlayer)
	{
		s_LastUseServerEnt = nullptr;
		m_VR->m_PhoneEquipped = false;
		VRHands::ResetVisualHandPhysics();
		VRViewModelDepth::SetEnabled(m_Game->m_BaseClient, false);
	}
	else if (!in3DViewControlMenu)
	{
		VRHands::UpdateVisualHandPhysics(m_Game, m_VR, position);
		VRGrab::UpdateHeldObjectFingerCollision(m_Game, m_VR);
	}

	Vector hmdAngle = m_VR->GetViewAngle();
	if (!cursorVisible && !in3DViewControlMenu) {
		QAngle inGameAngle(hmdAngle.x, hmdAngle.y, hmdAngle.z);
		inGameAngle = ConvertWorldAnglesForPlayerMoveParent(m_Game, localPlayer, inGameAngle);
		m_Game->m_EngineClient->SetViewAngles(inGameAngle);
	}

	setup.x = 0;
	setup.y = 0;
	setup.width = m_VR->m_RenderWidth;
	setup.height = m_VR->m_RenderHeight;
	setup.fov = m_VR->m_Fov;
	setup.fovViewmodel = m_VR->m_Fov;
	setup.m_flAspectRatio = m_VR->m_Aspect;
	setup.zNear = 2.0f;
	setup.zNearViewmodel = 2.0f;
	if (setup.zFar > setup.zNear)
		setup.zFarViewmodel = setup.zFar;
	setup.angles = hmdAngle;

	// HL:Alyx-style roomscale Head-in-Wall / Out-of-Map / Solid-Prop fade-to-black:
	// Trace against solid world BSP + out-of-map void + solid brush/prop geometry (ignoring player, vehicle, held/attached items, tools, NPCs).
	static float s_HeadClipFade = 0.0f;
	static LARGE_INTEGER s_LastHeadClipQpc{};
	static LARGE_INTEGER s_HeadClipFreq{};
	LARGE_INTEGER nowQpc{};
	QueryPerformanceCounter(&nowQpc);
	float headClipDt = 1.0f / 90.0f;
	if (s_HeadClipFreq.QuadPart == 0)
		QueryPerformanceFrequency(&s_HeadClipFreq);
	if (s_LastHeadClipQpc.QuadPart != 0 && s_HeadClipFreq.QuadPart > 0)
	{
		const double elapsedSec = (double)(nowQpc.QuadPart - s_LastHeadClipQpc.QuadPart) / (double)s_HeadClipFreq.QuadPart;
		if (elapsedSec > 0.0)
			headClipDt = (float)std::min(elapsedSec, 0.05);
	}
	s_LastHeadClipQpc = nowQpc;

	const Vector hmdWorldCenter = m_VR->GetViewOrigin(position);
	float headClipTarget = 0.0f;
	if (!in3DViewControlMenu && !isMainMenuLevel && !inGameViewEntity && localPlayer != nullptr && m_Game->m_EngineTrace)
	{
		headClipTarget = ComputeHeadClipTargetFade(m_Game, m_VR, localPlayer, position, hmdWorldCenter);
	}
	if (isMainMenuLevel || localPlayer == nullptr)
		s_HeadClipFade = 0.0f;
	else
		s_HeadClipFade = AdvanceHeadClipFade(s_HeadClipFade, headClipTarget, headClipDt);

	const float headClipFade = s_HeadClipFade;
	const bool fullyBlackHeadClip = (headClipFade >= 0.996f);

	// Keep 3D spatial audio aligned with the live VR HMD world pose.
	// CViewRender::SetUpView() sends origSetup (which lags by a frame in normal play, is clamped/rotated
	// by prop_vehicle_choreo_generic in the Chapter 1 intro, and glides away during point_viewcontrol/menu).
	if (m_VR->m_IsVREnabled && m_Game->m_EngineClient)
	{
		const Vector audioOrigin = (headClipTarget >= 0.5f) ? position : hmdWorldCenter;
		const AudioState_t vrAudioState = BuildVRAudioState(m_Game, localPlayer, audioOrigin, hmdAngle);
		m_Game->m_EngineClient->SetAudioState(vrAudioState);
		VRAudio::UpdateListenerPose(audioOrigin, QAngle(hmdAngle.x, hmdAngle.y, hmdAngle.z), m_VR->m_IsVREnabled, m_Game);
	}

	static bool s_WasActiveGameplay = false;
	const bool activeGameplay = m_VR->m_IsVREnabled && !in3DViewControlMenu && !isMainMenuLevel &&
		localPlayer != nullptr && m_Game->m_EngineClient && m_Game->m_EngineClient->IsInGame();
	if (activeGameplay)
	{
		const QAngle eyeQAngle(hmdAngle.x, hmdAngle.y, hmdAngle.z);
		VRCameraFlash::UpdateFrame(hmdWorldCenter, eyeQAngle);
		VRInventory::UpdateFrame(hmdWorldCenter, eyeQAngle);
	}
	else if (s_WasActiveGameplay)
	{
		VRInventory::Reset();
		VRCameraFlash::Reset();
		VRHands::ResetFlashlightGripAnim();
	}
	s_WasActiveGameplay = activeGameplay;

	// Disable full-screen 3006x3584 4x MSAA post-process resolve passes in VR:
	// - mat_postprocess_enable (client.dll+0x6CD5F0): when 0, DoEnginePostProcessing (0x1018B0D0) still executes
	//   UpdateTonemapping (0x10188FC0) and ResetColorCorrectionValues (0x10188200), then returns at 0x1018B285
	//   before UpdateScreenEffectTexture (which otherwise forces a full 3006x3584 4x MSAA resolve into _rt_FullFrameFB
	//   and draws a non-MSAA full-screen quad back over the 4x MSAA eye target on both eyes).
	// - mat_motion_blur_enabled (client.dll+0x6CD648) & mat_dof_enabled (client.dll+0x6CD8B0): skip per-eye full-screen resolves.
	if (m_Game->m_BaseClient)
	{
		*(float*)(m_Game->m_BaseClient + 0x6CD61C) = 0.0f;
		*(int32_t*)(m_Game->m_BaseClient + 0x6CD620) = 0;
		*(float*)(m_Game->m_BaseClient + 0x6CD674) = 0.0f;
		*(int32_t*)(m_Game->m_BaseClient + 0x6CD678) = 0;
		*(float*)(m_Game->m_BaseClient + 0x6CD8DC) = 0.0f;
		*(int32_t*)(m_Game->m_BaseClient + 0x6CD8E0) = 0;
	}

	// - _rt_CameraWeapon copy at client.dll+0x198872:
	//   Original bytes: 8B 0D (mov ecx, [1070B1B8h]); Skip bytes: EB 34 (jmp short +0x34 to 0x1988A8).
	//   Only allow copying into _rt_CameraWeapon on Eye Pass 1 when the camera is equipped (!m_VR->m_RightHandEmpty).
	//   Skip it on Eye Pass 2 so both eyes see the exact same mono image on the camera display (zero binocular rivalry/dizziness),
	//   and skip it on both eyes whenever the camera is holstered (saving 2 full-screen MSAA resolves per frame).
	auto writePatch2 = [](uint8_t* ptr, uint8_t b0, uint8_t b1) {
		if (ptr && (ptr[0] != b0 || ptr[1] != b1))
		{
			ptr[0] = b0;
			ptr[1] = b1;
			_ReadWriteBarrier();
		}
	};

	uint8_t* pCamCopy = m_Game->m_BaseClient ? (uint8_t*)(m_Game->m_BaseClient + 0x198872) : nullptr;
	bool camPatchValid = pCamCopy && ((pCamCopy[0] == 0x8B && pCamCopy[1] == 0x0D) || (pCamCopy[0] == 0xEB && pCamCopy[1] == 0x34));

	bool updateCameraScreenOnLeftEye = (!m_VR->m_RightHandEmpty && !in3DViewControlMenu && !isMainMenuLevel && !fullyBlackHeadClip);
	if (camPatchValid)
	{
		if (hkDrawViewModels.isEnabled)
		{
			// Skip client.dll's uncropped CopyRenderTargetToTextureEx(_rt_CameraWeapon) —
			// dDrawViewModels performs an aspect-matched center crop so the 4:3 camera LCD is undistorted in VR.
			writePatch2(pCamCopy, 0xEB, 0x34);
		}
		else if (updateCameraScreenOnLeftEye)
			writePatch2(pCamCopy, 0x8B, 0x0D);
		else
			writePatch2(pCamCopy, 0xEB, 0x34);
	}
	s_UpdateCameraScreenOnEye1 = (hkDrawViewModels.isEnabled && camPatchValid && updateCameraScreenOnLeftEye);

	// Keep ComputeShadowDepthTextures and UnlockAllShadowDepthTextures paired 1:1 on both eyes.
	// Skipping unlock on Eye 1 / compute on Eye 2 breaks flashlight shadows across water reflection/refraction
	// subviews (client.dll+0x1936F0) and can leave CShadowMgr::SetSingleFlashlight(0xFFFF) or stack depth unbalanced.
	// Restore full [0, 1] world depth range for VR viewmodels across both VR eye passes.
	const bool enableVmDepth = m_VR->m_IsVREnabled && !in3DViewControlMenu && !isMainMenuLevel && localPlayer != nullptr;
	struct VmDepthResetGuard
	{
		uintptr_t clientBase;
		~VmDepthResetGuard() { VRViewModelDepth::SetEnabled(clientBase, false); }
	} vmDepthResetGuard{ m_Game->m_BaseClient };
	VRViewModelDepth::SetEnabled(m_Game->m_BaseClient, enableVmDepth);

	// Precompute both stereo eye views from the same frozen frame pose before rendering Eye 1.
	CViewSetup leftEyeView = setup;
	CViewSetup rightEyeView = setup;
	{
		QAngle tempAngleL = QAngle(setup.angles.x, setup.angles.y, setup.angles.z);
		leftEyeView.origin = m_VR->TraceEye((uint32_t*)localPlayer, position, m_VR->GetViewOriginLeft(position), tempAngleL);
		leftEyeView.angles.y = tempAngleL.y;

		QAngle tempAngleR = QAngle(setup.angles.x, setup.angles.y, setup.angles.z);
		rightEyeView.origin = m_VR->TraceEye((uint32_t*)localPlayer, position, m_VR->GetViewOriginRight(position), tempAngleR);
		rightEyeView.angles.y = tempAngleR.y;
	}

	IMatRenderContext* rndrContext = matSystem->GetRenderContext();
	VRScreenFadeState screenFade{};
	const auto tEye1Start = VRProfiler::Clock::now();
	m_VR->m_Profiler.Add(VRProfiler::PreEyes, VRProfiler::Ms(tRenderViewStart, tEye1Start));
	m_VR->m_ActiveEyePass = 1;
	rndrContext->SetRenderTarget(m_VR->m_LeftEyeTexture);
	if (fullyBlackHeadClip)
	{
		// Explicitly reset viewport to full VR eye dimensions so D3D9 Clear is not clipped to last frame's 1280x720 desktop rect.
		rndrContext->Viewport(0, 0, (int)m_VR->m_RenderWidth, (int)m_VR->m_RenderHeight);
		rndrContext->ClearColor4ub(0, 0, 0, 255);
		rndrContext->ClearBuffers(true, true, true);
		rndrContext->Release();
	}
	else
	{
		rndrContext->Release();
		VRInventory::BeginEyeRender(1);
		hkRenderView.fOriginal(ecx, leftEyeView, hudViewSetup, nClearFlags, whatToDraw);
		DrawFlashlightLensGlow(leftEyeView, m_VR->m_LeftEyeTexture);
		if (activeGameplay)
		{
			VRInventory::DrawEyeOverlays(leftEyeView, m_VR->m_LeftEyeTexture);
			VRCameraFlash::DrawCameraFlashGlare(leftEyeView, m_VR->m_LeftEyeTexture);
		}
		VRScreenFadeState envFade = ReadVRScreenFadeState(m_Game);
		const float sincePhotoMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - s_LastCameraPhotoTime).count();
		const bool suppressPhotoWhiteout = (VRCameraFlash::IsFlashActive() || (sincePhotoMs >= 0.0f && sincePhotoMs < 650.0f)) &&
			envFade.active && envFade.r > 0.85f && envFade.g > 0.85f && envFade.b > 0.85f;
		if (suppressPhotoWhiteout)
		{
			envFade = VRScreenFadeState{};
		}
		screenFade = CombineVRScreenFades(envFade, headClipFade);
		const float blindWhite = VRCameraFlash::GetScreenWhiteoutOverride();
		if (blindWhite > 0.002f)
		{
			screenFade = CompositeWhiteFadeOver(screenFade, blindWhite);
		}
		DrawVRScreenFade(m_VR->m_LeftEyeTexture, screenFade);
	}
	const auto tEye1End = VRProfiler::Clock::now();
	m_VR->m_Profiler.Add(VRProfiler::Eye1, VRProfiler::Ms(tEye1Start, tEye1End));

	// Eye 2: always skip _rt_CameraWeapon copy (EB 34)
	s_UpdateCameraScreenOnEye1 = false;
	if (camPatchValid)
		writePatch2(pCamCopy, 0xEB, 0x34);

	rndrContext = matSystem->GetRenderContext();
	m_VR->m_ActiveEyePass = 2;
	rndrContext->SetRenderTarget(m_VR->m_RightEyeTexture);
	if (fullyBlackHeadClip)
	{
		rndrContext->Viewport(0, 0, (int)m_VR->m_RenderWidth, (int)m_VR->m_RenderHeight);
		rndrContext->ClearColor4ub(0, 0, 0, 255);
		rndrContext->ClearBuffers(true, true, true);
		rndrContext->Release();
	}
	else
	{
		rndrContext->Release();
		VRInventory::BeginEyeRender(2);
		// Pass RENDERVIEW_SUPPRESSMONITORRENDERING (4) and clear RENDERVIEW_DRAWHUD (2) on Eye 2
		// so point_camera / CCTV monitors and 2D VGUI HUD pre-render run only once per frame!
		hkRenderView.fOriginal(ecx, rightEyeView, hudViewSetup, nClearFlags, (whatToDraw & ~2) | 4);
		DrawFlashlightLensGlow(rightEyeView, m_VR->m_RightEyeTexture);
		if (activeGameplay)
		{
			VRInventory::DrawEyeOverlays(rightEyeView, m_VR->m_RightEyeTexture);
			VRCameraFlash::DrawCameraFlashGlare(rightEyeView, m_VR->m_RightEyeTexture);
		}
		DrawVRScreenFade(m_VR->m_RightEyeTexture, screenFade);
	}
	VRInventory::BeginEyeRender(0);
	m_VR->m_ActiveEyePass = 0;
	const auto tEye2End = VRProfiler::Clock::now();
	m_VR->m_Profiler.Add(VRProfiler::Eye2, VRProfiler::Ms(tEye1End, tEye2End));

	// Restore original code so any other ViewDrawScene (e.g. RenderWindow desktop pass) behaves like vanilla.
	s_UpdateCameraScreenOnEye1 = false;
	if (camPatchValid)
		writePatch2(pCamCopy, 0x8B, 0x0D);
	VRViewModelDepth::SetEnabled(m_Game->m_BaseClient, false);

	m_PushedHud = false;

	setup = origSetup;
	hudViewSetup = origHudSetup;

	bool show2DMenuOverlay = m_Game->m_EngineClient->Con_IsVisible() || m_VR->Is2DSubMenuVisible();
	rndrContext = matSystem->GetRenderContext();
	rndrContext->SetRenderTarget(NULL);
	if (origSetup.width > 0 && origSetup.height > 0) {
		rndrContext->Viewport(origSetup.x, origSetup.y, origSetup.width, origSetup.height);
	}
	if (show2DMenuOverlay) {
		rndrContext->ClearColor4ub(12, 15, 20, 255);
		rndrContext->ClearBuffers(true, true, true);
	}
	rndrContext->Release();

	m_VR->m_RenderedNewFrame = true;
	m_VR->PushRenderedFrame();

	const auto tRenderViewEnd = VRProfiler::Clock::now();
	m_VR->m_Profiler.Add(VRProfiler::PostEyes, VRProfiler::Ms(tEye2End, tRenderViewEnd));
	m_VR->m_Profiler.Add(VRProfiler::RenderViewTotal, VRProfiler::Ms(tRenderViewStart, tRenderViewEnd));
}

void __fastcall Hooks::dDrawViewModels(void *ecx, void *edx, CViewSetup &viewViewmodel, bool drawViewmodel)
{
	if (m_VR && m_VR->m_IsVREnabled && m_VR->m_ActiveEyePass == 1 && s_UpdateCameraScreenOnEye1 &&
		m_Game && m_Game->m_BaseClient && m_Game->m_MaterialSystem)
	{
		s_UpdateCameraScreenOnEye1 = false;

		void* pCamRTMgr = *reinterpret_cast<void**>(m_Game->m_BaseClient + 0x5F846C);
		ITexture* pCamTex = pCamRTMgr ? *reinterpret_cast<ITexture**>(reinterpret_cast<uint8_t*>(pCamRTMgr) + 0x1C) : nullptr;
		if (pCamTex)
		{
			// MaterialImage::Paint (client.dll+0x16D40E, usescreenaspectratio=1 in infra_camera.res)
			// reads IMaterialSystem::GetAspectRatioInfo() (vtable[35], offset +0x8C) -> float at +0x04
			// (m_flFrameBufferAspectRatio, e.g. 16:9 for the 1280x720 desktop window) and draws the
			// center 1/3 x 1/3 UV patch of _rt_CameraWeapon at (panelTall * targetAspect) x panelTall,
			// clipped to the 700x510 (~4:3) camera LCD.
			// Crop the nearly-square VR eye target (m_Aspect ~0.90) to match targetAspect before
			// copying into _rt_CameraWeapon so both live viewfinder and photo freeze-frame stay 1:1 undistorted.
			float targetAspect = 0.0f;
			void** matVtbl = *reinterpret_cast<void***>(m_Game->m_MaterialSystem);
			if (matVtbl && matVtbl[35])
			{
				typedef const void* (__thiscall* tGetAspectRatioInfo)(void* thisptr);
				const void* pAspectInfo = reinterpret_cast<tGetAspectRatioInfo>(matVtbl[35])(m_Game->m_MaterialSystem);
				if (pAspectInfo)
					targetAspect = *reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(pAspectInfo) + 4);
			}
			if (!std::isfinite(targetAspect) || targetAspect < 0.25f || targetAspect > 8.0f)
			{
				int bbW = 0, bbH = 0;
				m_Game->m_MaterialSystem->GetBackBufferDimensions(bbW, bbH);
				targetAspect = (bbW > 0 && bbH > 0) ? (static_cast<float>(bbW) / static_cast<float>(bbH)) : (16.0f / 9.0f);
			}

			const int eyeW = (viewViewmodel.width > 0) ? viewViewmodel.width : static_cast<int>(m_VR->m_RenderWidth);
			const int eyeH = (viewViewmodel.height > 0) ? viewViewmodel.height : static_cast<int>(m_VR->m_RenderHeight);
			const float tanX = (m_VR->m_TanHalfFovX > 0.1f && std::isfinite(m_VR->m_TanHalfFovX)) ? m_VR->m_TanHalfFovX : 1.0f;
			const float tanY = (m_VR->m_TanHalfFovY > 0.1f && std::isfinite(m_VR->m_TanHalfFovY)) ? m_VR->m_TanHalfFovY : 1.0f;

			if (eyeW > 0 && eyeH > 0)
			{
				// Wide-angle DSLR default (1.0x: tanHalfFovY = 0.76 -> ~38.4 x 28.4 deg on the 700x510 LCD)
				// scaled smoothly by m_CameraZoomFactor (1.0x .. 3.0x, controlled via Right Trackpad Up/Down or X / Left A)
				// while preserving exact 1:1 targetAspect across all VR headsets.
				constexpr float kCameraWideTanHalfFovY = 0.76f;
				const float zoom = std::clamp(m_VR->m_CameraZoomFactor, 1.0f, 3.0f);
				const float effTanHalfFovY = kCameraWideTanHalfFovY / zoom;
				float fracY = effTanHalfFovY / tanY;
				float fracX = (effTanHalfFovY * targetAspect) / tanX;
				const float maxFrac = (fracX > fracY) ? fracX : fracY;
				if (maxFrac > 1.0f)
				{
					fracX /= maxFrac;
					fracY /= maxFrac;
				}

				Rect_t srcRect{};
				srcRect.width  = std::clamp(static_cast<int>(lroundf(static_cast<float>(eyeW) * fracX)), 1, eyeW);
				srcRect.height = std::clamp(static_cast<int>(lroundf(static_cast<float>(eyeH) * fracY)), 1, eyeH);
				srcRect.x      = viewViewmodel.x + (eyeW - srcRect.width) / 2;
				srcRect.y      = viewViewmodel.y + (eyeH - srcRect.height) / 2;

				const int dstW = pCamTex->GetActualWidth();
				const int dstH = pCamTex->GetActualHeight();
				Rect_t dstRect = { 0, 0, dstW, dstH };
				Rect_t* pDstRect = (dstW > 0 && dstH > 0) ? &dstRect : nullptr;

				IMatRenderContext* ctx = m_Game->m_MaterialSystem->GetRenderContext();
				if (ctx)
				{
					// Verified against client.dll+0x19889B & 0x1AE3E1:
					// IMatRenderContext::CopyRenderTargetToTextureEx is at vtable offset +0x1C4 (slot 113).
					void** ctxVtbl = *reinterpret_cast<void***>(ctx);
					if (ctxVtbl && ctxVtbl[113])
					{
						typedef void (__thiscall* tCopyRTToTexEx)(void* thisptr, ITexture* pTexture, int nRenderTargetID, Rect_t* pSrcRect, Rect_t* pDstRect);
						reinterpret_cast<tCopyRTToTexEx>(ctxVtbl[113])(ctx, pCamTex, 0, &srcRect, pDstRect);
					}
					ctx->Release();
				}
			}
		}
	}

	hkDrawViewModels.fOriginal(ecx, viewViewmodel, drawViewmodel);
}

bool __fastcall Hooks::dCreateMove(void *ecx, void *edx, float flInputSampleTime, CUserCmd *cmd)
{
	if (!cmd)
		return false;
	if (!cmd->command_number)
		return hkCreateMove.fOriginal(ecx, flInputSampleTime, cmd);

	bool menuSelectDown = false;
	if (m_VR && m_VR->m_IsVREnabled && m_VR->m_In3DViewControlMenu)
	{
		cmd->viewangles = m_VR->m_MenuViewAngles;
		menuSelectDown = !m_VR->m_SuppressMenuSelectUntilRelease &&
		                 (m_VR->PressedDigitalAction(m_VR->m_MenuSelect) ||
		                  m_VR->PressedDigitalAction(m_VR->m_ActionPrimaryAttack));
		if (menuSelectDown)
		{
			// C_BasePlayer::UpdateVGuiScreenButtonState(cmd) runs inside hkCreateMove.fOriginal
			// and transfers IN_ATTACK (1) / IN_USE (0x20) into C_VGuiScreen::m_nButtonPressed (+0xA98),
			// which C_VGuiScreen::ClientThink (client.dll+0x902A1) uses to trigger InternalMousePressed(MOUSE_LEFT)
			// inside the 3D VGuiScreen's private InputContext!
			cmd->buttons |= IN_ATTACK;
		}
	}

	bool origRet = hkCreateMove.fOriginal(ecx, flInputSampleTime, cmd);

	if (!m_VR || !m_VR->m_IsVREnabled)
		return origRet;

	{
		if (m_VR->m_In3DViewControlMenu)
		{
			m_VR->m_RoomscaleCmdActive = false;
			m_VR->m_RoomscalePullVelXY = { 0.0f, 0.0f, 0.0f };
			cmd->viewangles = m_VR->m_MenuViewAngles;
			cmd->forwardmove = 0.0f;
			cmd->sidemove = 0.0f;
			cmd->upmove = 0.0f;
			cmd->buttons &= ~(IN_FORWARD | IN_BACK | IN_MOVELEFT | IN_MOVERIGHT | IN_ATTACK | IN_ATTACK2 | IN_USE | IN_RELOAD | IN_JUMP | IN_DUCK | (1u << 30));
			if (menuSelectDown)
				cmd->buttons |= IN_ATTACK;
			return origRet;
		}

		C_BasePlayer* cmdPlayer = (m_Game && m_Game->m_EngineClient && m_Game->m_EngineClient->IsInGame())
			? (C_BasePlayer*)m_Game->GetClientEntity(m_Game->m_EngineClient->GetLocalPlayer())
			: nullptr;
		cmd->viewangles = ConvertWorldAnglesForPlayerMoveParent(m_Game, cmdPlayer, m_VR->m_HmdAngAbs);

		bool inInGameMenu = m_Game->m_VguiSurface->IsCursorVisible() ||
		                    (m_Game->m_EngineClient->IsInGame() && m_Game->m_EngineClient->IsPaused());
		if (m_VR->m_IsCalibrating || inInGameMenu)
		{
			m_VR->m_RoomscaleCmdActive = false;
			m_VR->m_RoomscalePullVelXY = { 0.0f, 0.0f, 0.0f };
			cmd->forwardmove = 0.0f;
			cmd->sidemove = 0.0f;
			cmd->upmove = 0.0f;
			cmd->buttons &= ~(IN_FORWARD | IN_BACK | IN_MOVELEFT | IN_MOVERIGHT | IN_ATTACK | IN_ATTACK2 | IN_USE | IN_RELOAD | IN_JUMP | IN_DUCK | (1u << 30));
			return false;
		}

		// Clear flatscreen IN_FLASHLIGHT so VR Left B/Y (equip) and Left Trigger (beam toggle) control it cleanly
		cmd->buttons &= ~(1u << 30);

		// Apply VR controller buttons AFTER hkCreateMove.fOriginal so CInput::CreateMove doesn't overwrite them:
		// - Right Trigger (m_ActionPrimaryAttack): Take Photo / Primary Attack (IN_ATTACK)
		if (m_VR->PressedDigitalAction(m_VR->m_ActionPrimaryAttack))
			cmd->buttons |= IN_ATTACK;
		VRGrab::FilterCreateMoveButtons(m_VR, cmd);

		// - Both Left Grip (m_ActionCrouch) and Right Grip (m_ActionReload) + close-contact Knuckles curl: Interact / Use (+use = IN_USE)
		//   Hold-to-Grab: keep IN_USE active while Grip is held; releasing Grip immediately drops the object in dUpdateObject / dProcessUsercmds.
		VRGrab::UpdateCreateMoveUseButton(m_VR, cmd);

		// - Left A / X Button (m_ActionSecondaryAttack / m_ActionJump): Camera LCD Zoom when Camera is equipped; Secondary Action (+attack2) otherwise
		if ((m_VR->m_RightHandEmpty || m_VR->m_PhoneEquipped) &&
		    !m_VR->m_SuppressAltFireUntilRelease &&
		    (m_VR->PressedDigitalAction(m_VR->m_ActionSecondaryAttack) || m_VR->PressedDigitalAction(m_VR->m_ActionJump)))
			cmd->buttons |= IN_ATTACK2;

		// - Right Touchpad Swipe Down: Reload Camera Battery (+reload = IN_RELOAD)
		if (m_VR->m_CameraReloadTicks > 0)
		{
			cmd->buttons |= IN_RELOAD;
			m_VR->m_CameraReloadTicks--;
			if (m_VR->m_CameraReloadTicks == 0)
				m_VR->m_PendingCameraReload = false;
		}

		// - Right Stick Up / Down: Jump / Crouch Toggle
		if (m_VR->m_StickJumpActive)
			cmd->buttons |= IN_JUMP;
		if (m_VR->m_StickCrouchActive)
			cmd->buttons |= IN_DUCK;

		if (m_VR->m_SprintActive)
		{
			cmd->buttons |= (1 << 17); // IN_SPEED
		}

		float stickFwd = 0.0f;
		float stickSide = 0.0f;
		vr::InputAnalogActionData_t analogActionData;
		if (m_VR->GetAnalogActionData(m_VR->m_ActionWalk, analogActionData)) {
			// Run toward other guy
			cmd->buttons &= ~(IN_FORWARD | IN_BACK | IN_MOVELEFT | IN_MOVERIGHT);

			stickFwd = analogActionData.y * MAX_LINEAR_SPEED;
			stickSide = analogActionData.x * MAX_LINEAR_SPEED;
			cmd->forwardmove += stickFwd;
			cmd->sidemove += stickSide;

			// We'll only be moving fwd or sideways
			cmd->upmove = 0.0f;
		}

		constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
		const float yawRad = cmd->viewangles.y * kDegToRad;
		const Vector flatFwd(std::cos(yawRad), std::sin(yawRad), 0.0f);
		const Vector flatRight(std::sin(yawRad), -std::cos(yawRad), 0.0f);
		const Vector stickWorldVelXY(
			flatFwd.x * stickFwd + flatRight.x * stickSide,
			flatFwd.y * stickFwd + flatRight.y * stickSide,
			0.0f);

		if (m_VR->m_RoomscaleSyncAllowed)
		{
			const float errX = m_VR->m_HmdPosRelative.x;
			const float errY = m_VR->m_HmdPosRelative.y;
			const float errLen = std::hypot(errX, errY);
			constexpr float kDeadzoneUnits = 1.5f;
			if (errLen > kDeadzoneUnits)
			{
				const float pullSpeed = std::clamp((errLen - kDeadzoneUnits) * 14.0f, 35.0f, 190.0f);
				const Vector roomVel((errX / errLen) * pullSpeed, (errY / errLen) * pullSpeed, 0.0f);
				const float addFwd = roomVel.x * flatFwd.x + roomVel.y * flatFwd.y;
				const float addSide = roomVel.x * flatRight.x + roomVel.y * flatRight.y;
				cmd->forwardmove = std::clamp(cmd->forwardmove + addFwd, -(float)MAX_LINEAR_SPEED, (float)MAX_LINEAR_SPEED);
				cmd->sidemove = std::clamp(cmd->sidemove + addSide, -(float)MAX_LINEAR_SPEED, (float)MAX_LINEAR_SPEED);
				m_VR->m_RoomscaleCmdActive = true;
				m_VR->m_RoomscaleStickVelXY = stickWorldVelXY;
				m_VR->m_RoomscalePullVelXY = roomVel;
			}
			else
			{
				m_VR->m_RoomscaleCmdActive = false;
				m_VR->m_RoomscaleStickVelXY = stickWorldVelXY;
				m_VR->m_RoomscalePullVelXY = { 0.0f, 0.0f, 0.0f };
			}
		}
		else
		{
			m_VR->m_RoomscaleCmdActive = false;
			m_VR->m_RoomscalePullVelXY = { 0.0f, 0.0f, 0.0f };
		}

		cmd->buttons &= ~(IN_FORWARD | IN_BACK | IN_MOVELEFT | IN_MOVERIGHT);
		if (cmd->forwardmove > 0.0f)
			cmd->buttons |= IN_FORWARD;
		else if (cmd->forwardmove < 0.0f)
			cmd->buttons |= IN_BACK;

		if (cmd->sidemove > 0.0f)
			cmd->buttons |= IN_MOVERIGHT;
		else if (cmd->sidemove < 0.0f)
			cmd->buttons |= IN_MOVELEFT;
	}

	return false;
}

void __fastcall Hooks::dCalcViewModelView(void *ecx, void *edx, const Vector &eyePosition, const QAngle &eyeAngles)
{
	hkCalcViewModelView.fOriginal(ecx, eyePosition, eyeAngles);

	if (m_VR->m_IsVREnabled && ecx)
	{
		// Cache and reference-pin viewmodel IMaterial* pointers so FindMaterial isn't called 11+ times per eye pass
		// and CMaterialSystem::UncacheUnusedMaterials() never invalidates them on map transitions.
		static PinnedMaterialSlot s_AlwaysHiddenMats[4];
		static const char* s_AlwaysHiddenViewmodelMaterials[4] = {
			"models/characters/player_body_work",
			"models/weapons/v_camera/maglite",
			"models/weapons/v_light/camera",
			"models/weapons/v_superlight/camera"
		};
		for (int i = 0; i < 4; ++i)
		{
			if (IMaterial* mat = s_AlwaysHiddenMats[i].Get(m_Game->m_MaterialSystem, s_AlwaysHiddenViewmodelMaterials[i], "Model textures"))
				mat->SetMaterialVarFlag(MATERIAL_VAR_NO_DRAW, true);
		}

		static PinnedMaterialSlot s_HandMat;
		if (IMaterial* mat = s_HandMat.Get(m_Game->m_MaterialSystem, "models/characters/player_hands", "Model textures"))
		{
			mat->SetMaterialVarFlag(MATERIAL_VAR_NO_DRAW, !m_VR->m_ShowHands);
			static IMaterial* s_PatchedHandMat = nullptr;
			static unsigned long long s_LastHandMatVerifyMs = 0ULL;
			typedef void(__thiscall* tSetFloatValue)(void*, float);
			typedef void(__thiscall* tSetIntValue)(void*, int);
			typedef void(__thiscall* tSetVecValue)(void*, const float*, int);
			typedef int(__thiscall* tGetIntValue)(void*);
			typedef float(__thiscall* tGetFloatValue)(void*);

			bool needsPatch = (mat != s_PatchedHandMat);
			const unsigned long long handVerifyNowMs = GetTickCount64();
			if (!needsPatch && (handVerifyNowMs - s_LastHandMatVerifyMs) >= 2000ULL)
			{
				s_LastHandMatVerifyMs = handVerifyNowMs;
				bool foundPhong = false;
				bool foundPhongBoost = false;
				void* varPhong = mat->FindVar("$phong", &foundPhong, false);
				void* varPhongBoost = mat->FindVar("$phongboost", &foundPhongBoost, false);

				if (foundPhong && varPhong)
				{
					if (const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(varPhong))
					{
						if ((*reinterpret_cast<const tGetIntValue*>(vt + 0x6C))(varPhong) != 0)
							needsPatch = true;
					}
				}
				if (!needsPatch && foundPhongBoost && varPhongBoost)
				{
					if (const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(varPhongBoost))
					{
						if ((*reinterpret_cast<const tGetFloatValue*>(vt + 0x70))(varPhongBoost) > 0.001f)
							needsPatch = true;
					}
				}
			}

			if (needsPatch)
			{
				auto setIntVar = [&](const char* name, int val) {
					bool found = false;
					if (void* var = mat->FindVar(name, &found, false))
					{
						if (found)
						{
							if (const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(var))
								(*reinterpret_cast<const tSetIntValue*>(vt + 0x14))(var, val);
						}
					}
				};
				auto setFloatVar = [&](const char* name, float val) {
					bool found = false;
					if (void* var = mat->FindVar(name, &found, false))
					{
						if (found)
						{
							if (const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(var))
								(*reinterpret_cast<const tSetFloatValue*>(vt + 0x10))(var, val);
						}
					}
				};
				auto setVec3Var = [&](const char* name, float x, float y, float z) {
					bool found = false;
					if (void* var = mat->FindVar(name, &found, false))
					{
						if (found)
						{
							if (const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(var))
							{
								const float v[3] = { x, y, z };
								(*reinterpret_cast<const tSetVecValue*>(vt + 0x34))(var, v, 3);
							}
						}
					}
				};

				setIntVar("$phong", 0);
				setIntVar("$rimlight", 0);
				setFloatVar("$phongboost", 0.0f);
				setFloatVar("$rimlightboost", 0.0f);
				setVec3Var("$phongtint", 0.0f, 0.0f, 0.0f);
				setVec3Var("$phongfresnelranges", 0.0f, 0.0f, 0.0f);
				setVec3Var("$color2", 0.82f, 0.78f, 0.76f);

				mat->RefreshPreservingMaterialVars();
				s_PatchedHandMat = mat;
				s_LastHandMatVerifyMs = handVerifyNowMs;
			}
		}

		// Belt w_camera / w_light / w_superlight share materials with viewmodels; never force NO_DRAW here.
		static PinnedMaterialSlot s_CamMat;
		IMaterial* camMat = s_CamMat.Get(m_Game->m_MaterialSystem, "models/weapons/v_camera/camera", "Model textures");
		if (camMat)
			camMat->SetMaterialVarFlag(MATERIAL_VAR_NO_DRAW, false);

		static PinnedMaterialSlot s_FlashlightMats[5];
		static const char* s_FlashlightMaterials[5] = {
			"models/weapons/v_light/maglite",
			"models/weapons/v_superlight/led_flashlight",
			"models/weapons/v_superlight/led_flashlight_backplate",
			"models/weapons/v_superlight/led_flashlight_backplate_skin1",
			"models/weapons/v_superlight/led_flashlight_backplate_skin2"
		};
		for (int i = 0; i < 5; ++i)
		{
			if (IMaterial* mat = s_FlashlightMats[i].Get(m_Game->m_MaterialSystem, s_FlashlightMaterials[i], "Model textures"))
				mat->SetMaterialVarFlag(MATERIAL_VAR_NO_DRAW, false);
		}

		typedef void* (__thiscall *tGetViewModel)(void *thisptr, int index);
		typedef void(__thiscall *tSetLocalOrigin)(void *thisptr, const Vector &origin);
		typedef void(__thiscall *tSetLocalAngles)(void *thisptr, const QAngle &angles);
		typedef void(__thiscall *tClientSetWeaponModel)(void *thisptr, const char *modelname, void *weapon);
		typedef void(__thiscall *tClientRemoveEffects)(void *thisptr, int nEffects);
		typedef void* (__thiscall *tClientGetOwningWeapon)(void *thisptr);
		typedef void(__thiscall *tClientUpdateVisibility)(void *thisptr);
		typedef void(__thiscall *tClientSendVmMatchingSeq)(void *thisptr, int seq);

		auto fnGetViewModel          = (tGetViewModel)(m_Game->m_BaseClient + 0x55260);
		auto fnSetLocalOrigin        = (tSetLocalOrigin)(m_Game->m_BaseClient + 0x331E0);
		auto fnSetLocalAngles        = (tSetLocalAngles)(m_Game->m_BaseClient + 0x332C0);
		auto fnClientSetWeaponModel  = (tClientSetWeaponModel)(m_Game->m_BaseClient + 0x16630);
		auto fnClientRemoveEffects   = (tClientRemoveEffects)(m_Game->m_BaseClient + 0x15F90);
		auto fnClientGetOwningWeapon = (tClientGetOwningWeapon)(m_Game->m_BaseClient + 0x15FE0);
		auto fnClientUpdateVis       = (tClientUpdateVisibility)(m_Game->m_BaseClient + 0x3BB30);

		tClientSendVmMatchingSeq fnClientSendVmMatchingSeq = nullptr;
		if (m_Game->m_BaseClient)
		{
			const uint8_t* pSendSeq = reinterpret_cast<const uint8_t*>(m_Game->m_BaseClient + 0x166C0);
			if (pSendSeq[0] == 0x55 && pSendSeq[1] == 0x8B && pSendSeq[2] == 0xEC &&
			    pSendSeq[3] == 0x8B && pSendSeq[4] == 0x45 && pSendSeq[5] == 0x08 &&
			    pSendSeq[6] == 0x56 && pSendSeq[7] == 0x50)
			{
				fnClientSendVmMatchingSeq = reinterpret_cast<tClientSendVmMatchingSeq>(m_Game->m_BaseClient + 0x166C0);
			}
		}

		Vector primaryPos, fwd0, right0, up0;
		QAngle primaryAng;
		VRGrab::GetGimbalFreeControllerBasis(m_VR, m_VR->m_LeftHanded, eyePosition, primaryPos, fwd0, right0, up0, primaryAng);

		Vector secondaryPos, fwd1, right1, up1;
		QAngle secondaryAng;
		VRGrab::GetGimbalFreeControllerBasis(m_VR, !m_VR->m_LeftHanded, eyePosition, secondaryPos, fwd1, right1, up1, secondaryAng);

		bool inMainMenu = m_VR->m_In3DViewControlMenu || m_Game->m_EngineClient->IsLevelMainMenuBackground();

		// ViewModel 0: Camera / Phone / Right VR Hand (v_camera.mdl local grip offset: +15.5 fwd, +1.9 right, -2.1 up)
		void *vm0 = fnGetViewModel(ecx, 0);
		if (vm0 && !inMainMenu)
		{
			void *clientWeapon = fnClientGetOwningWeapon(vm0);
			// Verified client.dll RTTI vtables:
			// C_INFRAHands  = m_BaseClient + 0x4DDDF4
			// C_INFRACamera = m_BaseClient + 0x4DAFB4
			// C_INFRAPhone  = m_BaseClient + 0x4DFB1C
			uintptr_t clientHandsVtable = m_Game->m_BaseClient + 0x4DDDF4;
			uintptr_t clientPhoneVtable = m_Game->m_BaseClient + 0x4DFB1C;
			bool isClientEmptyHands = (clientWeapon == nullptr) || (*(uintptr_t*)clientWeapon == clientHandsVtable);
			bool isPhoneEquipped = (clientWeapon != nullptr) && (*(uintptr_t*)clientWeapon == clientPhoneVtable);
			m_VR->m_RightHandEmpty = isClientEmptyHands;
			m_VR->m_PhoneEquipped = isPhoneEquipped;

			if (camMat)
			{
				camMat->SetMaterialVarFlag(MATERIAL_VAR_NO_DRAW, false);
			}

			if (m_VR->m_ShowHands && isClientEmptyHands)
			{
				int targetModelIdx = -1;
				const char *targetBareHandMdl = SelectBareHandModel(&targetModelIdx);

				// C_INFRAHands::WeaponIdle (client.dll+0x89B0) calls SendWeaponAnim(ACT_VM_IDLE) -> SetViewModel() every client frame,
				// which reads FileWeaponInfo_t::szViewModel (+0xA6 via GetViewModel at vtable+0x4D4) and sets client vm0's model to ""!
				// Only patch szViewModel when clientWeapon is C_INFRAHands (NEVER overwrite C_INFRAPhone's "models/weapons/v_phone.mdl"!).
				if (clientWeapon && *(uintptr_t*)clientWeapon == clientHandsVtable)
				{
					auto fnClientWeaponGetViewModel = *(char*(__thiscall**)(void*, int))(*(uintptr_t*)clientWeapon + 0x4D4);
					if (fnClientWeaponGetViewModel)
					{
						char *szClientViewModel = fnClientWeaponGetViewModel(clientWeapon, 0);
						if (szClientViewModel && _stricmp(szClientViewModel, targetBareHandMdl) != 0)
						{
							strcpy_s(szClientViewModel, 80, targetBareHandMdl);
						}
					}
					if ((*(uint32_t*)((uintptr_t)clientWeapon + 0xE8) & 0x20) != 0)
					{
						fnClientRemoveEffects(clientWeapon, 0x20);
					}
				}

				if (*(void**)((uintptr_t)vm0 + 0x68) == nullptr ||
				    (targetModelIdx != -1 && *(int16_t*)((uintptr_t)vm0 + 0x350) != (int16_t)targetModelIdx))
				{
					fnClientSetWeaponModel(vm0, targetBareHandMdl, clientWeapon);
				}

				*(uint8_t*)((uintptr_t)vm0 + 0x370) = 1; // m_bReadyToDraw = true
				*(uint8_t*)((uintptr_t)vm0 + 0xE0) &= ~1; // Clear m_bDormant if set
				if ((*(uint32_t*)((uintptr_t)vm0 + 0xE8) & 0x20) != 0)
				{
					fnClientRemoveEffects(vm0, 0x20);
				}
				else if (*(uint16_t*)((uintptr_t)vm0 + 0x3D4) == 0xFFFF)
				{
					// Re-insert into ClientLeafSystem if m_hRenderHandle (+0x3D4) is invalid
					fnClientUpdateVis(vm0);
				}
				if (fnClientSendVmMatchingSeq && *(int*)((uintptr_t)vm0 + 0xCF4) != 0)
				{
					fnClientSendVmMatchingSeq(vm0, 0);
				}
			}
			else if (!isClientEmptyHands && clientWeapon)
			{
				// Active tool equipped (C_INFRACamera or C_INFRAPhone): ensure vm0 immediately uses the weapon's own viewmodel (e.g. v_phone.mdl)
				auto fnClientWeaponGetViewModel = *(char*(__thiscall**)(void*, int))(*(uintptr_t*)clientWeapon + 0x4D4);
				if (fnClientWeaponGetViewModel && m_Game->m_ModelInfo)
				{
					const char *weaponVmMdl = fnClientWeaponGetViewModel(clientWeapon, 0);
					if (weaponVmMdl && weaponVmMdl[0] != '\0')
					{
						int expectedIdx = m_Game->m_ModelInfo->GetModelIndex(weaponVmMdl);
						if (expectedIdx != -1 && *(int16_t*)((uintptr_t)vm0 + 0x350) != (int16_t)expectedIdx)
						{
							fnClientSetWeaponModel(vm0, weaponVmMdl, clientWeapon);
						}
					}
				}
				if (fnClientSendVmMatchingSeq)
				{
					const int clientVm0Seq = *(int*)((uintptr_t)vm0 + 0xCF4);
					if (isPhoneEquipped)
					{
						// Suppress draw (5) and holster (6); preserve answer(2)/talk_idle(3)/hangup(4)/text(7)
						if (clientVm0Seq == 5 || clientVm0Seq == 6)
							fnClientSendVmMatchingSeq(vm0, 0);
					}
					else if (*(uintptr_t*)clientWeapon == (m_Game->m_BaseClient + 0x4DAFB4)) // C_INFRACamera
					{
						if (clientVm0Seq == 3 || clientVm0Seq == 4 || clientVm0Seq == 5)
							fnClientSendVmMatchingSeq(vm0, 0);
					}
				}
			}
			Vector targetOrigin0 = primaryPos - (fwd0 * 15.5f) - (right0 * 1.9f) + (up0 * 2.1f);
			if (isPhoneEquipped)
				VRHands::ApplyEquippedPhoneTilt(targetOrigin0, fwd0, right0, up0, &primaryAng);
			else if (!isClientEmptyHands)
			{
				const float camShiftUnits = 0.01f * m_VR->m_VRScale; // 1.0 cm up (+up0) and 1.0 cm toward viewer (-fwd0)
				targetOrigin0 = targetOrigin0 + (up0 * camShiftUnits) - (fwd0 * camShiftUnits);
			}
			Vector visOrigin0 = targetOrigin0;
			QAngle visAngles0 = primaryAng;
			VRHands::GetVisualViewModelTransform(m_VR, true, eyePosition, targetOrigin0, primaryAng, visOrigin0, visAngles0);
			fnSetLocalAngles(vm0, visAngles0);
			fnSetLocalOrigin(vm0, visOrigin0);
		}
		else
		{
			m_VR->m_PhoneEquipped = false;
		}

		// ViewModel 1: Flashlight / Left VR Hand (v_phone.mdl when bare so left hand shares natural open sculpt; v_light/v_superlight when equipped)
		void *vm1 = fnGetViewModel(ecx, 1);
		if (vm1 && !inMainMenu)
		{
			if (m_VR->m_ShowHands)
			{
				const char* targetVm1Mdl = nullptr;
				if (!m_VR->m_FlashlightEquipped && m_Game->m_ModelInfo &&
				    m_Game->m_ModelInfo->GetModelIndex("models/weapons/v_phone.mdl") != -1)
				{
					targetVm1Mdl = "models/weapons/v_phone.mdl";
				}
				else
				{
					targetVm1Mdl = m_VR->m_HasSuperFlashlight ? "models/weapons/v_superlight.mdl" : "models/weapons/v_light.mdl";
				}
				int targetIdx1 = m_Game->m_ModelInfo ? m_Game->m_ModelInfo->GetModelIndex(targetVm1Mdl) : -1;
				if (*(void**)((uintptr_t)vm1 + 0x68) == nullptr ||
				    (targetIdx1 != -1 && *(int16_t*)((uintptr_t)vm1 + 0x350) != (int16_t)targetIdx1))
				{
					fnClientSetWeaponModel(vm1, targetVm1Mdl, fnClientGetOwningWeapon(vm1));
				}

				*(uint8_t*)((uintptr_t)vm1 + 0x370) = 1; // m_bReadyToDraw = true
				*(uint8_t*)((uintptr_t)vm1 + 0xE0) &= ~1; // Clear m_bDormant if set
				if ((*(uint32_t*)((uintptr_t)vm1 + 0xE8) & 0x20) != 0)
				{
					fnClientRemoveEffects(vm1, 0x20);
				}
				else if (*(uint16_t*)((uintptr_t)vm1 + 0x3D4) == 0xFFFF)
				{
					fnClientUpdateVis(vm1);
				}
			}
			Vector targetOrigin1 = secondaryPos - (fwd1 * 16.7f) + (right1 * 7.6f) + (up1 * 5.0f);
			Vector visOrigin1 = targetOrigin1;
			QAngle visAngles1 = secondaryAng;
			VRHands::GetVisualViewModelTransform(m_VR, false, eyePosition, targetOrigin1, secondaryAng, visOrigin1, visAngles1);
			fnSetLocalAngles(vm1, visAngles1);
			fnSetLocalOrigin(vm1, visOrigin1);
		}
	}
}

float __fastcall Hooks::dProcessUsercmds(void *ecx, void *edx, edict_t *player, void *buf, int numcmds, int totalcmds, int dropped_packets, bool ignore, bool paused)
{
	Server_BaseEntity *pPlayer = (player && player->m_pUnk) ? (Server_BaseEntity*)player->m_pUnk->GetBaseEntity() : nullptr;

	int index = (pPlayer && EntityIndex) ? EntityIndex(pPlayer) : -1;
	m_Game->m_CurrentUsercmdID = index;

	const bool isLocalVrGameplay = (m_VR && m_VR->m_IsVREnabled && pPlayer && m_Game->m_BaseServer &&
	    index == m_Game->m_EngineClient->GetLocalPlayer() &&
	    !m_VR->m_In3DViewControlMenu && !m_Game->m_EngineClient->IsLevelMainMenuBackground());

	typedef void (__thiscall *tSetContextThink)(void *thisptr, void *pfnThink, float thinkTime, const char *szContext);
	tSetContextThink fnSetContextThink = nullptr;
	if (m_Game->m_BaseServer)
	{
		const uint8_t* pSetCtx = reinterpret_cast<const uint8_t*>(m_Game->m_BaseServer + 0xCC250);
		if (pSetCtx[0] == 0x55 && pSetCtx[1] == 0x8B && pSetCtx[2] == 0xEC &&
		    pSetCtx[3] == 0x56 && pSetCtx[4] == 0x57)
		{
			fnSetContextThink = reinterpret_cast<tSetContextThink>(m_Game->m_BaseServer + 0xCC250);
		}
	}

	if (isLocalVrGameplay)
	{
		uintptr_t baseSrvPre = m_Game->m_BaseServer;
		if (*(uint32_t*)((uintptr_t)pPlayer + 0x182C) != 0xFFFFFFFFu &&
		    *(float*)((uintptr_t)pPlayer + 0x1830) > 0.0f)
		{
			typedef void* (__thiscall *tGetActiveWeaponPre)(void *thisptr);
			auto fnGetActiveWeaponPre = (tGetActiveWeaponPre)(baseSrvPre + 0xAA280);
			void* preAct = fnGetActiveWeaponPre(pPlayer);
			if (preAct && fnSetContextThink)
			{
				fnSetContextThink(preAct, nullptr, 0.0f, "BaseCombatWeapon_HideThink");
			}
			*(float*)((uintptr_t)pPlayer + 0x1830) = 0.0f;
		}
	}

	float ret = hkProcessUsercmds.fOriginal(ecx, player, buf, numcmds, totalcmds, dropped_packets, ignore, paused);

	if (isLocalVrGameplay)
	{
		if (*(uint32_t*)((uintptr_t)pPlayer + 0x182C) != 0xFFFFFFFFu &&
		    *(float*)((uintptr_t)pPlayer + 0x1830) > 0.0f)
		{
			*(float*)((uintptr_t)pPlayer + 0x1830) = 0.0f;
		}
		if (pPlayer != s_PrevServerPlayer)
		{
			s_PrevServerPlayer = pPlayer;
			s_LastUseServerEnt = nullptr;
			VRGrab::ResetOpenHandPhysics();
			VRInventory::Reset();
			VRCameraFlash::Reset();
			ResetCameraScreenCache();
			VRHands::ResetFlashlightGripAnim();
			m_VR->m_PendingFlashlightBoost = false;

			typedef int(__cdecl* tPrecacheModel)(const char* modelName);
			auto fnPrecacheModel = (tPrecacheModel)(m_Game->m_BaseServer + 0xC7ED0);
			fnPrecacheModel("models/items/batterypack_001.mdl");
			fnPrecacheModel("models/items/batterypack_002.mdl");
			fnPrecacheModel("models/weapons/w_camera.mdl");
			fnPrecacheModel("models/weapons/w_light.mdl");
			fnPrecacheModel("models/weapons/w_superlight.mdl");
			fnPrecacheModel("models/weapons/v_phone.mdl");
			fnPrecacheModel("models/props_office/mobile_phone_001.mdl");

			// Neutralize skill1.cfg re-binding F7/p to mat_texture_list on every map load (crashes under mat_queue_mode 2).
			m_Game->ClientCmd_Unrestricted("mat_texture_list 0; unbind \"F7\"; unbind \"F8\"; unbind \"p\"");
		}
		typedef void* (__thiscall *tGetActiveWeapon)(void *thisptr);
		typedef void* (__thiscall *tGetServerViewModel)(void *thisptr, int vmIndex);
		typedef const char* (__thiscall *tGetEntityModelName)(void *thisptr);
		typedef void (__thiscall *tResetSequenceInfo)(void *thisptr);
		typedef void (__thiscall *tAddEffects)(void *thisptr, int effects);
		typedef void (__thiscall *tRemoveEffects)(void *thisptr, int effects);
		typedef void (__thiscall *tSetWeaponVisible)(void *thisptr, bool visible);
		typedef bool (__thiscall *tHasSuperFlashlight)(void *thisptr);
		typedef bool (__thiscall *tIsFlashlightBoosted)(void *thisptr);
		typedef int  (__thiscall *tGetFlashlightBattery)(void *thisptr);
		typedef void (__thiscall *tSendFlashlightAnim)(void *thisptr, int activity);
		typedef void (__thiscall *tOnFlashlightTurnedOff)(void *thisptr);
		typedef void (__thiscall *tUpdateFlashlightSkin)(void *thisptr);
		typedef void (__thiscall *tActivateFlashlightBoost)(void *thisptr, bool bPlayAnim);

		uintptr_t baseSrv = m_Game->m_BaseServer;
		auto fnGetActiveWeapon         = (tGetActiveWeapon)(baseSrv + 0xAA280);
		auto fnGetServerViewModel      = (tGetServerViewModel)(baseSrv + 0x1B36C0);
		auto fnGetEntityModelName      = (tGetEntityModelName)(baseSrv + 0xB2720);
		auto fnResetSequenceInfo       = (tResetSequenceInfo)(baseSrv + 0x9D540);
		auto fnAddEffects              = (tAddEffects)(baseSrv + 0xCB6B0);
		auto fnRemoveEffects           = (tRemoveEffects)(baseSrv + 0x100E0);
		auto fnSetWeaponVisible        = (tSetWeaponVisible)(baseSrv + 0xB0D20);
		auto fnHasSuperFlashlight      = (tHasSuperFlashlight)(baseSrv + 0x2BE760);
		auto fnIsFlashlightBoosted     = (tIsFlashlightBoosted)(baseSrv + 0x2BE770);
		auto fnGetFlashlightBattery    = (tGetFlashlightBattery)(baseSrv + 0x2BF350);
		auto fnSendFlashlightAnim      = (tSendFlashlightAnim)(baseSrv + 0x2A0830);
		auto fnOnFlashlightTurnedOff   = (tOnFlashlightTurnedOff)(baseSrv + 0x2A1460);
		auto fnUpdateFlashlightSkin    = (tUpdateFlashlightSkin)(baseSrv + 0x2A14C0);
		auto fnActivateFlashlightBoost = (tActivateFlashlightBoost)(baseSrv + 0x2A3070);

		// 1. Right Hand (vm0): Keep v_camera.mdl loaded and visible even when CINFRAHands is active so the bare Right VR Hand is always visible
		void *activeWeapon = fnGetActiveWeapon(pPlayer);
		uintptr_t handsVtable = baseSrv + 0x5BAA5C;
		bool isEmptyHands = (activeWeapon == nullptr) || (*(uintptr_t*)activeWeapon == handsVtable);
		m_VR->m_RightHandEmpty = isEmptyHands;

		void *vm0 = fnGetServerViewModel(pPlayer, 0);
		if (vm0 && m_VR->m_ShowHands && isEmptyHands)
		{
			EnsureServerBareHandViewModel(activeWeapon, vm0);
		}
		else if (vm0 && !isEmptyHands && activeWeapon)
		{
			auto fnServerWeaponGetViewModel = *(char*(__thiscall**)(void*, int))(*(uintptr_t*)activeWeapon + 0x4F4);
			const char *weaponVmMdl = fnServerWeaponGetViewModel ? fnServerWeaponGetViewModel(activeWeapon, 0) : nullptr;
			const char *vm0Mdl = *(const char**)((uintptr_t)vm0 + 0xAC);
			if (weaponVmMdl && weaponVmMdl[0] != '\0' && (!vm0Mdl || _stricmp(vm0Mdl, weaponVmMdl) != 0))
			{
				auto fnSetWeaponModel = *(void(__thiscall**)(void*, const char*, void*))(*(uintptr_t*)vm0 + 0x3A8);
				if (fnSetWeaponModel)
				{
					fnSetWeaponModel(vm0, weaponVmMdl, activeWeapon);
					fnResetSequenceInfo(vm0);
				}
			}
			const char* curVm0Mdl = *(const char**)((uintptr_t)vm0 + 0xAC);
			if (curVm0Mdl && ContainsCaseInsensitive(curVm0Mdl, "v_camera.mdl"))
			{
				const int curSeq = *(int*)((uintptr_t)vm0 + 0x3C0);
				if (curSeq == 3 || curSeq == 4 || curSeq == 5)
				{
					auto fnSendVmMatchingSeq = *(void(__thiscall**)(void*, int))(*(uintptr_t*)vm0 + 0x3A4);
					if (fnSendVmMatchingSeq)
						fnSendVmMatchingSeq(vm0, 0);
				}
			}
			else if (curVm0Mdl && ContainsCaseInsensitive(curVm0Mdl, "v_phone.mdl"))
			{
				const int curSeq = *(int*)((uintptr_t)vm0 + 0x3C0);
				if (curSeq == 5 || curSeq == 6) // Suppress draw (5) and holster (6); preserve answer(2)/talk_idle(3)/hangup(4)/text(7)
				{
					auto fnSendVmMatchingSeq = *(void(__thiscall**)(void*, int))(*(uintptr_t*)vm0 + 0x3A4);
					if (fnSendVmMatchingSeq)
						fnSendVmMatchingSeq(vm0, 0);
					if (*(uintptr_t*)activeWeapon == (baseSrv + 0x5BE25C)) // CINFRAPhone
					{
						// CINFRAPhone::ItemPostFrame (+0x29D790) requires m_Activity (+0x4FC) == ACT_VM_IDLE (0xB7) to trigger Pause(1, 0)
						*(int*)((uintptr_t)activeWeapon + 0x4FC) = 0xB7;
					}
					*(float*)((uintptr_t)pPlayer + 0x6DC) = 0.0f; // m_flNextAttack
					NotifyServerFieldChanged(baseSrv, pPlayer, 0x6DC);
				}
			}
		}

		// 2. Left Hand (vm1): Use v_phone.mdl when bare so left hand matches right open hand sculpt; switch to v_light/v_superlight when flashlight is equipped
		void *vm1 = fnGetServerViewModel(pPlayer, 1);
		auto ensureServerVm1Model = [&](bool sendIdleAnim)
		{
			if (!vm1 || !m_VR->m_ShowHands)
				return;
			if ((*(uint32_t*)((uintptr_t)vm1 + 0xA8) & 0x20) != 0)
			{
				fnRemoveEffects(vm1, 0x20); // Clear server EF_NODRAW on vm1
			}
			const bool hasSuper = fnHasSuperFlashlight(pPlayer);
			const char *targetVm1Mdl = nullptr;
			if (!m_VR->m_FlashlightEquipped && m_Game->m_ModelInfo &&
			    m_Game->m_ModelInfo->GetModelIndex("models/weapons/v_phone.mdl") != -1)
			{
				targetVm1Mdl = "models/weapons/v_phone.mdl";
			}
			else
			{
				targetVm1Mdl = hasSuper ? "models/weapons/v_superlight.mdl" : "models/weapons/v_light.mdl";
			}
			const char *vm1Mdl = *(const char**)((uintptr_t)vm1 + 0xAC);
			if (!vm1Mdl || _stricmp(vm1Mdl, targetVm1Mdl) != 0 || *(void**)((uintptr_t)vm1 + 0x4A0) == nullptr)
			{
				auto fnSetWeaponModel = *(void(__thiscall**)(void*, const char*, void*))(*(uintptr_t*)vm1 + 0x3A8);
				if (fnSetWeaponModel)
				{
					fnSetWeaponModel(vm1, targetVm1Mdl, nullptr);
					fnResetSequenceInfo(vm1);
					if (sendIdleAnim && _stricmp(targetVm1Mdl, "models/weapons/v_phone.mdl") != 0)
					{
						fnSendFlashlightAnim(pPlayer, 0xB7); // ACT_VM_IDLE
					}
				}
			}
		};
		ensureServerVm1Model(true);

		// 3. Hold-to-Grab release for INFRA's custom CUseController (+0x1840) and CPlayerPickupController (m_hHeldObject +0x189C / m_bUsingHands +0x1668):
		//    If neither Grip is held, stop any active CUseController (valves/doors); if the specific hand holding a physics object
		//    releases its Grip, call CBasePlayer::ForceDropOfCarriedPhysObjects (vtable+0x6E8) immediately.
		const bool docCtrlActive = (*(const uint32_t*)((uintptr_t)pPlayer + 0x1840) != 0xFFFFFFFFu);
		const bool pflagUsing    = !m_VR->m_IsHoldingObject &&
		                           (*(const uint32_t*)((uintptr_t)pPlayer + 0x189C) == 0xFFFFFFFFu) &&
		                           ((*(const uint32_t*)((uintptr_t)pPlayer + 0x0BC4) & 0x04u) != 0u);
		VRGrab::NotifyServerUseControllerActive(docCtrlActive || pflagUsing);
		bool leftGripDown  = VRGrab::EvaluateHandGripInput(m_VR, true,  m_VR->m_IsHoldingObject &&  m_VR->m_HoldingWithLeftHand);
		bool rightGripDown = VRGrab::EvaluateHandGripInput(m_VR, false, m_VR->m_IsHoldingObject && !m_VR->m_HoldingWithLeftHand);
		bool anyGripHeld   = leftGripDown || rightGripDown;
		bool carryGripHeld = VRGrab::EvaluateCarryGripHeld(m_Game, m_VR, pPlayer);
		if (!anyGripHeld)
		{
			if (docCtrlActive)
			{
				typedef void* (__thiscall *tGetInfraUseCtrl)(void *thisptr);
				typedef void  (__thiscall *tStopInfraUseCtrl)(void *thisptr, void *pPlayer);
				auto fnGetInfraUseCtrl  = (tGetInfraUseCtrl)(baseSrv + 0x29EC80);
				auto fnStopInfraUseCtrl = (tStopInfraUseCtrl)(baseSrv + 0x295380);
				void *useCtrl = fnGetInfraUseCtrl(pPlayer);
				if (useCtrl)
				{
					fnStopInfraUseCtrl(useCtrl, pPlayer);
				}
				VRGrab::NotifyServerUseControllerActive(pflagUsing);
			}
		}
		if (!carryGripHeld)
		{
			VRGrab::ReleaseCarriedObject(m_Game, m_VR, nullptr, pPlayer);
		}
		else if (*(uint32_t*)((uintptr_t)pPlayer + 0x189C) != 0xFFFFFFFF ||
		         *(uint8_t*)((uintptr_t)pPlayer + 0x1668) != 0 ||
		         m_VR->m_IsHoldingObject)
		{
			VRGrab::MaintainServerGrabState(m_Game, m_VR, nullptr, pPlayer);
		}

		// 4. Flashlight Equip (Left A / Belt) vs. Beam Toggle (Left Trigger) & Inventory Snapshot / Actions
		// Server CINFRA_Player offsets: m_fEffects = +0xA8, m_bHasFlashlight = +0x1848, m_iFlashlightBattery = +0x184C, m_nFlashlightTargetActivity = +0x1868, m_bFlashlightBroken = +0x18A5
		typedef void* (__thiscall *tGetWeapon)(void *thisptr, int index);
		typedef bool  (__thiscall *tWeaponReload)(void *thisptr);
		typedef void  (__thiscall *tWeaponFinishReload)(void *thisptr);
		typedef int   (__thiscall *tGetAmmoCountByName)(void *thisptr, const char *szName);
		typedef void  (__thiscall *tRemoveAmmoByName)(void *thisptr, int iCount, const char *szName);
		auto fnGetWeapon          = (tGetWeapon)(baseSrv + 0xAA2B0);
		auto fnCameraReload       = (tWeaponReload)(baseSrv + 0x2B71A0);
		auto fnCameraFinishReload = (tWeaponFinishReload)(baseSrv + 0x2B7B70);
		auto fnGetAmmoCount       = (tGetAmmoCountByName)(baseSrv + 0xAA480);
		auto fnRemoveAmmo         = (tRemoveAmmoByName)(baseSrv + 0xAA350);

		const bool hasFlashlightItem = (*(uint8_t*)((uintptr_t)pPlayer + 0x1848) != 0);
		const bool hasSuperLight     = fnHasSuperFlashlight(pPlayer);
		m_VR->m_HasSuperFlashlight   = hasSuperLight;
		if (!hasFlashlightItem)
		{
			m_VR->m_FlashlightEquipped = false;
			m_VR->m_FlashlightBeamOn = false;
			m_VR->m_PendingFlashlightBoost = false;
		}

		uintptr_t cameraVtable = baseSrv + 0x5B67FC;
		void *pCamera = (activeWeapon && *(uintptr_t*)activeWeapon == cameraVtable) ? activeWeapon : nullptr;
		void *pHands  = (activeWeapon && *(uintptr_t*)activeWeapon == handsVtable)  ? activeWeapon : nullptr;
		if (!pCamera || !pHands)
		{
			for (int w = 0; w < 48; ++w)
			{
				void *cand = fnGetWeapon(pPlayer, w);
				if (!cand)
					continue;
				const uintptr_t vt = *(uintptr_t*)cand;
				if (!pCamera && vt == cameraVtable)
					pCamera = cand;
				else if (!pHands && vt == handsVtable)
					pHands = cand;
				if (pCamera && pHands)
					break;
			}
		}

		const VRInventory::PendingActions invAct = VRInventory::ConsumePendingActions();
		if (invAct.equipFlashlight && hasFlashlightItem)
		{
			m_VR->m_FlashlightEquipped = true;
		}
		if (invAct.holsterFlashlight)
		{
			m_VR->m_FlashlightEquipped = false;
			m_VR->m_FlashlightBeamOn = false;
			m_VR->m_PendingFlashlightBoost = false;
		}
		auto fnWeaponSwitch = *(bool(__thiscall**)(void*, void*, int))(*(uintptr_t*)pPlayer + 0x484);
		if (invAct.equipCamera && pCamera && isEmptyHands)
		{
			if (fnWeaponSwitch && fnWeaponSwitch(pPlayer, pCamera, 0))
			{
				*(uint32_t*)((uintptr_t)pPlayer + 0x182C) = 0xFFFFFFFFu;
				*(float*)((uintptr_t)pPlayer + 0x1830) = 0.0f;
				if (fnSetContextThink)
				{
					fnSetContextThink(pCamera, nullptr, 0.0f, "BaseCombatWeapon_HideThink");
					if (pHands)
						fnSetContextThink(pHands, nullptr, 0.0f, "BaseCombatWeapon_HideThink");
				}
				*(float*)((uintptr_t)pPlayer + 0x6DC) = 0.0f; // m_flNextAttack
				NotifyServerFieldChanged(baseSrv, pPlayer, 0x6DC);
				activeWeapon = pCamera;
				isEmptyHands = false;
				m_VR->m_RightHandEmpty = false;
				if (vm0)
				{
					auto fnServerWeaponGetViewModel = *(char*(__thiscall**)(void*, int))(*(uintptr_t*)pCamera + 0x4F4);
					const char *camVmMdl = fnServerWeaponGetViewModel ? fnServerWeaponGetViewModel(pCamera, 0) : nullptr;
					const char *vm0Mdl = *(const char**)((uintptr_t)vm0 + 0xAC);
					if (camVmMdl && camVmMdl[0] != '\0' && (!vm0Mdl || _stricmp(vm0Mdl, camVmMdl) != 0))
					{
						auto fnSetWeaponModel = *(void(__thiscall**)(void*, const char*, void*))(*(uintptr_t*)vm0 + 0x3A8);
						if (fnSetWeaponModel)
						{
							fnSetWeaponModel(vm0, camVmMdl, pCamera);
							fnResetSequenceInfo(vm0);
						}
					}
					const int curSeq = *(int*)((uintptr_t)vm0 + 0x3C0);
					if (curSeq == 3 || curSeq == 4 || curSeq == 5)
					{
						auto fnSendVmMatchingSeq = *(void(__thiscall**)(void*, int))(*(uintptr_t*)vm0 + 0x3A4);
						if (fnSendVmMatchingSeq)
							fnSendVmMatchingSeq(vm0, 0);
					}
				}
			}
			else
			{
				m_Game->ClientCmd_Unrestricted("use infra_camera");
			}
		}
		if (invAct.holsterCamera && pCamera && activeWeapon == pCamera)
		{
			if (pHands && fnWeaponSwitch && fnWeaponSwitch(pPlayer, pHands, 0))
			{
				*(uint32_t*)((uintptr_t)pPlayer + 0x182C) = 0xFFFFFFFFu;
				*(float*)((uintptr_t)pPlayer + 0x1830) = 0.0f;
				if (fnSetContextThink)
				{
					fnSetContextThink(pCamera, nullptr, 0.0f, "BaseCombatWeapon_HideThink");
				}
				fnAddEffects(pCamera, 0x20); // Hide camera entity directly without hiding shared vm0
				*(float*)((uintptr_t)pPlayer + 0x6DC) = 0.0f; // m_flNextAttack
				NotifyServerFieldChanged(baseSrv, pPlayer, 0x6DC);
				activeWeapon = pHands;
				isEmptyHands = true;
				m_VR->m_RightHandEmpty = true;
				if (vm0 && m_VR->m_ShowHands)
				{
					EnsureServerBareHandViewModel(pHands, vm0);
				}
			}
			else
			{
				m_Game->ClientCmd_Unrestricted("use infra_hands");
			}
		}

		const bool doInstantFlashlightReload = invAct.instantReloadFlashlight || m_VR->m_PendingInstantFlashlightReload;
		m_VR->m_PendingInstantFlashlightReload = false;
		if (doInstantFlashlightReload && hasFlashlightItem && !hasSuperLight &&
		    fnGetFlashlightBattery(pPlayer) < 100 && fnGetAmmoCount(pPlayer, "DBattery") >= 100)
		{
			fnRemoveAmmo(pPlayer, 100, "DBattery");
			*(int*)((uintptr_t)pPlayer + 0x184C) = 100;
			*(uint8_t*)((uintptr_t)pPlayer + 0x18A5) = 0;
			NotifyServerFieldChanged(baseSrv, pPlayer, 0x184C);
			fnUpdateFlashlightSkin(pPlayer);
		}

		const bool doInstantCameraReload = invAct.instantReloadCamera || m_VR->m_PendingInstantCameraReload;
		m_VR->m_PendingInstantCameraReload = false;
		if (doInstantCameraReload && pCamera && *(int*)((uintptr_t)pCamera + 0x4D8) < 100 &&
		    fnGetAmmoCount(pPlayer, "AABattery") >= 100)
		{
			fnRemoveAmmo(pPlayer, 100, "AABattery");
			*(int*)((uintptr_t)pCamera + 0x4D8) = 100;
			*(uint8_t*)((uintptr_t)pCamera + 0x4D1) = 0;
			NotifyServerFieldChanged(baseSrv, pCamera, 0x4D8);
		}

		if (invAct.holsterFlashlight || !hasFlashlightItem)
		{
			ensureServerVm1Model(true);
		}

		static bool s_WasFlashlightEquipped = false;
		if (m_VR->m_FlashlightEquipped && !s_WasFlashlightEquipped)
		{
			ensureServerVm1Model(false);
			*(int*)((uintptr_t)pPlayer + 0x1868) = 0xB7;
			*(int*)((uintptr_t)pPlayer + 0x186C) = 0xB7;
			fnSendFlashlightAnim(pPlayer, 0xB7); // ACT_VM_IDLE (suppress draw animation)
			fnUpdateFlashlightSkin(pPlayer);
		}
		s_WasFlashlightEquipped = m_VR->m_FlashlightEquipped;

		if (m_VR->m_PendingCameraReload)
		{
			m_VR->m_PendingCameraReload = false;
			if (pCamera)
			{
				uint32_t origEffects = *(uint32_t*)((uintptr_t)pPlayer + 0xA8);
				*(uint32_t*)((uintptr_t)pPlayer + 0xA8) = origEffects & ~4u; // Temporarily clear EF_DIMLIGHT so CBaseInfraWeapon::Reload never aborts
				bool startedCamReload = fnCameraReload(pCamera);
				if (pCamera != activeWeapon || (!startedCamReload && *(int*)((uintptr_t)pCamera + 0x4D8) < 100))
				{
					fnCameraFinishReload(pCamera);
					*(uint8_t*)((uintptr_t)pCamera + 0x4D1) = 0; // Clear m_bInReload if holstered
				}
				*(uint32_t*)((uintptr_t)pPlayer + 0xA8) = origEffects;
			}
		}

		static auto s_FlashlightReloadStartTime = std::chrono::steady_clock::now();
		if (m_VR->m_PendingFlashlightReload)
		{
			m_VR->m_PendingFlashlightReload = false;
			if (hasFlashlightItem && hkReloadFlashlight.fOriginal && !fnHasSuperFlashlight(pPlayer) && fnGetFlashlightBattery(pPlayer) < 100)
			{
				if (!m_VR->m_FlashlightEquipped)
				{
					m_VR->m_FlashlightEquipped = true;
					s_WasFlashlightEquipped = true;
				}
				ensureServerVm1Model(false);
				*(uint8_t*)((uintptr_t)pPlayer + 0x18A5) = 0; // Clear m_bFlashlightBroken
				m_VR->m_AllowFlashlightReload = true;
				hkReloadFlashlight.fOriginal(pPlayer); // Starts ACT_VM_RELOAD (0xC0) on vm1
				m_VR->m_AllowFlashlightReload = false;
				s_FlashlightReloadStartTime = std::chrono::steady_clock::now();

				// SendFlashlightAnim(0xC0) sets +0x186C = 0xC0 and +0x1868 = 2 if a transition sequence exists,
				// which prevented ReloadFlashlight() (+0x1868 == 0xC0 check) from ever completing the reload!
				// Force +0x1868 = 0xC0 and play the target reload sequence (+0x1870) directly on vm1.
				if (*(int*)((uintptr_t)pPlayer + 0x186C) == 0xC0)
				{
					*(int*)((uintptr_t)pPlayer + 0x1868) = 0xC0;
					int reloadSeq = *(int*)((uintptr_t)pPlayer + 0x1870);
					if (vm1 && reloadSeq >= 0)
					{
						auto fnSendVmMatchingSeq = *(void(__thiscall**)(void*, int))(*(uintptr_t*)vm1 + 0x3A4);
						if (fnSendVmMatchingSeq)
						{
							fnSendVmMatchingSeq(vm1, reloadSeq);
						}
					}
				}
			}
		}

		int currentFlActivity = *(int*)((uintptr_t)pPlayer + 0x1868);
		int targetFlActivity  = *(int*)((uintptr_t)pPlayer + 0x186C);
		if ((currentFlActivity == 0xC0 || targetFlActivity == 0xC0) && hkReloadFlashlight.fOriginal)
		{
			*(int*)((uintptr_t)pPlayer + 0x1868) = 0xC0;
			float reloadElapsedSec = std::chrono::duration<float>(std::chrono::steady_clock::now() - s_FlashlightReloadStartTime).count();
			if (vm1 && reloadElapsedSec > 2.2f)
			{
				// Guarantee reload completion even if vm1 sequence completion flag (+0x3B4) was reset
				*(uint8_t*)((uintptr_t)vm1 + 0x3B4) = 1;
			}
			// Continue driving ReloadFlashlight() while ACT_VM_RELOAD (0xC0) is playing, even if CINFRAHands is not the active weapon
			m_VR->m_AllowFlashlightReload = true;
			bool finishedReload = hkReloadFlashlight.fOriginal(pPlayer);
			m_VR->m_AllowFlashlightReload = false;
			if (finishedReload || reloadElapsedSec > 3.0f)
			{
				*(int*)((uintptr_t)pPlayer + 0x1868) = 0xB7;
				*(int*)((uintptr_t)pPlayer + 0x186C) = 0xB7;
				fnSendFlashlightAnim(pPlayer, 0xB7); // Return to ACT_VM_IDLE
				fnUpdateFlashlightSkin(pPlayer);
			}
		}
		else
		{
			if (vm1 && m_VR->m_ShowHands && m_VR->m_FlashlightEquipped)
			{
				const char* curVm1Mdl = *(const char**)((uintptr_t)vm1 + 0xAC);
				const int vm1Seq = *(int*)((uintptr_t)vm1 + 0x3C0);
				if (curVm1Mdl &&
				    (ContainsCaseInsensitive(curVm1Mdl, "v_light.mdl") ||
				     ContainsCaseInsensitive(curVm1Mdl, "v_superlight.mdl")) &&
				    (vm1Seq == 1 || vm1Seq == 2 || vm1Seq == 3 ||
				     currentFlActivity == 0xB6 || targetFlActivity == 0xB6))
				{
					*(int*)((uintptr_t)pPlayer + 0x1868) = 0xB7;
					*(int*)((uintptr_t)pPlayer + 0x186C) = 0xB7;
					auto fnSendVmMatchingSeq = *(void(__thiscall**)(void*, int))(*(uintptr_t*)vm1 + 0x3A4);
					if (fnSendVmMatchingSeq)
						fnSendVmMatchingSeq(vm1, 0);
				}
			}

			int battery = fnGetFlashlightBattery(pPlayer);
			if (battery <= 0)
			{
				m_VR->m_FlashlightBeamOn = false;
			}

			bool isDimLightOn = (*(uint32_t*)((uintptr_t)pPlayer + 0xA8) & 4) != 0;
			bool wantBeamOn = hasFlashlightItem && m_VR->m_FlashlightEquipped && m_VR->m_FlashlightBeamOn && (battery > 0);

			if (wantBeamOn && !isDimLightOn)
			{
				*(uint8_t*)((uintptr_t)pPlayer + 0x18A5) = 0;
				fnAddEffects(pPlayer, 4);
				fnUpdateFlashlightSkin(pPlayer);
			}
			else if (!wantBeamOn && isDimLightOn)
			{
				fnRemoveEffects(pPlayer, 4);
				fnOnFlashlightTurnedOff(pPlayer);
				fnUpdateFlashlightSkin(pPlayer);
			}

			if (m_VR->m_PendingFlashlightBoost)
			{
				m_VR->m_PendingFlashlightBoost = false;
				if (hasFlashlightItem && hasSuperLight && m_VR->m_FlashlightEquipped && m_VR->m_FlashlightBeamOn)
				{
					const uint8_t* pAct = reinterpret_cast<const uint8_t*>(baseSrv + 0x2A3070);
					const uint8_t* pIsB = reinterpret_cast<const uint8_t*>(baseSrv + 0x2BE770);
					if (pAct[0] == 0x55 && pAct[1] == 0x8B && pAct[2] == 0xEC &&
					    pIsB[0] == 0x55 && pIsB[1] == 0x8B && pIsB[2] == 0xEC)
					{
						const bool wasBoosted = fnIsFlashlightBoosted(pPlayer);
						fnActivateFlashlightBoost(pPlayer, false);
						const bool nowBoosted = fnIsFlashlightBoosted(pPlayer);
						const bool flPhysLeft = !m_VR->m_LeftHanded;
						if (!wasBoosted && nowBoosted)
							m_VR->TriggerHapticPulseOnHand(flPhysLeft, 2800, 0.90f);
						else
							m_VR->TriggerHapticPulseOnHand(flPhysLeft, 500, 0.20f);
					}
				}
			}
		}

		VRInventory::ServerSnapshot snap{};
		snap.valid              = true;
		snap.hasFlashlight      = hasFlashlightItem;
		snap.hasSuperFlashlight = hasSuperLight;
		snap.hasCamera          = (pCamera != nullptr);
		snap.flashlightBroken   = (*(uint8_t*)((uintptr_t)pPlayer + 0x18A5) != 0);
		snap.flashlightBattery  = fnGetFlashlightBattery(pPlayer);
		snap.cameraBattery      = pCamera ? *(int*)((uintptr_t)pCamera + 0x4D8) : 0;
		snap.dBatteryReserve    = std::max(0, fnGetAmmoCount(pPlayer, "DBattery") / 100);
		snap.aaBatteryReserve   = std::max(0, fnGetAmmoCount(pPlayer, "AABattery") / 100);
		VRInventory::PublishServerSnapshot(snap);

		if (!paused)
		{
			VRGrab::UpdateOpenHandPhysics(m_Game, m_VR, pPlayer);
		}
	}
	else if (!m_VR || !m_VR->m_IsVREnabled || !pPlayer || !m_Game->m_BaseServer ||
	         m_VR->m_In3DViewControlMenu || m_Game->m_EngineClient->IsLevelMainMenuBackground() ||
	         m_Game->m_EngineClient->GetLocalPlayer() <= 0)
	{
		s_PrevServerPlayer = nullptr;
		s_LastUseServerEnt = nullptr;
		VRGrab::ResetOpenHandPhysics();
		VRInventory::Reset();
		VRCameraFlash::Reset();
		ResetCameraScreenCache();
		VRHands::ResetFlashlightGripAnim();
	}

	return ret;
}

int Hooks::dWriteUsercmd(bf_write *buf, CUserCmd *to, CUserCmd *from)
{
	auto result =  hkWriteUsercmd.fOriginal(buf, to, from);

	// Let's write our stuff into the buffer
	if (buf && !buf->IsOverflowed() && m_VR && m_VR->m_IsVREnabled)
	{
		if (buf->GetNumBitsLeft() >= 160)
		{
			Vector controllerPos = m_VR->GetRightControllerAbsPos();
			QAngle controllerAngles = m_VR->GetRightControllerAbsAngle();

			buf->WriteChar(-2);
			buf->WriteBitVec3Coord(controllerPos);
			buf->WriteBitAngles(controllerAngles);
		}
	}

	return result;
}

int Hooks::dReadUsercmd(bf_read *buf, CUserCmd* move, CUserCmd* from)
{
	auto result = hkReadUsercmd.fOriginal(buf, move, from);

	if (!buf || buf->IsOverflowed())
		return result;

	int i = m_Game->m_CurrentUsercmdID;
	if (i < 0 || i >= (int)m_Game->m_PlayersVRInfo.size())
		return result;
	auto& vrPlayer = m_Game->m_PlayersVRInfo[i];

	if (buf->GetNumBitsLeft() < 8)
	{
		vrPlayer.isUsingVR = false;
		return result;
	}

	auto pos = buf->Tell();
	int res = buf->ReadChar();

	// This means we got a VR player on the other side
	if (res == -2 && !buf->IsOverflowed() && buf->GetNumBitsLeft() >= 64)
	{
		vrPlayer.isUsingVR = true;
		buf->ReadBitVec3Coord(vrPlayer.controllerPos);
		buf->ReadBitAngles(vrPlayer.controllerAngle);
	}
	else {
		vrPlayer.isUsingVR = false;
		buf->Seek(pos);
	}

	return result;
}

Vector *Hooks::dEyePosition(void *ecx, void *edx, Vector *eyePos)
{
	Vector *result = hkEyePosition.fOriginal(ecx, eyePos);
	if (m_VR->m_OverrideEyeAngles)
	{
		int localIndex = m_Game->m_EngineClient->GetLocalPlayer();
		int index = EntityIndex ? EntityIndex(ecx) : -1;

		if (m_VR->m_IsVREnabled && localIndex == index)
		{
			if (m_VR->m_InPickupUpdate)
			{
				*result = VRGrab::GetPickupShootPosition(m_VR);
			}
			else if (s_HasCustomUseAngle)
			{
				bool physicalLeft = m_VR->m_LeftHanded ? !m_VR->m_UseWithLeftHand : m_VR->m_UseWithLeftHand;
				*result = physicalLeft ? m_VR->GetLeftPointerAbsPos() : m_VR->GetRightPointerAbsPos();
			}
			else
			{
				bool useLeft = m_VR->m_LeftHanded ? !m_VR->m_UseWithLeftHand : m_VR->m_UseWithLeftHand;
				*result = useLeft ? m_VR->GetLeftControllerAbsPos() : m_VR->GetRightControllerAbsPos();
			}
		}
		else if (index >= 0 && index < (int)m_Game->m_PlayersVRInfo.size() && m_Game->m_PlayersVRInfo[index].isUsingVR)
		{
			*result = m_Game->m_PlayersVRInfo[index].controllerPos;
		}
	}
	return result;
}

void Hooks::dDrawModelExecute(void *ecx, void *edx, void *ctx, void *state, const ModelRenderInfo_t &info, void *pCustomBoneToWorld)
{
	const char* modelName = (info.pModel && m_Game && m_Game->m_ModelInfo)
		? m_Game->m_ModelInfo->GetModelName(const_cast<model_t*>(info.pModel))
		: nullptr;
	const bool drawInvNow = VRInventory::ShouldDrawWorldModelsNow(modelName, info);

	VRHands::ViewModelLensInfo lensInfo{};
	if (pCustomBoneToWorld && VRHands::ApplyViewModelHandRig(
	        m_VR, m_Game, state, info, static_cast<matrix3x4_t*>(pCustomBoneToWorld), nullptr, &lensInfo))
	{
		void(__thiscall* fnFlipCullMode)(void*) = nullptr;
		if (lensInfo.flipCull && ctx)
		{
			void** ctxVt = *reinterpret_cast<void***>(ctx);
			if (ctxVt && ctxVt[41])
				fnFlipCullMode = reinterpret_cast<void(__thiscall*)(void*)>(ctxVt[41]);
		}
		if (fnFlipCullMode)
			fnFlipCullMode(ctx);
		hkDrawModelExecute.fOriginal(ecx, ctx, state, info, pCustomBoneToWorld);
		if (fnFlipCullMode)
			fnFlipCullMode(ctx);
		DrawHandInteriorBlackPass(ecx, ctx, state, info, pCustomBoneToWorld, m_Game, m_VR, fnFlipCullMode != nullptr);
		if (lensInfo.updated)
		{
			s_LensPos = lensInfo.pos;
			s_LensNormal = lensInfo.normal;
			s_LensFrame = s_RenderFrameId;
		}
		if (drawInvNow)
			VRInventory::DrawWorldModelsInDME(ecx, ctx, state, info, static_cast<matrix3x4_t*>(pCustomBoneToWorld));
		return;
	}

	hkDrawModelExecute.fOriginal(ecx, ctx, state, info, pCustomBoneToWorld);
	if (drawInvNow)
		VRInventory::DrawWorldModelsInDME(ecx, ctx, state, info, static_cast<matrix3x4_t*>(pCustomBoneToWorld));
}

Vector* Hooks::dWeapon_ShootPosition(void* ecx, void* edx, Vector* eyePos)
{
	Vector* result = hkWeapon_ShootPosition.fOriginal(ecx, eyePos);

	int localIndex = m_Game->m_EngineClient->GetLocalPlayer();
	int index = EntityIndex ? EntityIndex(ecx) : -1;

	if (m_VR->m_IsVREnabled && localIndex == index) {
		if (m_VR->m_InPickupUpdate) {
			*result = VRGrab::GetPickupShootPosition(m_VR);
		} else if (s_HasCustomUseAngle) {
			bool physicalLeft = m_VR->m_LeftHanded ? !m_VR->m_UseWithLeftHand : m_VR->m_UseWithLeftHand;
			*result = physicalLeft ? m_VR->GetLeftPointerAbsPos() : m_VR->GetRightPointerAbsPos();
		} else if (m_VR->m_OverrideEyeAngles) {
			bool useLeft = m_VR->m_LeftHanded ? !m_VR->m_UseWithLeftHand : m_VR->m_UseWithLeftHand;
			*result = useLeft ? m_VR->GetLeftControllerAbsPos() : m_VR->GetRightControllerAbsPos();
		} else {
			*result = m_VR->m_LeftHanded ? m_VR->GetLeftControllerAbsPos() : m_VR->GetRightControllerAbsPos();
		}
	}
	else if (index >= 0 && index < (int)m_Game->m_PlayersVRInfo.size() && m_Game->m_PlayersVRInfo[index].isUsingVR)
	{
		*result = m_Game->m_PlayersVRInfo[index].controllerPos;
	}

	return result;
}

bool __fastcall Hooks::dUpdateObject(void* ecx, void* edx, void* pPlayer, float flError, bool bIsTeleport) {
	VRGrabUpdateScope scope{};
	float effectiveError = flError;
	if (!VRGrab::BeginPickupUpdate(m_Game, m_VR, ecx, pPlayer, scope, effectiveError))
		return false;

	bool value = hkUpdateObject.fOriginal(ecx, pPlayer, effectiveError, bIsTeleport);
	if (value && ecx && m_VR && m_VR->m_IsVREnabled && !VRGrab::WasSetTargetPositionCalledInScope())
		dGrabControllerSetTargetPosition(ecx, nullptr, scope.preTargetPos, scope.preTargetAng, bIsTeleport);

	VRGrab::EndPickupUpdate(m_Game, m_VR, value, false, scope);
	return value;
}

bool __fastcall Hooks::dUpdateObjectVM(void* ecx, void* edx, void* pPlayer, float flError) {
	VRGrabUpdateScope scope{};
	float effectiveError = flError;
	if (!VRGrab::BeginPickupUpdate(m_Game, m_VR, ecx, pPlayer, scope, effectiveError))
		return false;

	bool value = hkUpdateObjectVM.fOriginal(ecx, pPlayer, effectiveError);
	if (value && ecx && m_VR && m_VR->m_IsVREnabled && !VRGrab::WasSetTargetPositionCalledInScope())
		dGrabControllerSetTargetPosition(ecx, nullptr, scope.preTargetPos, scope.preTargetAng, false);

	VRGrab::EndPickupUpdate(m_Game, m_VR, value, true, scope);
	return value;
}

QAngle& __fastcall Hooks::dEyeAngles(void* ecx, void* edx) {
	if (m_VR->m_OverrideEyeAngles) {
		int localIndex = m_Game->m_EngineClient->GetLocalPlayer();
		int index = EntityIndex ? EntityIndex(ecx) : -1;

		if (m_VR->m_IsVREnabled && localIndex == index) {
			if (s_HasCustomUseAngle && !m_VR->m_InPickupUpdate) {
				return s_CustomUseAngle;
			}
			if (m_VR->m_InPickupUpdate) {
				return VRGrab::GetPickupEyeAngles(m_VR);
			}
			bool useLeft = m_VR->m_LeftHanded ? !m_VR->m_UseWithLeftHand : m_VR->m_UseWithLeftHand;
			const QAngle& ctrlAng = useLeft ? m_VR->GetLeftControllerAbsAngleConst() : m_VR->GetRightControllerAbsAngleConst();
			return const_cast<QAngle&>(ctrlAng);
		}
		else if (index >= 0 && index < (int)m_Game->m_PlayersVRInfo.size() && m_Game->m_PlayersVRInfo[index].isUsingVR)
		{
			return m_Game->m_PlayersVRInfo[index].controllerAngle;
		}
	}

	return hkEyeAngles.fOriginal(ecx);
}

double __fastcall Hooks::dGetDefaultFOV(void* ecx, void* edx) {
	if (m_VR->m_In3DViewControlMenu)
		return m_VR->m_MenuViewFov;
	return m_VR->m_Fov;
}

double __fastcall Hooks::dGetFOV(void* ecx, void* edx) {
	if (m_VR->m_In3DViewControlMenu)
		return m_VR->m_MenuViewFov;
	return m_VR->m_Fov;
}

double __fastcall Hooks::dGetViewModelFOV(void* ecx, void* edx) {
	return m_VR->m_Fov;
}

void __fastcall Hooks::dUpdateFlashlight(void* ecx, void* edx) {
	bool wasTrue = m_VR->m_OverrideFlashlightEyeAngles;
	m_VR->m_OverrideFlashlightEyeAngles = true;
	hkUpdateFlashlight.fOriginal(ecx);
	if (!wasTrue)
		m_VR->m_OverrideFlashlightEyeAngles = false;
}

QAngle& __fastcall Hooks::dClientEyeAngles(void* ecx, void* edx) {
	if (m_VR->m_IsVREnabled) {
		int playerIndex = m_Game->m_EngineClient->GetLocalPlayer();
		C_BasePlayer* localPlayer = (C_BasePlayer*)m_Game->GetClientEntity(playerIndex);
		if (ecx == localPlayer) {
			if (m_VR->m_OverrideFlashlightEyeAngles) {
				if (VRHands::g_ViewModelLens.updated && VectorLength(VRHands::g_ViewModelLens.normal) > 0.01f) {
					static QAngle s_FlashlightBeamAng;
					s_FlashlightBeamAng = DirToQAngle(VRHands::g_ViewModelLens.normal);
					return s_FlashlightBeamAng;
				}
				return m_VR->m_LeftHanded ? m_VR->GetRightControllerAbsAngleConst() : m_VR->GetLeftControllerAbsAngleConst();
			}
			if (m_VR->m_OverrideClientUseEye) {
				if (s_HasCustomUseAngle) {
					return s_CustomUseAngle;
				}
				bool useLeft = m_VR->m_LeftHanded ? !m_VR->m_UseWithLeftHand : m_VR->m_UseWithLeftHand;
				return useLeft ? m_VR->GetLeftControllerAbsAngleConst() : m_VR->GetRightControllerAbsAngleConst();
			}
			if (m_VR->m_OverrideClientEyeAngles) {
				return m_VR->m_LeftHanded ? m_VR->GetLeftControllerAbsAngleConst() : m_VR->GetRightControllerAbsAngleConst();
			}
			if (m_VR->m_In3DViewControlMenu) {
				return m_VR->m_MenuViewAngles;
			}
		}
	}
	return hkClientEyeAngles.fOriginal(ecx);
}

Vector* __fastcall Hooks::dClientEyePosition(void* ecx, void* edx, Vector* eyePos) {
	Vector* result = hkClientEyePosition.fOriginal(ecx, eyePos);
	if (m_VR->m_IsVREnabled) {
		int playerIndex = m_Game->m_EngineClient->GetLocalPlayer();
		C_BasePlayer* localPlayer = (C_BasePlayer*)m_Game->GetClientEntity(playerIndex);
		if (ecx == localPlayer) {
			if (m_VR->m_OverrideFlashlightEyeAngles) {
				if (VRHands::g_ViewModelLens.updated) {
					*result = VRHands::g_ViewModelLens.pos;
				} else {
					*result = m_VR->m_LeftHanded ? m_VR->GetRightControllerAbsPos() : m_VR->GetLeftControllerAbsPos();
				}
			} else if (m_VR->m_OverrideClientUseEye) {
				bool useLeft = m_VR->m_LeftHanded ? !m_VR->m_UseWithLeftHand : m_VR->m_UseWithLeftHand;
				*result = useLeft ? m_VR->GetLeftControllerAbsPos() : m_VR->GetRightControllerAbsPos();
			} else if (m_VR->m_OverrideClientEyeAngles) {
				*result = m_VR->m_LeftHanded ? m_VR->GetLeftControllerAbsPos() : m_VR->GetRightControllerAbsPos();
			} else if (m_VR->m_In3DViewControlMenu) {
				*result = m_VR->m_MenuViewOrigin;
			}
		}
	}
	return result;
}

void __fastcall Hooks::dClientCameraThink(void* ecx, void* edx) {
	if (m_VR && m_VR->m_IsVREnabled && ecx && !m_VR->m_RightHandEmpty && !m_VR->m_PhoneEquipped) {
		// ecx is IClientThinkable* (C_INFRACamera + 0x0C); +0xF02 is m_bScreenOn, +0xF03 is pending photo flash flag.
		void* pCam = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(ecx) - 0x0C);
		*(uint8_t*)((uintptr_t)pCam + 0xF02) = 1;

		uint8_t* pPhotoPending = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(pCam) + 0xF03);
		*pPhotoPending = 0;

		const auto now = std::chrono::steady_clock::now();
		const uint32_t curSeq = s_ServerPhotoTakenSeq.load(std::memory_order_acquire);
		const bool serverJustShot = (curSeq != s_ConsumedPhotoTakenSeq);
		if (serverJustShot)
		{
			s_ConsumedPhotoTakenSeq = curSeq;
			s_PendingFlashFired = true;
			s_LastPhotoCam = pCam;
			s_PendingFlashFireTime = now;
			s_LastCameraPhotoTime = now;
			if (hkCameraOnPhotoTaken.fOriginal)
				hkCameraOnPhotoTaken.fOriginal(pCam);
			VRCameraFlash::OnPhotoTaken(pCam);
		}
	}
	bool wasTrue = m_VR->m_OverrideClientEyeAngles;
	m_VR->m_OverrideClientEyeAngles = true;
	hkClientCameraThink.fOriginal(ecx);
	if (!wasTrue)
		m_VR->m_OverrideClientEyeAngles = false;
}

bool __cdecl Hooks::dShouldUsePhonePause() {
	// client.dll + 0x1BDB30 is CINFRAMainMenu::CanPlayerPause().
	// Returning false here caused CINFRAMainMenu::OnTick (0x101BDCCE) to immediately call "gameui_hide"!
	return hkShouldUsePhonePause.fOriginal();
}

// Computes the hand-based use target on the client (see "virtual fingertip" notes at the top of this file).
static HandUseSelection ComputeHandUseSelection(void* localPlayer)
{
	VR* vr = Hooks::m_VR;
	Game* game = Hooks::m_Game;
	HandUseSelection out;
	out.time = std::chrono::steady_clock::now();
	out.state = HandSelState::None;
	if (!vr || !game || !game->m_BaseClient || !game->m_ClientEntityList || !game->m_EngineTrace || !localPlayer)
		return out;

	typedef bool (__thiscall* tIsUseCandidateValid)(void* player, void* ent);
	typedef void (__thiscall* tCalcNearestPoint)(void* collisionProp, const Vector& worldPt, Vector* nearestOut);
	auto fnIsUseCandidateValid = (tIsUseCandidateValid)(game->m_BaseClient + 0x1C8AF0);
	auto fnCalcNearestPoint    = (tCalcNearestPoint)(game->m_BaseClient + 0xB7990);

	const float unitsPerCm  = vr->m_VRScale * 0.01f;
	const float tipDist     = std::clamp(vr->m_UseTipDistanceCm, 0.0f, 30.0f) * unitsPerCm;
	const float touchRadius = std::clamp(vr->m_UseTouchRadiusCm, 3.0f, 40.0f) * unitsPerCm;
	const float farRange    = 190.0f * unitsPerCm;   // ~1.9 m pointing reach
	const float farConeTan  = 0.1405f;               // tan(8 deg)
	const float farMinTol   = 3.0f * unitsPerCm;
	const float hysteresis  = 2.0f * unitsPerCm;
	const float prefilter   = farRange + 96.0f;      // generous: entity centers of big doors are far from the handle

	struct HandCtx {
		bool logicalLeft;
		bool physicalLeft;
		Vector pos;
		Vector fwd;
		Vector tip;
		Vector palmPos;
		Vector pinchPos;
		Vector pointDir;
	};
	HandCtx hands[2];
	int numHands = 0;
	auto addHand = [&](bool logicalLeft)
	{
		bool physicalLeft = vr->m_LeftHanded ? !logicalLeft : logicalLeft;
		Vector pos = physicalLeft ? vr->GetLeftPointerAbsPos() : vr->GetRightPointerAbsPos();
		Vector fwd = physicalLeft ? vr->m_LeftPointerFwd : vr->m_RightPointerFwd;
		float fwdLen = VectorLength(fwd);
		if (fwdLen > 0.0001f)
			fwd = fwd * (1.0f / fwdLen);
		Vector palmPos = pos, pinchPos = pos + fwd * tipDist, pointDir = fwd;
		VRGrab::GetCalibratedHandSelectPose(vr, physicalLeft, vr->m_SetupOrigin, palmPos, pinchPos, pointDir);
		hands[numHands++] = { logicalLeft, physicalLeft, pos, fwd, pos + fwd * tipDist, palmPos, pinchPos, pointDir };
	};
	addHand(false);
	addHand(true);

	auto hasLineOfSight = [&](const Vector& from, const Vector& to) -> bool
	{
		Vector d = to - from;
		float len = VectorLength(d);
		if (len < 1.0f)
			return true;
		CTraceFilterWorldOnlyUse filter;
		CGameTrace tr;
		Ray_t ray;
		ray.Init(from, to);
		game->m_EngineTrace->TraceRay(ray, CONTENTS_SOLID, &filter, &tr);
		if (tr.startsolid)
			return true;
		return (1.0f - tr.fraction) * len <= 1.5f;
	};

	void* prevEnt  = (s_HandSel.state == HandSelState::Valid) ? s_HandSel.clientEnt : nullptr;
	bool  prevLeft = s_HandSel.leftHand;

	struct HandBestBucket
	{
		void* closeEnt = nullptr; int closeIdx = -1; float closeScore = FLT_MAX; float prevCloseScore = FLT_MAX;
		void* farEnt   = nullptr; int farIdx   = -1; float farScore   = FLT_MAX; float prevFarScore   = FLT_MAX;
	};
	HandBestBucket perHand[2]{};
	bool physHasClose[2]{ false, false }; // [0]=physRight, [1]=physLeft

	int highest = game->m_ClientEntityList->GetHighestEntityIndex();
	for (int i = 1; i <= highest; ++i)
	{
		void* ent = game->GetClientEntity(i);
		if (!ent || ent == localPlayer)
			continue;

		auto fnWorldSpaceCenter = *(const Vector&(__thiscall**)(void*))(*(uintptr_t*)ent + 0x130);
		if (!fnWorldSpaceCenter)
			continue;
		Vector center = fnWorldSpaceCenter(ent);

		bool nearAnyHand = false;
		for (int h = 0; h < numHands; ++h)
		{
			Vector dc = center - hands[h].pos;
			if (Dot3(dc, dc) < prefilter * prefilter) { nearAnyHand = true; break; }
		}
		if (!nearAnyHand)
			continue;

		if (!fnIsUseCandidateValid(localPlayer, ent))
			continue;

		void* collisionProp = (void*)((uintptr_t)ent + 0x410);
		for (int h = 0; h < numHands; ++h)
		{
			const HandCtx& hand = hands[h];
			HandBestBucket& hb = perHand[h];
			const bool isPrev = (ent == prevEnt && hand.logicalLeft == prevLeft);

			Vector nearestPalm, nearestPinch, nearestTip;
			fnCalcNearestPoint(collisionProp, hand.palmPos,  &nearestPalm);
			fnCalcNearestPoint(collisionProp, hand.pinchPos, &nearestPinch);
			fnCalcNearestPoint(collisionProp, hand.tip,      &nearestTip);

			float dPalm  = VectorLength(nearestPalm  - hand.palmPos);
			float dPinch = VectorLength(nearestPinch - hand.pinchPos);
			float dTip   = VectorLength(nearestTip   - hand.tip);

			Vector nearest = nearestPinch;
			Vector losOrigin = hand.pinchPos;
			float closeDist = dPinch;
			if (dPalm < closeDist) { closeDist = dPalm; nearest = nearestPalm; losOrigin = hand.palmPos; }
			if (dTip  < closeDist) { closeDist = dTip;  nearest = nearestTip;  losOrigin = hand.pos; }

			if (closeDist <= touchRadius)
			{
				if (hasLineOfSight(losOrigin, nearest))
				{
					physHasClose[hand.physicalLeft ? 1 : 0] = true;
					float score = closeDist + 0.05f * VectorLength(center - hand.pinchPos);
					if (isPrev)
						hb.prevCloseScore = std::min(hb.prevCloseScore, score);
					if (score < hb.closeScore)
					{
						hb.closeScore = score;
						hb.closeEnt = ent;
						hb.closeIdx = i;
					}
				}
			}
			else
			{
				float bestRayScore = FLT_MAX;
				Vector rayNearest{}, rayOrigin{};
				auto evalRay = [&](const Vector& rOrig, const Vector& rDir)
				{
					float t = std::clamp(Dot3(center - rOrig, rDir), 0.0f, farRange);
					Vector rayPt = rOrig + rDir * t;
					Vector candNearest;
					fnCalcNearestPoint(collisionProp, rayPt, &candNearest);
					float along = Dot3(candNearest - rOrig, rDir);
					if (along <= 0.0f || along > farRange)
						return;
					Vector onRay = rOrig + rDir * along;
					float perp = VectorLength(candNearest - onRay);
					if (perp > std::max(farMinTol, along * farConeTan))
						return;
					float candScore = touchRadius + 1.0f + perp + 0.05f * along;
					if (candScore < bestRayScore)
					{
						bestRayScore = candScore;
						rayNearest = candNearest;
						rayOrigin = rOrig;
					}
				};
				evalRay(hand.pos,      hand.fwd);
				evalRay(hand.pinchPos, hand.pointDir);
				if (bestRayScore < FLT_MAX && (bestRayScore < hb.farScore || isPrev) && hasLineOfSight(rayOrigin, rayNearest))
				{
					if (isPrev)
						hb.prevFarScore = std::min(hb.prevFarScore, bestRayScore);
					if (bestRayScore < hb.farScore)
					{
						hb.farScore = bestRayScore;
						hb.farEnt = ent;
						hb.farIdx = i;
					}
				}
			}
		}
	}

	VRGrab::NotifyHandCloseTargetProximity(false, physHasClose[0]);
	VRGrab::NotifyHandCloseTargetProximity(true,  physHasClose[1]);

	const bool leftDig     = vr->PressedDigitalAction(vr->m_ActionCrouch);
	const bool rightDig    = vr->PressedDigitalAction(vr->m_ActionReload);
	const bool leftActive  = VRGrab::EvaluateHandGripInput(vr, true,  vr->m_IsHoldingObject &&  vr->m_HoldingWithLeftHand);
	const bool rightActive = VRGrab::EvaluateHandGripInput(vr, false, vr->m_IsHoldingObject && !vr->m_HoldingWithLeftHand);
	const bool anyActive   = leftActive || rightActive;

	void* bestEnt = nullptr;
	int   bestIdx = -1;
	bool  bestLeft = false;
	float bestScore = FLT_MAX;
	float prevScore = FLT_MAX;

	for (int h = 0; h < numHands; ++h)
	{
		const HandCtx& hand = hands[h];
		const HandBestBucket& hb = perHand[h];
		const bool handActive = hand.physicalLeft ? leftActive : rightActive;
		const bool handDig    = hand.physicalLeft ? leftDig    : rightDig;
		if (anyActive && !handActive)
			continue;

		prevScore = std::min(prevScore, hb.prevCloseScore);
		if (hb.closeEnt && hb.closeScore < bestScore)
		{
			bestScore = hb.closeScore;
			bestEnt   = hb.closeEnt;
			bestIdx   = hb.closeIdx;
			bestLeft  = hand.logicalLeft;
		}

		if (!anyActive || handDig)
		{
			prevScore = std::min(prevScore, hb.prevFarScore);
			if (hb.farEnt && hb.farScore < bestScore)
			{
				bestScore = hb.farScore;
				bestEnt   = hb.farEnt;
				bestIdx   = hb.farIdx;
				bestLeft  = hand.logicalLeft;
			}
		}
	}

	if (prevEnt && prevScore < FLT_MAX && bestEnt != prevEnt && prevScore <= bestScore + hysteresis)
	{
		bestEnt = prevEnt;
		bestIdx = s_HandSel.entIndex;
		bestLeft = prevLeft;
	}

	if (bestEnt)
	{
		out.state = HandSelState::Valid;
		out.clientEnt = bestEnt;
		out.entIndex = bestIdx;
		out.leftHand = bestLeft;

		// Haptic tick on the selecting controller whenever the target changes.
		if (bestEnt != prevEnt)
		{
			bool physicalLeft = vr->m_LeftHanded ? !bestLeft : bestLeft;
			vr->TriggerHapticPulseOnHand(physicalLeft, 1200, 0.45f);
		}
	}
	return out;
}

// Singleplayer: map the client's selected entity index to the server entity (shared edict index).
// CBaseEntity::entindex (server.dll+0xD6B40): mov eax,[ecx+1Ch]; test; jnz; ret; mov ecx,[gpGlobals]; sub eax,[ecx+58h]; sar eax,4
static void* ServerEntityByIndex(int entIndex)
{
	Game* game = Hooks::m_Game;
	if (!game || entIndex <= 0 || entIndex >= 8192)
		return nullptr;
	const uint8_t* fn = (const uint8_t*)game->m_Offsets->CBaseEntity_entindex.address;
	if (!fn || !(fn[0] == 0x8B && fn[1] == 0x41 && fn[2] == 0x1C && fn[8] == 0x8B && fn[9] == 0x0D))
		return nullptr;
	uintptr_t gpGlobalsSlot = *(const uintptr_t*)(fn + 10);
	uintptr_t gpGlobals = gpGlobalsSlot ? *(const uintptr_t*)gpGlobalsSlot : 0;
	if (!gpGlobals)
		return nullptr;
	uintptr_t pEdicts = *(const uintptr_t*)(gpGlobals + 0x58);
	if (!pEdicts)
		return nullptr;
	const uint8_t* edict = (const uint8_t*)(pEdicts + (uintptr_t)entIndex * 16);
	if ((*(const int*)edict) & 0x2)                 // FL_EDICT_FREE
		return nullptr;
	IServerUnknown* unk = *(IServerUnknown* const*)(edict + 0xC);
	if (!unk)
		return nullptr;
	void* ent = unk->GetBaseEntity();
	if (!ent || !Hooks::EntityIndex || Hooks::EntityIndex(ent) != entIndex)
		return nullptr;
	return ent;
}

// Resolve a server EHANDLE directly via g_pEntityList (verified in CGrabController::UpdateObject at server.dll+0x3A98C2):
//   edx = *(uintptr_t*)(baseServer + 0x690770), entry = edx + (eh & 0xFFFF) * 24 + 4, serial check [entry + 4] == (eh >> 16).
static void* ServerEntityByEHandle(uint32_t ehandle)
{
	Game* game = Hooks::m_Game;
	if (!game || !game->m_BaseServer || ehandle == 0xFFFFFFFF)
		return nullptr;
	uint32_t slot = ehandle & 0xFFFFu;
	if (slot >= 8192u)
		return nullptr;
	uintptr_t entList = *(const uintptr_t*)(game->m_BaseServer + 0x690770);
	if (!entList)
		return nullptr;
	uintptr_t entry = entList + (uintptr_t)slot * 24u + 4u;
	if (*(const uint32_t*)(entry + 4u) != (ehandle >> 16))
		return nullptr;
	return *(void* const*)entry;
}

void* __fastcall Hooks::dClientFindUseEntity(void* ecx, void* edx) {
	// ---- Hand-based use selection (see "virtual fingertip" notes at the top of this file) ----
	if (m_VR && m_VR->m_IsVREnabled && ecx && !m_VR->m_In3DViewControlMenu && m_Game->m_BaseClient)
	{
		// Active point_viewcontrol / view entity: INFRA itself disables use selection there (client.dll+0x1C9463).
		if (*(uint32_t*)((uintptr_t)ecx + 0x1738) != 0xFFFFFFFF)
			return hkClientFindUseEntity.fOriginal(ecx);

		auto now = std::chrono::steady_clock::now();

		// When the player is already holding a physics object (m_hHeldObject at +0x2514) or reading a document (+0x2518),
		// suppress the client HUD selection outline so only objects *about* to be grabbed are highlighted.
		bool holding = (*(uint32_t*)((uintptr_t)ecx + 0x2514) != 0xFFFFFFFF || *(uint32_t*)((uintptr_t)ecx + 0x2518) != 0xFFFFFFFF);
		if (!holding && m_VR->m_IsHoldingObject)
		{
			if (std::chrono::duration<float>(now - m_VR->m_LastGrabUpdateTime).count() < 0.15f)
				holding = true;
			else
				m_VR->m_IsHoldingObject = false;
		}
		if (holding)
		{
			s_HandSel = HandUseSelection();
			s_HandSel.state = HandSelState::Skipped;
			s_HandSel.time = now;
			s_HandSelFrame = s_RenderFrameId;
			return nullptr;
		}

		// INFRA calls FindUseEntity several times per frame (selection outline, hints, ...): compute once per rendered frame.
		if (s_HandSelFrame != s_RenderFrameId)
		{
			s_HandSel = ComputeHandUseSelection(ecx);
			s_HandSelFrame = s_RenderFrameId;
		}
		if (s_HandSel.state == HandSelState::Valid)
		{
			m_VR->m_UseWithLeftHand = s_HandSel.leftHand;
			return s_HandSel.clientEnt;
		}
		return nullptr;
	}
	return hkClientFindUseEntity.fOriginal(ecx);
}

void* __fastcall Hooks::dFindUseEntity(void* ecx, void* edx) {
	if (m_VR && m_VR->m_IsVREnabled && ecx && m_Game->m_BaseServer)
	{
		if (m_VR->m_IsHoldingObject ||
		    *(const uint32_t*)((uintptr_t)ecx + 0x189C) != 0xFFFFFFFFu ||
		    *(const uint32_t*)((uintptr_t)ecx + 0x1840) != 0xFFFFFFFFu)
		{
			s_LastUseServerEnt = nullptr;
			return hkFindUseEntity.fOriginal(ecx);
		}

		bool leftDigitalDown  = m_VR->PressedDigitalAction(m_VR->m_ActionCrouch);
		bool rightDigitalDown = m_VR->PressedDigitalAction(m_VR->m_ActionReload);
		bool leftGripDown     = VRGrab::EvaluateHandGripInput(m_VR, true,  m_VR->m_IsHoldingObject &&  m_VR->m_HoldingWithLeftHand);
		bool rightGripDown    = VRGrab::EvaluateHandGripInput(m_VR, false, m_VR->m_IsHoldingObject && !m_VR->m_HoldingWithLeftHand);
		const bool useJustPressed = (*(const int*)((uintptr_t)ecx + 0x0B54) & IN_USE) != 0;
		const bool useReleased = (*(const int*)((uintptr_t)ecx + 0x0B58) & IN_USE) != 0;

		if (!leftGripDown && !rightGripDown && !useReleased)
		{
			s_LastUseServerEnt = nullptr;
			return nullptr;
		}

		{
			auto nowSel = std::chrono::steady_clock::now();
			float selAge = std::chrono::duration<float>(nowSel - s_HandSel.time).count();
			bool selPhysLeft = m_VR->m_LeftHanded ? !s_HandSel.leftHand : s_HandSel.leftHand;
			bool gripMatches = useReleased || (leftGripDown && rightGripDown) || (selPhysLeft == leftGripDown);
			bool needRecompute = !useReleased && (s_HandSel.state != HandSelState::Skipped) &&
			                     (selAge > 0.1f || (s_HandSel.state == HandSelState::Valid && !gripMatches) ||
			                      (s_HandSel.state == HandSelState::None && selAge > 0.03f));
			void* clientPlayer = (m_Game->m_EngineClient && m_Game->m_EngineClient->IsInGame())
				? m_Game->GetClientEntity(m_Game->m_EngineClient->GetLocalPlayer())
				: nullptr;
			if (needRecompute && clientPlayer && !m_VR->m_In3DViewControlMenu &&
			    *(uint32_t*)((uintptr_t)clientPlayer + 0x1738) == 0xFFFFFFFF)
			{
				s_HandSel = ComputeHandUseSelection(clientPlayer);
				s_HandSelFrame = s_RenderFrameId;
				selAge = 0.0f;
			}

			if (selAge <= 0.25f && s_HandSel.state == HandSelState::None)
			{
				s_LastUseServerEnt = nullptr;
				return nullptr;
			}
			if (selAge <= 0.25f && s_HandSel.state == HandSelState::Valid)
			{
				void* srvEnt = ServerEntityByIndex(s_HandSel.entIndex);
				bool clientEntAlive = s_HandSel.clientEnt && m_Game->GetClientEntity(s_HandSel.entIndex) == s_HandSel.clientEnt;
				if (srvEnt && clientEntAlive)
				{
					// Sanity: server and client entity must be the same object (centers within 32 units).
					auto fnSrvWSC = *(const Vector&(__thiscall**)(void*))(*(uintptr_t*)srvEnt + 0x254);
					auto fnCliWSC = *(const Vector&(__thiscall**)(void*))(*(uintptr_t*)s_HandSel.clientEnt + 0x130);
					Vector dc = fnSrvWSC(srvEnt) - fnCliWSC(s_HandSel.clientEnt);
					if (dc.x * dc.x + dc.y * dc.y + dc.z * dc.z <= 32.0f * 32.0f)
					{
						if (!VRGrab::FilterServerPlayerUseTarget(m_Game, srvEnt, s_HandSel.entIndex, useJustPressed))
						{
							s_LastUseServerEnt = nullptr;
							return nullptr;
						}
						m_VR->m_UseWithLeftHand = s_HandSel.leftHand;
						s_LastUseServerEnt = srvEnt;
						return srvEnt;
					}
				}
			}
			// Skipped (holding/reading) or mapping failed: fall back to the legacy server search below.
		}

		bool wasTrue = m_VR->m_OverrideEyeAngles;
		bool prevUseLeft = m_VR->m_UseWithLeftHand;

		if (leftGripDown && !rightGripDown)
			m_VR->m_UseWithLeftHand = !m_VR->m_LeftHanded;
		else if (rightGripDown && !leftGripDown)
			m_VR->m_UseWithLeftHand = m_VR->m_LeftHanded;

		// Save player's server viewangles (pl.v_angle at +0x14D8, +0x14E4, +0x14F0, +0x14FC)
		// and server m_vecAbsOrigin (+0x1CC, read by CCollisionProperty::CalcNearestPoint at server.dll+0xEEA20)
		// because CBasePlayer::EyeVectors (server.dll+0xD7060) and CINFRA_Player::IsUseableEntity (+0x2BE9E0)
		// read these fields directly!
		QAngle savedAng0 = *(QAngle*)((uintptr_t)ecx + 0x14D8);
		QAngle savedAng1 = *(QAngle*)((uintptr_t)ecx + 0x14E4);
		QAngle savedAng2 = *(QAngle*)((uintptr_t)ecx + 0x14F0);
		QAngle savedAng3 = *(QAngle*)((uintptr_t)ecx + 0x14FC);
		Vector savedAbsOrigin = *(Vector*)((uintptr_t)ecx + 0x1CC);

		// INFRA ConVar m_fValue addresses in server.dll:
		// - infra_player_use_dot_threshold (+0x7D4DAC): default 0.95f
		// - infra_player_use_range_extended (+0x7D4EB4): default 96.0f
		float* pDotThresh = (float*)(m_Game->m_BaseServer + 0x7D4DAC);
		float* pRangeExt  = (float*)(m_Game->m_BaseServer + 0x7D4EB4);
		float origDotThresh = *pDotThresh;
		float origRangeExt  = *pRangeExt;

		auto isEntityNearHandServer = [](void* pEnt, const Vector& handPos, float maxDist) -> bool {
			if (!pEnt) return false;
			auto fnWorldSpaceCenter = *(const Vector&(__thiscall**)(void*))(*(uintptr_t*)pEnt + 0x254);
			if (!fnWorldSpaceCenter) return true;
			const Vector& center = fnWorldSpaceCenter(pEnt);
			Vector d = center - handPos;
			return (d.x * d.x + d.y * d.y + d.z * d.z) <= (maxDist * maxDist);
		};

		auto applyServerHandAngles = [&](const QAngle& ang, const Vector& handPos) {
			*(QAngle*)((uintptr_t)ecx + 0x14D8) = ang;
			*(QAngle*)((uintptr_t)ecx + 0x14E4) = ang;
			*(QAngle*)((uintptr_t)ecx + 0x14F0) = ang;
			*(QAngle*)((uintptr_t)ecx + 0x14FC) = ang;
			*(Vector*)((uintptr_t)ecx + 0x1CC)  = Vector(handPos.x, handPos.y, handPos.z - 36.0f);
			m_VR->m_OverrideEyeAngles = true;
		};

		// Pass 1: Close VR Hand Proximity (~28.5 units / ~52cm sphere around the physical hand)
		auto tryServerHandProximity = [&](bool useLeftHand) -> void* {
			m_VR->m_UseWithLeftHand = useLeftHand;
			bool actualLeft = m_VR->m_LeftHanded ? !useLeftHand : useLeftHand;
			Vector handPos = actualLeft ? m_VR->GetLeftPointerAbsPos() : m_VR->GetRightPointerAbsPos();
			const Vector& ptrFwd = actualLeft ? m_VR->m_LeftPointerFwd : m_VR->m_RightPointerFwd;
			const Vector& ptrUp  = actualLeft ? m_VR->m_LeftPointerUp  : m_VR->m_RightPointerUp;
			QAngle handAng;
			QAngle::VectorAngles(ptrFwd, ptrUp, handAng);

			*pDotThresh = 0.25f;
			*pRangeExt  = 9.5f; // 9.5f * 3.0f = 28.5f units trace & sphere radius

			QAngle pitchedAng(std::clamp(handAng.x + 15.0f, -89.0f, 89.0f), handAng.y, 0.0f);
			s_CustomUseAngle = pitchedAng;
			s_HasCustomUseAngle = true;
			applyServerHandAngles(pitchedAng, handPos);
			void* cand = hkFindUseEntity.fOriginal(ecx);
			s_HasCustomUseAngle = false;
			if (cand && isEntityNearHandServer(cand, handPos, 33.0f))
				return cand;

			return nullptr;
		};

		// Pass 2: Normal VR Hand Pointing Reach (up to 72-80 units, comfortable ~38 deg cone along physical pointer)
		auto tryServerHandPointing = [&](bool useLeftHand) -> void* {
			m_VR->m_UseWithLeftHand = useLeftHand;
			bool actualLeft = m_VR->m_LeftHanded ? !useLeftHand : useLeftHand;
			Vector handPos = actualLeft ? m_VR->GetLeftPointerAbsPos() : m_VR->GetRightPointerAbsPos();
			const Vector& ptrFwd = actualLeft ? m_VR->m_LeftPointerFwd : m_VR->m_RightPointerFwd;
			const Vector& ptrUp  = actualLeft ? m_VR->m_LeftPointerUp  : m_VR->m_RightPointerUp;
			QAngle handAng;
			QAngle::VectorAngles(ptrFwd, ptrUp, handAng);

			*pDotThresh = 0.78f;
			*pRangeExt  = origRangeExt;
			s_CustomUseAngle = handAng;
			s_HasCustomUseAngle = true;
			applyServerHandAngles(handAng, handPos);
			void* cand = hkFindUseEntity.fOriginal(ecx);
			s_HasCustomUseAngle = false;
			return cand;
		};

		bool primaryLeft = m_VR->m_UseWithLeftHand;
		void* result = tryServerHandProximity(primaryLeft);
		if (result)
		{
			prevUseLeft = primaryLeft;
		}
		else if (leftGripDown && rightGripDown)
		{
			void* otherProx = tryServerHandProximity(!primaryLeft);
			if (otherProx)
			{
				result = otherProx;
				prevUseLeft = !primaryLeft;
			}
		}
		if (!result)
		{
			auto handHasDigital = [&](bool useLeft) -> bool {
				bool physLeft = m_VR->m_LeftHanded ? !useLeft : useLeft;
				return physLeft ? leftDigitalDown : rightDigitalDown;
			};
			if (handHasDigital(primaryLeft))
			{
				result = tryServerHandPointing(primaryLeft);
				if (result)
					prevUseLeft = primaryLeft;
			}
			if (!result && leftGripDown && rightGripDown && handHasDigital(!primaryLeft))
			{
				void* otherPoint = tryServerHandPointing(!primaryLeft);
				if (otherPoint)
				{
					result = otherPoint;
					prevUseLeft = !primaryLeft;
				}
			}
		}

		*pDotThresh = origDotThresh;
		*pRangeExt  = origRangeExt;
		s_HasCustomUseAngle = false;
		*(QAngle*)((uintptr_t)ecx + 0x14D8) = savedAng0;
		*(QAngle*)((uintptr_t)ecx + 0x14E4) = savedAng1;
		*(QAngle*)((uintptr_t)ecx + 0x14F0) = savedAng2;
		*(QAngle*)((uintptr_t)ecx + 0x14FC) = savedAng3;
		*(Vector*)((uintptr_t)ecx + 0x1CC)  = savedAbsOrigin;
		m_VR->m_UseWithLeftHand = prevUseLeft;
		if (!wasTrue)
			m_VR->m_OverrideEyeAngles = false;
		if (result)
		{
			const int resIdx = EntityIndex ? EntityIndex(result) : -1;
			if (!VRGrab::FilterServerPlayerUseTarget(m_Game, result, resIdx, useJustPressed))
			{
				s_LastUseServerEnt = nullptr;
				return nullptr;
			}
			s_LastUseServerEnt = result;
		}
		else
			s_LastUseServerEnt = nullptr;
		return result;
	}
	s_LastUseServerEnt = nullptr;
	return hkFindUseEntity.fOriginal(ecx);
}

template <typename Fn>
static inline void InvokeCameraMethodWithVROverride(VR* vr, Fn&& fn, void* ecx)
{
	bool wasTrue = vr->m_OverrideEyeAngles;
	bool prevUseLeft = vr->m_UseWithLeftHand;
	vr->m_UseWithLeftHand = false;
	vr->m_OverrideEyeAngles = true;
	fn(ecx);
	vr->m_UseWithLeftHand = prevUseLeft;
	if (!wasTrue)
		vr->m_OverrideEyeAngles = false;
}

void __fastcall Hooks::dCameraPrimaryAttack(void* ecx, void* edx) {
	const float nextBefore = ecx ? *reinterpret_cast<const float*>(reinterpret_cast<uintptr_t>(ecx) + 0x4BC) : 0.0f;
	InvokeCameraMethodWithVROverride(m_VR, hkCameraPrimaryAttack.fOriginal, ecx);
	const float nextAfter = ecx ? *reinterpret_cast<const float*>(reinterpret_cast<uintptr_t>(ecx) + 0x4BC) : 0.0f;
	if (m_VR && m_VR->m_IsVREnabled && ecx &&
	    std::isfinite(nextBefore) && std::isfinite(nextAfter) && (nextAfter > nextBefore + 0.01f))
	{
		s_ServerPhotoTakenSeq.fetch_add(1u, std::memory_order_release);
	}
}

void __fastcall Hooks::dCameraSecondaryAttack(void* ecx, void* edx) {
	InvokeCameraMethodWithVROverride(m_VR, hkCameraSecondaryAttack.fOriginal, ecx);
}

void __fastcall Hooks::dCameraItemPostFrame(void* ecx, void* edx) {
	InvokeCameraMethodWithVROverride(m_VR, hkCameraItemPostFrame.fOriginal, ecx);
}

void __fastcall Hooks::dHandsItemPostFrame(void* ecx, void* edx) {
	hkHandsItemPostFrame.fOriginal(ecx);
	// CINFRAHands::ItemPostFrame (server.dll+0x2964B0) runs during SV_Physics (after dProcessUsercmds) whenever NOT holding a physics object,
	// and unconditionally calls SetWeaponVisible(false), which sets EF_NODRAW (0x20) on CINFRAHands and vm0 right before snapshot transmission!
	// Re-enabling visibility here keeps the bare Right VR Hand (vm0) permanently visible even when not holding an object.
	if (m_VR && m_VR->m_IsVREnabled && m_VR->m_ShowHands && ecx && m_Game->m_BaseServer &&
	    !m_VR->m_In3DViewControlMenu && !m_Game->m_EngineClient->IsLevelMainMenuBackground())
	{
		uintptr_t baseSrv = m_Game->m_BaseServer;
		typedef void* (__thiscall *tGetWeaponOwner)(void *thisptr);
		typedef void* (__thiscall *tGetServerViewModel)(void *thisptr, int vmIndex);

		auto fnGetWeaponOwner     = (tGetWeaponOwner)(baseSrv + 0xAD450);
		auto fnGetServerViewModel = (tGetServerViewModel)(baseSrv + 0x1B36C0);

		void *pOwner = fnGetWeaponOwner(ecx);
		void *vm0    = pOwner ? fnGetServerViewModel(pOwner, 0) : nullptr;
		EnsureServerBareHandViewModel(ecx, vm0);
	}
}

double __fastcall Hooks::dGetCarryDistance(void* ecx, void* edx) {
	if (m_VR && m_VR->m_IsVREnabled) {
		// In flatscreen Source Engine, GetCarryDistance() returns 45..65 units (~1 meter) in front of the player's eyes.
		// Since VR replaces EyePosition with the controller hand position and minDist (0x3A9F80) is zeroed,
		// returning 3.0 units (~5.7cm in front of the wrist) holds the object directly in the palm of the VR hand!
		return 3.0;
	}
	return hkGetCarryDistance.fOriginal(ecx);
}

void __fastcall Hooks::dGrabControllerSetTargetPosition(void* ecx, void* edx, const Vector& target, const QAngle& targetOrientation, bool bIsTeleport) {
	Vector vrTarget = target;
	QAngle vrOrientation = targetOrientation;
	if (VRGrab::ComputeTargetTransform(m_Game, m_VR, ecx, s_LastUseServerEnt, target, targetOrientation, bIsTeleport, vrTarget, vrOrientation))
	{
		hkGrabControllerSetTargetPosition.fOriginal(ecx, vrTarget, vrOrientation, bIsTeleport);
		return;
	}
	hkGrabControllerSetTargetPosition.fOriginal(ecx, target, targetOrientation, bIsTeleport);
}

double __fastcall Hooks::dGetCarryVerticalOffset(void* ecx, void* edx, void* pObject) {
	if (m_VR && m_VR->m_IsVREnabled) {
		return 0.0;
	}
	return hkGetCarryVerticalOffset.fOriginal(ecx, pObject);
}

bool __fastcall Hooks::dViewModelShouldDraw(void* ecx, void* edx) {
	if (m_VR && m_VR->m_IsVREnabled && m_VR->m_ShowHands && m_VR->m_RightHandEmpty && ecx &&
	    !m_VR->m_In3DViewControlMenu && !m_Game->m_EngineClient->IsLevelMainMenuBackground())
	{
		typedef void* (__thiscall *tGetViewModel)(void *thisptr, int index);
		auto fnGetViewModel = (tGetViewModel)(m_Game->m_BaseClient + 0x55260);
		void* localPlayer = m_Game->GetClientEntity(m_Game->m_EngineClient->GetLocalPlayer());
		if (localPlayer && fnGetViewModel(localPlayer, 0) == ecx &&
		    *(void**)((uintptr_t)ecx + 0x68) == nullptr)
			return false;
	}
	return hkViewModelShouldDraw.fOriginal(ecx);
}

void __fastcall Hooks::dFlashlightHideThink(void* ecx, void* edx) {
	if (m_VR && m_VR->m_IsVREnabled && m_VR->m_ShowHands) {
		// Keep ViewModel 1 (Left VR Hand) active even when the flashlight is turned off/holstered
		return;
	}
	hkFlashlightHideThink.fOriginal(ecx);
}

bool __fastcall Hooks::dReloadFlashlight(void* ecx, void* edx) {
	if (m_VR && m_VR->m_IsVREnabled && !m_VR->m_AllowFlashlightReload) {
		// Prevent IN_RELOAD (Right Touchpad Camera battery reload) from reloading the flashlight
		return false;
	}
	return hkReloadFlashlight.fOriginal(ecx);
}

void __fastcall Hooks::dGetCursorPosition(void* ecx, void* edx, int* pX, int* pY) {
	hkGetCursorPosition.fOriginal(ecx, pX, pY);
	if (m_VR && m_VR->m_IsVREnabled)
	{
		// In mat_queue_mode 2, if no 3D scene (dRenderView) is currently rendering (e.g. disconnected 2D menu),
		// VGUI still polls GetCursorPosition on the main thread every frame — keep MainThreadUpdate running here,
		// but never re-enter while dRenderView is mid-frame (prevents Eye 1 vs Eye 2 pose/shift mismatch).
		const unsigned long long nowTick = GetTickCount64();
		const unsigned long long lastRv = m_VR->m_LastRenderViewTickMs.load(std::memory_order_relaxed);
		const unsigned long long lastMu = m_VR->m_LastMainUpdateTickMs.load(std::memory_order_relaxed);
		if (m_VR->IsQueuedRendering() &&
		    !m_VR->m_InRenderView.load(std::memory_order_acquire) &&
		    m_VR->m_MainThreadId.load() == GetCurrentThreadId() &&
		    (lastRv == 0 || (nowTick - lastRv) > 50ULL) &&
		    (nowTick - lastMu) > 20ULL)
		{
			m_VR->MainThreadUpdate();
		}
		if (m_VR->m_ForceCenterCursor)
		{
			int screenW = 1920, screenH = 1080;
			m_Game->m_VguiSurface->GetScreenSize(screenW, screenH);
			if (screenW <= 0) screenW = 1920;
			if (screenH <= 0) screenH = 1080;
			if (pX) *pX = screenW / 2;
			if (pY) *pY = screenH / 2;
			return;
		}
		if (m_VR->m_MenuCursorInitialized &&
		    (m_VR->m_In3DViewControlMenu || m_VR->Is2DSubMenuVisible()))
		{
			if (pX) *pX = m_VR->m_MenuCursorX;
			if (pY) *pY = m_VR->m_MenuCursorY;
		}
	}
}

void* __cdecl Hooks::dFindVGuiScreenUnderCursor(const Vector& vecOrigin, const QAngle& vecAngles, float flFOV) {
	void* pScreen = hkFindVGuiScreenUnderCursor.fOriginal(vecOrigin, vecAngles, flFOV);
	if (!pScreen && m_VR && m_VR->m_IsVREnabled && m_VR->m_In3DViewControlMenu)
	{
		// If the cursor moved slightly past the bezel of the 3D laptop/binder/projector screen,
		// raycast straight down the center of the active point_viewcontrol camera so the 3D menu never loses focus!
		// Note: Never return a cached m_ActiveVGuiScreen pointer here, because destroyed C_VGuiScreen entities
		// after map transitions will cause C_BasePlayer::UpdateVGuiScreenButtonState (client.dll+0x52973) to crash!
		m_VR->m_ForceCenterCursor = true;
		pScreen = hkFindVGuiScreenUnderCursor.fOriginal(m_VR->m_MenuViewOrigin, m_VR->m_MenuViewAngles, m_VR->m_MenuViewFov);
		m_VR->m_ForceCenterCursor = false;
	}
	if (m_VR)
	{
		m_VR->m_ActiveVGuiScreen = pScreen;
		if (pScreen && m_VR->m_IsVREnabled && m_VR->m_In3DViewControlMenu)
		{
			// Ensure m_bLoseThinkNextFrame (+0xAC0) stays 0 so C_VGuiScreen::ClientThink (client.dll+0x903DD)
			// never calls SetNextClientThink(TICK_NEVER_THINK) or forces InternalMouseReleased!
			*(uint8_t*)((uintptr_t)pScreen + 0xAC0) = 0;
		}
	}
	return pScreen;
}

bool __fastcall Hooks::dVGuiScreenGetCursorPos(void* ecx, void* edx, int* pX, int* pY) {
	if (m_VR && m_VR->m_IsVREnabled && ecx)
	{
		int pixelW = *(int*)((uintptr_t)ecx + 0xA80);
		int pixelH = *(int*)((uintptr_t)ecx + 0xA84);

		// 1. In-Game 3D Cell Phone (infra_phone.res, 450x750):
		// Always bypass the 3D HMD-to-hand raycast (even if IsCursorVisible() was toggled off) so hand/finger movement
		// never shifts the ray intersection on v_phone.mdl, and trigger IN_ATTACK in m_nButtonPressed (+0xA98) / m_nButtonReleased (+0xA9C)
		// so C_VGuiScreen::ClientThink calls BOTH SetMousePressed(MOUSE_LEFT) and InternalMousePressed/Released(MOUSE_LEFT).
		if (pixelW == 450 && pixelH == 750 &&
		    m_Game->m_EngineClient->IsInGame() && !m_VR->m_In3DViewControlMenu && !m_VR->Is2DSubMenuVisible())
		{
			int screenW = 1920, screenH = 1080;
			m_Game->m_VguiSurface->GetScreenSize(screenW, screenH);
			if (screenW <= 0) screenW = 1920;
			if (screenH <= 0) screenH = 1080;
			if (pX) *pX = std::clamp((int)((float)m_VR->m_MenuCursorX * (float)pixelW / (float)screenW), 0, pixelW - 1);
			if (pY) *pY = std::clamp((int)((float)m_VR->m_MenuCursorY * (float)pixelH / (float)screenH), 0, pixelH - 1);
			*(uint8_t*)((uintptr_t)ecx + 0xAC0) = 0; // m_bLoseThinkNextFrame = false
			if (m_VR->m_PendingVGuiMousePress)
			{
				*(int*)((uintptr_t)ecx + 0xA98) |= 1; // m_nButtonPressed |= IN_ATTACK
				m_VR->m_PendingVGuiMousePress = false;
			}
			if (m_VR->m_PendingVGuiMouseRelease)
			{
				*(int*)((uintptr_t)ecx + 0xA9C) |= 1; // m_nButtonReleased |= IN_ATTACK
				m_VR->m_PendingVGuiMouseRelease = false;
			}
			return true;
		}

		// 2. 3D Main Menu VGuiScreen (Laptop / Binder / Projector):
		// In menu_main.bsp, engine->IsInGame() is true and CMatSystemSurface::IsCursorLocked() returns true,
		// which caused C_VGuiScreen::GetCursorPos (+0x8FF48) to skip dGetCursorPosition and hardcode (ScreenWidth/2, ScreenHeight/2)!
		// Temporarily bypass the IsCursorVisible (+0x8FE18) and IsCursorLocked (+0x8FF48) checks so C_VGuiScreen::GetCursorPos
		// calls dGetCursorPosition(m_MenuCursorX, m_MenuCursorY) and raycasts onto the exact 3D quad of ecx!
		if (m_VR->m_In3DViewControlMenu && m_Game->m_BaseClient)
		{
			uint8_t* pJneCursorVis = (uint8_t*)(m_Game->m_BaseClient + 0x8FE18);
			uint8_t* pJeCursorLock = (uint8_t*)(m_Game->m_BaseClient + 0x8FF48);
			uint8_t origVis  = *pJneCursorVis;
			uint8_t origLock = *pJeCursorLock;
			if (origVis == 0x75)  *pJneCursorVis = 0xEB; // jmp 1008FE25
			if (origLock == 0x74) *pJeCursorLock = 0xEB; // jmp 1008FF6A (always call dGetCursorPosition)
			_ReadWriteBarrier();

			bool onScreen = hkVGuiScreenGetCursorPos.fOriginal(ecx, pX, pY);

			*pJneCursorVis = origVis;
			*pJeCursorLock = origLock;
			_ReadWriteBarrier();

			if (onScreen)
			{
				*(uint8_t*)((uintptr_t)ecx + 0xAC0) = 0; // m_bLoseThinkNextFrame = false
				if (m_VR->m_PendingVGuiMousePress)
				{
					*(int*)((uintptr_t)ecx + 0xA98) |= 1; // m_nButtonPressed |= IN_ATTACK
					m_VR->m_PendingVGuiMousePress = false;
				}
				if (m_VR->m_PendingVGuiMouseRelease)
				{
					*(int*)((uintptr_t)ecx + 0xA9C) |= 1; // m_nButtonReleased |= IN_ATTACK
					m_VR->m_PendingVGuiMouseRelease = false;
				}
			}
			else if (m_VR->m_ActiveVGuiScreen == ecx)
			{
				m_VR->m_PendingVGuiMousePress = false;
				m_VR->m_PendingVGuiMouseRelease = false;
			}
			return onScreen;
		}
	}
	return hkVGuiScreenGetCursorPos.fOriginal(ecx, pX, pY);
}

bool __fastcall Hooks::dCameraShouldFlash(void* ecx, void* edx)
{
	const bool orig = hkCameraShouldFlash.fOriginal ? hkCameraShouldFlash.fOriginal(ecx) : false;
	if (m_VR && m_VR->m_IsVREnabled)
		return VRCameraFlash::EvaluateShouldFlash(ecx, orig);
	return orig;
}

void __fastcall Hooks::dCameraOnPhotoTaken(void* ecx, void* edx)
{
	if (m_VR && m_VR->m_IsVREnabled)
	{
		// In VR, server dCameraPrimaryAttack -> dClientCameraThink exclusively drives the flash burst.
		// Suppress CINFRACameraScreen's asynchronous OnPhotoTaken trigger so turning the LCD back toward view never fires a 2nd flash.
		return;
	}
	if (hkCameraOnPhotoTaken.fOriginal)
		hkCameraOnPhotoTaken.fOriginal(ecx);
}

void __fastcall Hooks::dCameraFlashlightUpdateLight(void* ecx, void* edx, Vector pos, float qx, float qy, float qz, float qw)
{
	if (m_VR && m_VR->m_IsVREnabled)
	{
		Vector vrPos = pos;
		Vector vrForward{ 1.0f, 0.0f, 0.0f };
		float intensity = 0.0f;
		if (VRCameraFlash::GetProjectedLightOverride(vrPos, vrForward, intensity))
		{
			float q[4] = { qx, qy, qz, qw };
			const QAngle ang = DirToQAngle(vrForward);
			if (m_Game && m_Game->m_BaseClient)
			{
				typedef void(__cdecl* tAngleQuaternion)(const QAngle& angles, float outQ[4]);
				auto fnAngleQuaternion = (tAngleQuaternion)(m_Game->m_BaseClient + 0x2FDB30);
				fnAngleQuaternion(ang, q);
			}
			hkCameraFlashlightUpdateLight.fOriginal(ecx, vrPos, q[0], q[1], q[2], q[3]);
			return;
		}
	}
	hkCameraFlashlightUpdateLight.fOriginal(ecx, pos, qx, qy, qz, qw);
}

void __fastcall Hooks::dCameraScreenOnTick(void* ecx, void* edx)
{
	if (hkCameraScreenOnTick.fOriginal)
		hkCameraScreenOnTick.fOriginal(ecx);

	if (!ecx || !m_Game || !m_Game->m_BaseClient || !m_VR || !m_VR->m_IsVREnabled)
		return;

	if (s_CameraScreenResetPending.exchange(false, std::memory_order_acq_rel))
	{
		s_LastCamScreen = nullptr;
		s_LastFlashMode = -1;
		s_CameraScreenVerifyCounter = 0u;
	}

	void* screenPanel = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(ecx) - 0x234);
	const int mode = static_cast<int>(m_VR->m_CameraFlashMode);
	const bool needsSync = (screenPanel != s_LastCamScreen) || (mode != s_LastFlashMode);
	const bool periodicVerify = (++s_CameraScreenVerifyCounter >= 15u);
	if (!needsSync && !periodicVerify)
		return;

	s_CameraScreenVerifyCounter = 0u;

	const uint8_t* pFindChild = reinterpret_cast<const uint8_t*>(m_Game->m_BaseClient + 0x3B2DB0);
	if (pFindChild[0] != 0x55 || pFindChild[1] != 0x8B || pFindChild[2] != 0xEC ||
	    pFindChild[3] != 0x83 || pFindChild[4] != 0xEC || pFindChild[5] != 0x10 ||
	    pFindChild[6] != 0x53 || pFindChild[7] != 0x56)
	{
		return;
	}

	typedef void* (__thiscall* tFindChildByName)(void* thisptr, const char* childName, bool recurseDown);
	auto fnFindChildByName = reinterpret_cast<tFindChildByName>(const_cast<uint8_t*>(pFindChild));
	void* topRightLabel = fnFindChildByName(screenPanel, "top_right_indicators", true);
	if (!topRightLabel)
	{
		// Panel children may not be attached yet on the first tick; retry immediately next tick.
		s_LastFlashMode = -1;
		return;
	}

	void** vt = *reinterpret_cast<void***>(topRightLabel);
	if (!vt)
		return;

	typedef void(__thiscall* tSetText)(void* thisptr, const char* text);
	typedef void(__thiscall* tGetText)(void* thisptr, char* textOut, int bufLen);
	typedef void(__thiscall* tSetFgColor)(void* thisptr, uint32_t color);
	auto fnSetText = reinterpret_cast<tSetText>(vt[0x2E0 / 4]);
	auto fnGetText = reinterpret_cast<tGetText>(vt[0x2E8 / 4]);
	auto fnSetFgColor = reinterpret_cast<tSetFgColor>(vt[0x80 / 4]);
	if (!fnSetText || !fnSetFgColor)
		return;

	// infra_camera.res top_right_indicators uses INFRAicons font:
	// "g" (burst/photo), "h" (face), "f" (lightning bolt / On), "m" (bolt + A / Auto), "n" (crossed-out bolt / Off)
	const char* glyphStr = "g h m";
	if (mode == static_cast<int>(VR::CameraFlashMode::Off))
	{
		glyphStr = "g h n";
	}
	else if (mode == static_cast<int>(VR::CameraFlashMode::On))
	{
		glyphStr = "g h f";
	}

	bool textMismatch = needsSync;
	if (!textMismatch && fnGetText)
	{
		char curText[32] = {};
		fnGetText(topRightLabel, curText, static_cast<int>(sizeof(curText)));
		textMismatch = (std::strcmp(curText, glyphStr) != 0);
	}
	if (textMismatch)
	{
		fnSetText(topRightLabel, glyphStr);
		fnSetFgColor(topRightLabel, 0xFFFFFFFFu);
	}

	s_LastCamScreen = screenPanel;
	s_LastFlashMode = mode;
}

void Hooks::InstallTextureDownloadHook(ITexture* pTex)
{
	if (!pTex || hkTextureDownload.pTarget != nullptr)
		return;

	void** vtable = *reinterpret_cast<void***>(pTex);
	void* pDownload = vtable ? vtable[0x34 / sizeof(void*)] : nullptr;
	if (!pDownload)
		return;

	const uint8_t* pBytes = reinterpret_cast<const uint8_t*>(pDownload);
	if (pBytes[0] == 0x55 && pBytes[1] == 0x8B && pBytes[2] == 0xEC &&
	    pBytes[3] == 0x56 && pBytes[4] == 0x8B && pBytes[5] == 0xF1)
	{
		if (hkTextureDownload.createHook(pDownload, &dTextureDownload) == 0)
			hkTextureDownload.enableHook();
	}
}

void __fastcall Hooks::dTextureDownload(void* ecx, void* edx, void* pRect, int nAdditionalCreationFlags)
{
	if (!hkTextureDownload.fOriginal)
		return;

	if (m_VR)
	{
		const VR::TextureID prevId = m_VR->m_CreatingTextureID;
		if (ecx == m_VR->m_LeftEyeTexture)
			m_VR->m_CreatingTextureID = VR::Texture_LeftEye;
		else if (ecx == m_VR->m_RightEyeTexture)
			m_VR->m_CreatingTextureID = VR::Texture_RightEye;
		else if (ecx == m_VR->m_HUDTexture)
			m_VR->m_CreatingTextureID = VR::Texture_HUD;
		else if (ecx == m_VR->m_BlankTexture)
			m_VR->m_CreatingTextureID = VR::Texture_Blank;
		hkTextureDownload.fOriginal(ecx, pRect, nAdditionalCreationFlags);
		m_VR->m_CreatingTextureID = prevId;
		return;
	}

	hkTextureDownload.fOriginal(ecx, pRect, nAdditionalCreationFlags);
}