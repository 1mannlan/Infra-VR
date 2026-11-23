#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "vr_grab.h"
#include "vr.h"
#include "vr_inventory.h"
#include "game.h"
#include "offsets.h"
#include "sdk.h"
#include "sdk_server.h"
#include "trace.h"

#ifndef VR_GRAB_HAS_VR_HANDS
#if __has_include("vr_hands.h")
#include "vr_hands.h"
#define VR_GRAB_HAS_VR_HANDS 1
#else
#define VR_GRAB_HAS_VR_HANDS 0
#endif
#elif VR_GRAB_HAS_VR_HANDS
#include "vr_hands.h"
#endif

#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

class ICollideable;

namespace
{
	constexpr float kPi = 3.14159265358979323846f;
	constexpr float kRad2Deg = 180.0f / kPi;

	// Verified binary field offsets on server CGrabController (server.dll+0x3A6B00..0x3AB320):
	//   +0x0004  Vector  m_shadow.targetPosition
	//   +0x0010  QAngle  m_shadow.targetRotation
	//   +0x001C  float   m_shadow.maxAngular
	//   +0x0020  float   m_shadow.maxDampAngular
	//   +0x2034  float   m_timeToArrive
	//   +0x2038  float   m_errorTime          (when <= 0, ComputeError at +0x3A7829 immediately returns 0.0f)
	//   +0x203C  float   m_error
	//   +0x2040  float   m_flAngleAlignment
	//   +0x2054  uint32  m_attachedEntity     (CBaseHandle)
	//   +0x2078  Vector  m_attachedPositionObjectSpace
	//   +0x2084  void*   m_pShadow            (IPhysicsShadowController*)
	//   +0x2094  uint32  m_hPlayer            (CBaseHandle)
	constexpr uintptr_t kGrabOff_TargetPos     = 0x0004;
	constexpr uintptr_t kGrabOff_TargetAng     = 0x0010;
	constexpr uintptr_t kGrabOff_MaxAngular    = 0x001C;
	constexpr uintptr_t kGrabOff_MaxDampAng    = 0x0020;
	constexpr uintptr_t kGrabOff_ErrorTime     = 0x2038;
	constexpr uintptr_t kGrabOff_Error         = 0x203C;
	constexpr uintptr_t kGrabOff_AngleAlign    = 0x2040;
	constexpr uintptr_t kGrabOff_AttachedEnt   = 0x2054;
	constexpr uintptr_t kGrabOff_ObjSpacePos   = 0x2078;
	constexpr uintptr_t kGrabOff_PlayerHandle  = 0x2094;

	// Verified binary field offsets on server CINFRA_Player / CPortal_Player / CBasePlayer:
	//   +0x0B50  int     m_nButtons           (IN_ATTACK = bit 0)
	//   +0x139D  uint8   m_bUsingVMGrab       (CPortal_Player)
	//   +0x139E  uint8   m_bCarriedByLocalPlayer (CPortal_Player; when != 0, SetTargetPosition early-outs!)
	//   +0x13AC  uint32  m_hAttachedObject    (CPortal_Player; drives client ManageHeldObjects / clone)
	//   +0x13BC  Vector  m_vecCarriedOffsetPos
	//   +0x13C8  QAngle  m_angCarriedOffsetAng
	//   +0x1668  uint8   m_bUsingHands
	//   +0x189C  uint32  m_hHeldObject        (CINFRA_Player — preserved untouched)
	constexpr uintptr_t kSrvPlayer_Buttons        = 0x0B50;
	constexpr uintptr_t kSrvPlayer_UsingVMGrab    = 0x139D;
	constexpr uintptr_t kSrvPlayer_CarriedByLocal = 0x139E;
	constexpr uintptr_t kSrvPlayer_AttachedObj    = 0x13AC;
	constexpr uintptr_t kSrvPlayer_CarryOffPos    = 0x13BC;
	constexpr uintptr_t kSrvPlayer_CarryOffAng    = 0x13C8;

	// Verified binary field offsets on server CBaseEntity:
	//   +0x00A8  uint32  m_fEffects           (EF_NODRAW = 0x20)
	//   +0x010C  float   m_Collision.m_flRadius
	//   +0x014C  void*   m_pPhysicsObject     (IPhysicsObject*, Wake() at vtable + 0x60)
	//   +0x0184  matrix3x4_t m_rgflCoordinateFrame
	constexpr uintptr_t kSrvEnt_EffectsAddr    = 0x00A8;
	constexpr uintptr_t kSrvEnt_EFlags         = 0x00C8;   // uint32 m_iEFlags (EFL_KILLME = 0x1, EFL_DIRTY_ABSTRANSFORM = 0x800)
	constexpr uintptr_t kSrvEnt_CollisionRad   = 0x010C;
	constexpr uintptr_t kSrvEnt_PhysicsObject  = 0x014C;
	constexpr uintptr_t kSrvEnt_AbsVelocity    = 0x016C;   // Vector m_vecAbsVelocity (verified server.dll+0x3AD546)
	constexpr uintptr_t kSrvEnt_CoordFrame     = 0x0184;
	constexpr uintptr_t kSrvEnt_VPhysObjListVt = 0x0284;   // CBaseEntity::VPhysicsGetObjectList(IPhysicsObject**, int) (verified server.dll+0x3AD410)
	constexpr uintptr_t kPhysObj_SetVelInstVt  = 0x00C8;   // IPhysicsObject::SetVelocityInstantaneous(const Vector*, const AngularImpulse*) (verified server.dll+0x3A69F8)
	constexpr uintptr_t kSrvPhysEnvGlobalRva   = 0x793C1C; // IPhysicsEnvironment* physenv
	constexpr uintptr_t kSrvGlobalEntListRva   = 0x690770;
	constexpr uintptr_t kSrvFn_RemoveEffects   = 0x0100E0;

	// Verified ConVar RVAs for player_held_object_use_view_model (m_pParent +0x1C, m_fValue +0x2C, m_nValue +0x30):
	constexpr uintptr_t kSrvCvar_UseViewModel  = 0x7F8470;
	constexpr uintptr_t kCliCvar_UseViewModel  = 0x6F6A30;

	// Verified binary field offsets on client C_INFRA_Player / C_Portal_Player:
	//   +0x215D  uint8   m_bUsingVMGrab       (C_Portal_Player)
	//   +0x215E  uint8   m_bCarriedByLocalPlayer (C_Portal_Player)
	//   +0x2160  uint32  m_hAttachedObject    (C_Portal_Player)
	//   +0x2514  uint32  m_hHeldObject        (C_INFRA_Player — preserved untouched)
	constexpr uintptr_t kCliPlayer_UsingVMGrab    = 0x215D;
	constexpr uintptr_t kCliPlayer_CarriedByLocal = 0x215E;
	constexpr uintptr_t kCliPlayer_AttachedObj    = 0x2160;
	constexpr uintptr_t kCliPlayer_HeldObj        = 0x2514;

	inline float ClampF(float v, float lo, float hi)
	{
		return (v < lo) ? lo : ((v > hi) ? hi : v);
	}

	inline float SmoothStep01(float t)
	{
		t = ClampF(t, 0.0f, 1.0f);
		return t * t * (3.0f - 2.0f * t);
	}

	inline float WrapDeg180(float a)
	{
		a = std::fmod(a + 180.0f, 360.0f);
		if (a < 0.0f)
			a += 360.0f;
		return a - 180.0f;
	}

	inline bool HasModuleRange(uintptr_t base, uintptr_t rva, size_t bytes = 4u)
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

	inline bool HasExecutableFn(uintptr_t base, uintptr_t rva)
	{
		if (!HasModuleRange(base, rva, 16u))
			return false;
		const uint8_t b0 = *reinterpret_cast<const uint8_t*>(base + rva);
		return b0 != 0x00u && b0 != 0xCCu;
	}

	inline void ForceConVarIntZero(uintptr_t moduleBase, uintptr_t cvarRva)
	{
		if (!HasModuleRange(moduleBase, cvarRva, 0x34u))
			return;
		const uintptr_t self = moduleBase + cvarRva;
		*reinterpret_cast<float*>(self + 0x2Cu) = 0.0f;
		*reinterpret_cast<int32_t*>(self + 0x30u) = 0;
		const uintptr_t parent = *reinterpret_cast<const uintptr_t*>(self + 0x1Cu);
		if (parent != 0u && parent != self)
		{
			*reinterpret_cast<float*>(parent + 0x2Cu) = 0.0f;
			*reinterpret_cast<int32_t*>(parent + 0x30u) = 0;
		}
	}

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

	inline Vector NormalizeSafe(const Vector& v, const Vector& fallback)
	{
		float lsq = LenSq3(v);
		if (lsq > 1e-8f)
		{
			float inv = 1.0f / std::sqrt(lsq);
			return Vector(v.x * inv, v.y * inv, v.z * inv);
		}
		return fallback;
	}

	inline void Mul3x3(const float A[3][3], const float B[3][3], float out[3][3])
	{
		for (int i = 0; i < 3; ++i)
		{
			for (int j = 0; j < 3; ++j)
			{
				out[i][j] = A[i][0] * B[0][j] + A[i][1] * B[1][j] + A[i][2] * B[2][j];
			}
		}
	}

	inline void MulTransposeA3x3(const float A[3][3], const float B[3][3], float out[3][3])
	{
		for (int i = 0; i < 3; ++i)
		{
			for (int j = 0; j < 3; ++j)
			{
				out[i][j] = A[0][i] * B[0][j] + A[1][i] * B[1][j] + A[2][i] * B[2][j];
			}
		}
	}

	inline Vector RotateVec3x3(const float R[3][3], const Vector& v)
	{
		return Vector(
			R[0][0] * v.x + R[0][1] * v.y + R[0][2] * v.z,
			R[1][0] * v.x + R[1][1] * v.y + R[1][2] * v.z,
			R[2][0] * v.x + R[2][1] * v.y + R[2][2] * v.z);
	}

	inline Vector InvRotateVec3x3(const float R[3][3], const Vector& v)
	{
		return Vector(
			R[0][0] * v.x + R[1][0] * v.y + R[2][0] * v.z,
			R[0][1] * v.x + R[1][1] * v.y + R[2][1] * v.z,
			R[0][2] * v.x + R[1][2] * v.y + R[2][2] * v.z);
	}

	inline void BuildRotFromAngles(const QAngle& ang, float R[3][3])
	{
		Vector fwd, right, up;
		QAngle::AngleVectors(ang, &fwd, &right, &up);
		Vector left(-right.x, -right.y, -right.z);
		R[0][0] = fwd.x;  R[0][1] = left.x; R[0][2] = up.x;
		R[1][0] = fwd.y;  R[1][1] = left.y; R[1][2] = up.y;
		R[2][0] = fwd.z;  R[2][1] = left.z; R[2][2] = up.z;
	}

	inline QAngle AnglesFromRot3x3(const float R[3][3])
	{
		float xyDist = std::sqrt(R[0][0] * R[0][0] + R[1][0] * R[1][0]);
		QAngle out;
		if (xyDist > 0.001f)
		{
			out.x = std::atan2(-R[2][0], xyDist) * kRad2Deg;
			out.y = std::atan2(R[1][0], R[0][0]) * kRad2Deg;
			out.z = std::atan2(R[2][1], R[2][2]) * kRad2Deg;
		}
		else
		{
			out.x = std::atan2(-R[2][0], xyDist) * kRad2Deg;
			out.y = std::atan2(-R[0][1], R[1][1]) * kRad2Deg;
			out.z = 0.0f;
		}
		return out;
	}

	inline Vector TransformPoint3x4(const matrix3x4_t& m, const Vector& p)
	{
		return Vector(
			m[0][0] * p.x + m[0][1] * p.y + m[0][2] * p.z + m[0][3],
			m[1][0] * p.x + m[1][1] * p.y + m[1][2] * p.z + m[1][3],
			m[2][0] * p.x + m[2][1] * p.y + m[2][2] * p.z + m[2][3]);
	}

	inline Vector InvTransformPointOrtho3x4(const matrix3x4_t& m, const Vector& p)
	{
		float dx = p.x - m[0][3];
		float dy = p.y - m[1][3];
		float dz = p.z - m[2][3];
		return Vector(
			m[0][0] * dx + m[1][0] * dy + m[2][0] * dz,
			m[0][1] * dx + m[1][1] * dy + m[2][1] * dz,
			m[0][2] * dx + m[1][2] * dy + m[2][2] * dz);
	}

	void* ResolveServerEntityByEHandle(Game* game, uint32_t eh)
	{
		if (!game || !HasModuleRange(game->m_BaseServer, kSrvGlobalEntListRva, sizeof(uintptr_t)) || eh == 0xFFFFFFFFu)
			return nullptr;
		uint32_t entryIdx = eh & 0xFFFFu;
		if (entryIdx >= 0x1000u)
			return nullptr;
		uint32_t serial = eh >> 16;
		uintptr_t entArrayBase = *reinterpret_cast<const uintptr_t*>(game->m_BaseServer + kSrvGlobalEntListRva);
		if (!entArrayBase)
			return nullptr;
		uintptr_t infoAddr = entArrayBase + 4u + static_cast<uintptr_t>(entryIdx) * 24u;
		if (*reinterpret_cast<const uint32_t*>(infoAddr + 4u) != serial)
			return nullptr;
		return *reinterpret_cast<void* const*>(infoAddr);
	}

	bool IsLiveServerEntityInList(Game* game, const void* candidate, int* outIdx = nullptr)
	{
		if (outIdx)
			*outIdx = -1;
		if (!candidate || !game || !HasModuleRange(game->m_BaseServer, kSrvGlobalEntListRva, sizeof(uintptr_t)))
			return false;
		const uintptr_t entArrayBase = *reinterpret_cast<const uintptr_t*>(game->m_BaseServer + kSrvGlobalEntListRva);
		if (!entArrayBase)
			return false;
		for (uint32_t idx = 1; idx < 4096u; ++idx)
		{
			const uintptr_t infoAddr = entArrayBase + 4u + static_cast<uintptr_t>(idx) * 24u;
			const void* ent = *reinterpret_cast<void* const*>(infoAddr);
			if (ent != candidate)
				continue;
			const uint32_t eFlags = *reinterpret_cast<const uint32_t*>(reinterpret_cast<uintptr_t>(ent) + kSrvEnt_EFlags);
			if ((eFlags & 0x1u) != 0u) // EFL_KILLME
				return false;
			if (outIdx)
				*outIdx = static_cast<int>(idx);
			return true;
		}
		return false;
	}

	int ResolveServerEntityIndex(Game* game, void* serverEnt, uint32_t eh)
	{
		if (eh != 0xFFFFFFFFu)
		{
			int idx = static_cast<int>(eh & 0x0FFFu);
			if (idx > 0 && idx < 4096)
				return idx;
		}
		if (game && serverEnt && game->m_Offsets && game->m_Offsets->CBaseEntity_entindex.address)
		{
			typedef int(__thiscall* tEntIndex)(void*);
			auto fnEntIndex = reinterpret_cast<tEntIndex>(game->m_Offsets->CBaseEntity_entindex.address);
			int idx = fnEntIndex(serverEnt);
			if (idx > 0 && idx < 4096)
				return idx;
		}
		return -1;
	}

	void WakeServerEntityPhysics(void* serverEnt)
	{
		if (!serverEnt)
			return;
		void* physObj = *reinterpret_cast<void* const*>(reinterpret_cast<uintptr_t>(serverEnt) + kSrvEnt_PhysicsObject);
		if (!physObj)
			return;
		uintptr_t vtbl = *reinterpret_cast<const uintptr_t*>(physObj);
		if (!vtbl)
			return;
		typedef void(__thiscall* tWake)(void*);
		auto fnWake = *reinterpret_cast<tWake const*>(vtbl + 0x60u);
		if (fnWake)
			fnWake(physObj);
	}

	inline Vector GetPalmSkinLocalCoords(bool isLeftHandMesh, bool usePhoneOpenTable)
	{
		if (isLeftHandMesh)
			return Vector(3.92f, 0.50f, -0.42f);
		return Vector(3.88f, usePhoneOpenTable ? 0.76f : 0.48f, 0.22f);
	}

	// Grip-pose container (populated via VRHands::ComputeHandGripPose or gimbal-free orthonormal fallback)
	struct LocalGripPose
	{
		Vector gripWorldPos{ 0.0f, 0.0f, 0.0f };
		Vector palmSkinWorldPos{ 0.0f, 0.0f, 0.0f };
		Vector wristWorldPos{ 0.0f, 0.0f, 0.0f };
		Vector palmOutwardNormal{ 0.0f, 0.0f, 1.0f };
		Vector fingerDistalDir{ 1.0f, 0.0f, 0.0f };
		Vector thumbRadialDir{ 0.0f, 1.0f, 0.0f };
		float wristR[3][3]{
			{ 1.0f, 0.0f, 0.0f },
			{ 0.0f, 1.0f, 0.0f },
			{ 0.0f, 0.0f, 1.0f }
		};
		matrix3x4_t wristToWorld{};
		bool isLeftHandMesh = false;
		bool usePhoneOpenTable = false;
	};

	// Orthonormalizes OpenVR controller basis vectors into Source's right-handed frame
	// (+X = Forward, -Y = Right, +Y = Left = -Right, +Z = Up, det([Fwd, -Right, Up]) == +1)
	// without GetLeft/RightControllerAbsAngle() +/-90-degree Euler pitch-pole flips.
	void EvaluateOrthonormalControllerBasis(
		const Vector& rawFwd,
		const Vector& rawRight,
		const Vector& rawUp,
		const QAngle& fallbackAng,
		Vector& outFwd,
		Vector& outRight,
		Vector& outUp)
	{
		if (LenSq3(rawFwd) > 0.25f && (LenSq3(rawRight) > 0.25f || LenSq3(rawUp) > 0.25f))
		{
			outFwd = NormalizeSafe(rawFwd, Vector(1.0f, 0.0f, 0.0f));
			Vector orthoR = rawRight - outFwd * Dot3(rawRight, outFwd);
			if (LenSq3(orthoR) > 1e-6f)
			{
				outRight = NormalizeSafe(orthoR, Vector(0.0f, -1.0f, 0.0f));
			}
			else
			{
				// Fallback when rawRight is degenerate: in Source (+X=Fwd, -Y=Right, +Z=Up), Right = Fwd x Up
				Vector fCrossU(
					outFwd.y * rawUp.z - outFwd.z * rawUp.y,
					outFwd.z * rawUp.x - outFwd.x * rawUp.z,
					outFwd.x * rawUp.y - outFwd.y * rawUp.x);
				outRight = NormalizeSafe(fCrossU, Vector(0.0f, -1.0f, 0.0f));
			}
			// In Source (+X=Fwd, -Y=Right, +Z=Up): Up = Right x Fwd, giving det([Fwd, -Right, Up]) == +1
			Vector rCrossF(
				outRight.y * outFwd.z - outRight.z * outFwd.y,
				outRight.z * outFwd.x - outRight.x * outFwd.z,
				outRight.x * outFwd.y - outRight.y * outFwd.x);
			outUp = NormalizeSafe(rCrossF, Vector(0.0f, 0.0f, 1.0f));
		}
		else
		{
			QAngle::AngleVectors(fallbackAng, &outFwd, &outRight, &outUp);
		}
	}

	bool EvaluateHandGripPoseAtEye(VR* vr, bool physicalLeftHand, const Vector& eyeOrigin, LocalGripPose& out)
	{
		if (!vr || !vr->m_IsVREnabled)
			return false;

#if VR_GRAB_HAS_VR_HANDS
		VRHandGripPose hp{};
		// Raw uncoupled controller pose (applyVisualPhysics = false) so the published physics feedback
		// cannot feed back into its own target and runaway-loop.
		if (VRHands::ComputeHandGripPose(vr, physicalLeftHand, eyeOrigin, hp, false) && hp.valid)
		{
			out.gripWorldPos = hp.gripWorldPos;
			out.palmSkinWorldPos = hp.palmSkinWorldPos;
			out.wristWorldPos = hp.wristWorldPos;
			out.palmOutwardNormal = hp.palmOutwardNormal;
			out.fingerDistalDir = hp.fingerDistalDir;
			out.thumbRadialDir = hp.thumbRadialDir;
			out.wristToWorld = hp.wristToWorld;
			out.isLeftHandMesh = hp.isLeftHandMesh;
			out.usePhoneOpenTable = hp.usePhoneOpenTable;
			for (int r = 0; r < 3; ++r)
			{
				for (int c = 0; c < 3; ++c)
					out.wristR[r][c] = hp.wristToWorld[r][c];
			}
			return true;
		}
#endif

		// Gimbal-free orthonormal controller basis fallback (avoids GetLeft/RightControllerAbsAngle() +/-90 pitch flip)
		const bool useVm0 = vr->m_LeftHanded ? physicalLeftHand : !physicalLeftHand;
		const bool isLeftMesh = !useVm0;
		const Vector ctrlPos = physicalLeftHand
			? vr->GetLeftControllerAbsPos(eyeOrigin)
			: vr->GetRightControllerAbsPos(eyeOrigin);

		const Vector rawFwd = physicalLeftHand ? vr->m_LeftControllerForward : vr->m_RightControllerForward;
		const Vector rawRight = physicalLeftHand ? vr->m_LeftControllerRight : vr->m_RightControllerRight;
		const Vector rawUp = physicalLeftHand ? vr->m_LeftControllerUp : vr->m_RightControllerUp;
		const QAngle ctrlAng = physicalLeftHand ? vr->GetLeftControllerAbsAngle() : vr->GetRightControllerAbsAngle();

		Vector fwd, right, up;
		EvaluateOrthonormalControllerBasis(rawFwd, rawRight, rawUp, ctrlAng, fwd, right, up);

		const Vector vmOrigin = useVm0
			? (ctrlPos - (fwd * 15.5f) - (right * 1.9f) + (up * 2.1f))
			: (ctrlPos - (fwd * 16.7f) + (right * 7.6f) + (up * 5.0f));
		const Vector left(-right.x, -right.y, -right.z);
		const matrix3x4_t modelToWorld(fwd, left, up, vmOrigin);

		static const matrix3x4_t kRightWrist(
			 0.390161f,  0.110232f, -0.914124f,  18.127191f,
			 0.332921f,  0.908747f,  0.251679f,  -8.362570f,
			 0.858451f, -0.402527f,  0.317859f,  -5.509399f
		);
		static const matrix3x4_t kLeftWrist(
			-0.073206f,  0.257670f,  0.963456f,  16.421315f,
			-0.259804f, -0.937620f,  0.231019f,   6.502138f,
			 0.962882f, -0.233398f,  0.135583f,  -3.867408f
		);
		const matrix3x4_t& mToW = useVm0 ? kRightWrist : kLeftWrist;

		for (int r = 0; r < 3; ++r)
		{
			out.wristToWorld[r][0] = modelToWorld[r][0] * mToW[0][0] + modelToWorld[r][1] * mToW[1][0] + modelToWorld[r][2] * mToW[2][0];
			out.wristToWorld[r][1] = modelToWorld[r][0] * mToW[0][1] + modelToWorld[r][1] * mToW[1][1] + modelToWorld[r][2] * mToW[2][1];
			out.wristToWorld[r][2] = modelToWorld[r][0] * mToW[0][2] + modelToWorld[r][1] * mToW[1][2] + modelToWorld[r][2] * mToW[2][2];
			out.wristToWorld[r][3] = modelToWorld[r][0] * mToW[0][3] + modelToWorld[r][1] * mToW[1][3] + modelToWorld[r][2] * mToW[2][3] + modelToWorld[r][3];
			for (int c = 0; c < 3; ++c)
				out.wristR[r][c] = out.wristToWorld[r][c];
		}

		const Vector localPalmSkin = GetPalmSkinLocalCoords(isLeftMesh, false);
		const Vector localGrip = isLeftMesh ? Vector(3.92f, 0.92f, -0.42f) : Vector(3.88f, 0.90f, 0.22f);
		out.wristWorldPos = Vector(out.wristToWorld[0][3], out.wristToWorld[1][3], out.wristToWorld[2][3]);
		out.palmSkinWorldPos = TransformPoint3x4(out.wristToWorld, localPalmSkin);
		out.gripWorldPos = TransformPoint3x4(out.wristToWorld, localGrip);
		out.fingerDistalDir = NormalizeSafe(Vector(out.wristR[0][0], out.wristR[1][0], out.wristR[2][0]), fwd);
		out.palmOutwardNormal = NormalizeSafe(Vector(out.wristR[0][1], out.wristR[1][1], out.wristR[2][1]), up);
		float thumbSign = isLeftMesh ? -1.0f : 1.0f;
		out.thumbRadialDir = NormalizeSafe(
			Vector(out.wristR[0][2] * thumbSign, out.wristR[1][2] * thumbSign, out.wristR[2][2] * thumbSign), left);
		out.isLeftHandMesh = isLeftMesh;
		out.usePhoneOpenTable = false;
		return true;
	}

	bool EvaluateHandGripPose(VR* vr, bool physicalLeftHand, LocalGripPose& out)
	{
		return EvaluateHandGripPoseAtEye(vr, physicalLeftHand, vr ? vr->m_SetupOrigin : Vector(0.0f, 0.0f, 0.0f), out);
	}

	// Per-hand edge-armed, close-contact-gated Knuckles/Index curl grip state [0 = physical Right, 1 = physical Left]
	struct HandCurlGripState
	{
		std::atomic<bool> curlArmed{ false };
		std::atomic<bool> curlLatched{ false };
		std::atomic<bool> curlSustained{ false };
		std::atomic<bool> curlGraspUnopened{ false };
		std::atomic<unsigned long long> latchStartMs{ 0 };
		std::atomic<unsigned long long> curlGraspStartMs{ 0 };
		std::atomic<bool> hasCloseTarget{ false };
		std::atomic<unsigned long long> closeTargetMs{ 0 };

		bool hasEvalCache = false;
		unsigned long long lastEvalTickMs = 0ULL;
		bool lastCachedResult = false;
		bool lastDigitalGrip = false;
		bool lastHoldingOnThisHand = false;
		bool lastHandFreeForCurl = false;
		bool lastUseCtrlActive = false;
		bool lastIsHoldingObject = false;
		int lastSkelState = -1;
		bool lastHasCloseTarget = false;
		float lastCurls[3]{ 0.0f, 0.0f, 0.0f };

		void Reset()
		{
			curlArmed.store(false, std::memory_order_relaxed);
			curlLatched.store(false, std::memory_order_relaxed);
			curlSustained.store(false, std::memory_order_relaxed);
			curlGraspUnopened.store(false, std::memory_order_relaxed);
			latchStartMs.store(0ULL, std::memory_order_relaxed);
			curlGraspStartMs.store(0ULL, std::memory_order_relaxed);
			hasCloseTarget.store(false, std::memory_order_relaxed);
			closeTargetMs.store(0ULL, std::memory_order_relaxed);
			hasEvalCache = false;
			lastEvalTickMs = 0ULL;
			lastCachedResult = false;
		}
	};

	HandCurlGripState s_HandGrip[2];
	std::mutex s_HandGripMutex;
	std::atomic<bool> s_ServerUseCtrlActive{ false };
	std::atomic<bool> s_LastSentInUse{ false };
	std::atomic<unsigned long long> s_LastPropDropMs{ 0ULL };
	std::atomic<int> s_LastDroppedEntIdx{ -1 };

	// High-frequency (90-144 Hz) per-hand OpenVR throw velocity ring buffer [0 = physical Right, 1 = physical Left].
	// Captures controller linear velocity + wrist-flick tangential velocity (omega x r_lever) in Source world units/s
	// so releasing Grip / opening fingers recovers the true peak throw momentum instead of the deceleration tail.
	struct HandThrowSample
	{
		uint64_t tickMs = 0ULL;
		Vector linVelWorld{ 0.0f, 0.0f, 0.0f };
		Vector angVelWorldDeg{ 0.0f, 0.0f, 0.0f };
	};

	struct HandThrowHistory
	{
		static constexpr int kCapacity = 24;
		HandThrowSample samples[kCapacity]{};
		int writeIdx = 0;
		int count = 0;

		void Push(uint64_t nowMs, const Vector& linVel, const Vector& angVelDeg)
		{
			samples[writeIdx] = HandThrowSample{ nowMs, linVel, angVelDeg };
			writeIdx = (writeIdx + 1) % kCapacity;
			if (count < kCapacity)
				++count;
		}

		void Clear()
		{
			writeIdx = 0;
			count = 0;
		}
	};

	HandThrowHistory s_ThrowHist[2];
	std::mutex s_ThrowHistMutex;

	struct CreateMoveUseButtonState
	{
		bool prevDig[2]{ false, false };
		bool prevCurl[2]{ false, false };
		bool prevEffGrip[2]{ false, false };
		bool handCycleActive[2]{ false, false };
		bool hadHoldInCycle[2]{ false, false };
		bool bounceBlocked[2]{ false, false };
		bool staggeredSqueezeSuppressed[2]{ false, false };
		unsigned long long lastImpulseReleaseMs[2]{ 0ULL, 0ULL };
		bool prevInUseSent = false;
		bool reassertNextCmd = false;

		void Reset()
		{
			prevDig[0] = prevDig[1] = false;
			prevCurl[0] = prevCurl[1] = false;
			prevEffGrip[0] = prevEffGrip[1] = false;
			handCycleActive[0] = handCycleActive[1] = false;
			hadHoldInCycle[0] = hadHoldInCycle[1] = false;
			bounceBlocked[0] = bounceBlocked[1] = false;
			staggeredSqueezeSuppressed[0] = staggeredSqueezeSuppressed[1] = false;
			lastImpulseReleaseMs[0] = lastImpulseReleaseMs[1] = 0ULL;
			prevInUseSent = false;
			reassertNextCmd = false;
		}
	};

	CreateMoveUseButtonState s_CreateMoveUse;

	struct DoorCooldownSlot
	{
		uint32_t entIdx = 0;
		uint32_t serial = 0;
		uint64_t nextMs = 0;
		uint64_t lastOpenHandPushMs = 0;
		bool latchedByHand[2]{ false, false };
		uint64_t clearStartMs[2]{ 0ULL, 0ULL };
	};

	struct LastServerImpulseUse
	{
		uint32_t entIdx = 0;
		uint32_t serial = 0;
		uint64_t tickMs = 0;
	};

	DoorCooldownSlot s_DoorCooldowns[16]{};
	LastServerImpulseUse s_LastServerImpulseUse{};

	struct ButtonTouchSlot
	{
		uint32_t entIdx = 0;
		uint32_t serial = 0;
		uint64_t cooldownUntilMs = 0ULL;
		uint64_t lastTouchClickMs = 0ULL;
		bool latchedByHand[2]{ false, false };
		uint64_t clearStartMs[2]{ 0ULL, 0ULL };
		bool momentaryOn[2]{ false, false };
	};

	ButtonTouchSlot s_ButtonTouchSlots[16]{};
	uint64_t s_ButtonTouchSuppressUntilMs = 0ULL;

	inline void ClearDoorAndImpulseCooldowns()
	{
		std::memset(s_DoorCooldowns, 0, sizeof(s_DoorCooldowns));
		std::memset(s_ButtonTouchSlots, 0, sizeof(s_ButtonTouchSlots));
		s_LastServerImpulseUse = LastServerImpulseUse{};
		s_ButtonTouchSuppressUntilMs = GetTickCount64() + 500ULL;
	}

	// Lock-free seqlock snapshot sharing the wrist-locked object attachment from the server simulation
	// thread to the client render thread so finger-surface collision has zero 1-tick interpolation jitter.
	struct HeldAttachmentSnapshot
	{
		std::atomic<uint32_t> seq{ 0 };
		std::atomic<int> entIndex{ -1 };
		std::atomic<bool> holdingLeftHand{ false };
		std::atomic<float> R_rel[3][3]{
			{ { 1.0f }, { 0.0f }, { 0.0f } },
			{ { 0.0f }, { 1.0f }, { 0.0f } },
			{ { 0.0f }, { 0.0f }, { 1.0f } }
		};
		std::atomic<float> wristLocalOrigin[3]{ { 3.88f }, { 0.90f }, { 0.22f } }; // Object origin in wristToWorld space
		std::atomic<float> localOBBCenter[3]{ { 0.0f }, { 0.0f }, { 0.0f } };
		std::atomic<float> obbHalfExtents[3]{ { 1.5f }, { 1.5f }, { 1.5f } };
		std::atomic<unsigned long long> tickMs{ 0 };

		void Publish(
			int idx,
			bool leftHand,
			const float rel[3][3],
			const Vector& localOrig,
			const Vector& obbCenter,
			const Vector& obbHalf,
			unsigned long long nowMs)
		{
			uint32_t s = seq.load(std::memory_order_relaxed);
			if ((s & 1u) != 0u)
				++s;
			seq.store(s + 1u, std::memory_order_relaxed);
			std::atomic_thread_fence(std::memory_order_release);

			entIndex.store(idx, std::memory_order_relaxed);
			holdingLeftHand.store(leftHand, std::memory_order_relaxed);
			for (int r = 0; r < 3; ++r)
			{
				for (int c = 0; c < 3; ++c)
					R_rel[r][c].store(rel[r][c], std::memory_order_relaxed);
			}
			wristLocalOrigin[0].store(localOrig.x, std::memory_order_relaxed);
			wristLocalOrigin[1].store(localOrig.y, std::memory_order_relaxed);
			wristLocalOrigin[2].store(localOrig.z, std::memory_order_relaxed);
			localOBBCenter[0].store(obbCenter.x, std::memory_order_relaxed);
			localOBBCenter[1].store(obbCenter.y, std::memory_order_relaxed);
			localOBBCenter[2].store(obbCenter.z, std::memory_order_relaxed);
			obbHalfExtents[0].store(obbHalf.x, std::memory_order_relaxed);
			obbHalfExtents[1].store(obbHalf.y, std::memory_order_relaxed);
			obbHalfExtents[2].store(obbHalf.z, std::memory_order_relaxed);
			tickMs.store(nowMs, std::memory_order_relaxed);

			seq.store(s + 2u, std::memory_order_release);
		}

		void Clear()
		{
			uint32_t s = seq.load(std::memory_order_relaxed);
			if ((s & 1u) != 0u)
				++s;
			seq.store(s + 1u, std::memory_order_relaxed);
			std::atomic_thread_fence(std::memory_order_release);

			entIndex.store(-1, std::memory_order_relaxed);
			tickMs.store(0ULL, std::memory_order_relaxed);

			seq.store(s + 2u, std::memory_order_release);
		}

		bool Read(
			int& outIdx,
			bool& outLeft,
			float outRel[3][3],
			Vector& outLocalOrig,
			unsigned long long& outTickMs,
			Vector* outOBBCenter = nullptr,
			Vector* outOBBHalf = nullptr) const
		{
			for (int i = 0; i < 4; ++i)
			{
				uint32_t s0 = seq.load(std::memory_order_acquire);
				if (s0 == 0u)
					return false;
				if ((s0 & 1u) != 0u)
					continue;

				int idx = entIndex.load(std::memory_order_relaxed);
				bool left = holdingLeftHand.load(std::memory_order_relaxed);
				float rel[3][3]{};
				for (int r = 0; r < 3; ++r)
				{
					for (int c = 0; c < 3; ++c)
						rel[r][c] = R_rel[r][c].load(std::memory_order_relaxed);
				}
				Vector loc(
					wristLocalOrigin[0].load(std::memory_order_relaxed),
					wristLocalOrigin[1].load(std::memory_order_relaxed),
					wristLocalOrigin[2].load(std::memory_order_relaxed));
				Vector obbC(
					localOBBCenter[0].load(std::memory_order_relaxed),
					localOBBCenter[1].load(std::memory_order_relaxed),
					localOBBCenter[2].load(std::memory_order_relaxed));
				Vector obbH(
					obbHalfExtents[0].load(std::memory_order_relaxed),
					obbHalfExtents[1].load(std::memory_order_relaxed),
					obbHalfExtents[2].load(std::memory_order_relaxed));
				unsigned long long t = tickMs.load(std::memory_order_relaxed);

				std::atomic_thread_fence(std::memory_order_acquire);
				uint32_t s1 = seq.load(std::memory_order_relaxed);
				if (s0 == s1)
				{
					if (idx <= 0 || t == 0ULL)
						return false;
					outIdx = idx;
					outLeft = left;
					std::memcpy(outRel, rel, sizeof(rel));
					outLocalOrig = loc;
					if (outOBBCenter)
						*outOBBCenter = obbC;
					if (outOBBHalf)
						*outOBBHalf = obbH;
					outTickMs = t;
					return true;
				}
			}
			return false;
		}
	};

	HeldAttachmentSnapshot s_HeldSnapshot;

	// Server-side state for active pickup session
	struct ServerGrabSession
	{
		void* lastGrabCtrl = nullptr;
		uint32_t lastEHandle = 0xFFFFFFFFu;
		int heldEntIndex = -1;
		bool setTargetCalledInUpdate = false;
		bool hasLocalCenter = false;
		bool hasServerOBB = false;
		bool hasCalibratedSeating = false;
		bool isLargeObject = false;
		float objRadius = 0.0f;
		float initialObjRot[3][3]{
			{ 1.0f, 0.0f, 0.0f },
			{ 0.0f, 1.0f, 0.0f },
			{ 0.0f, 0.0f, 1.0f }
		};
		float R_rel[3][3]{
			{ 1.0f, 0.0f, 0.0f },
			{ 0.0f, 1.0f, 0.0f },
			{ 0.0f, 0.0f, 1.0f }
		};
		Vector initialEngineTargetPos{ 0.0f, 0.0f, 0.0f };
		LocalGripPose initialGripPose{};
		Vector localOBBCenter{ 0.0f, 0.0f, 0.0f };
		Vector obbHalfExtents{ 1.5f, 1.5f, 1.5f };
		Vector initialWristLocalCenter{ 3.88f, 0.90f, 0.22f }; // Initial object center in wristToWorld space
		Vector targetWristLocalCenter{ 3.88f, 0.90f, 0.22f };  // Seated flush-on-skin object center in wristToWorld space
		uint64_t seatDurationMs = 0;         // Smooth glide duration (90 ms close contact, 115 ms reach pull-in)
		bool hasSettledInHand = false;       // far-grab pull-in finished -> physics feedback / haptics armed
		uint64_t attachTickMs = 0;
		bool wasWallBlocked = false;
		uint64_t lastHapticTickMs = 0;
		Vector lastTargetPos{ 0.0f, 0.0f, 0.0f };
		uint64_t lastTargetTickMs = 0;
		float lastTargetSpeed = 0.0f;
		bool hasLastTarget = false;
		float stablePickupYaw = 0.0f;
		bool yawInitialized = false;
		Vector prevHorizontalDir{ 1.0f, 0.0f, 0.0f };
		QAngle cachedPickupEyeAngles{ 0.0f, 0.0f, 0.0f };

		// Short ring buffer of the held prop's own live VPhysics velocity (sampled per server tick)
		// so releasing Grip simply preserves the prop's real momentum without DetachEntity clamping it.
		struct PropVelSample
		{
			uint64_t tickMs = 0ULL;
			Vector linVel{ 0.0f, 0.0f, 0.0f };
			Vector angVel{ 0.0f, 0.0f, 0.0f };
		};
		static constexpr int kPropVelHistCap = 4;
		PropVelSample propVelHist[kPropVelHistCap]{};
		int propVelWriteIdx = 0;
		int propVelCount = 0;

		void PushPropVel(uint64_t nowMs, const Vector& lin, const Vector& ang)
		{
			propVelHist[propVelWriteIdx] = PropVelSample{ nowMs, lin, ang };
			propVelWriteIdx = (propVelWriteIdx + 1) % kPropVelHistCap;
			if (propVelCount < kPropVelHistCap)
				++propVelCount;
		}

		void ClearPropVel()
		{
			propVelWriteIdx = 0;
			propVelCount = 0;
		}
	};

	ServerGrabSession s_SrvGrab;

	inline unsigned short ClampHapticDurationUs(unsigned short durationUs)
	{
		return (durationUs < 750u) ? 750u : ((durationUs > 950u) ? 950u : durationUs);
	}

	inline float GetControllerSpeedMps(const VR* vr, bool physicalLeftHand)
	{
		if (!vr)
			return 0.0f;
		const float sp = physicalLeftHand ? vr->m_LeftHandSpeedMps : vr->m_RightHandSpeedMps;
		return (std::isfinite(sp) && sp > 0.0f) ? sp : 0.0f;
	}

	inline float UnitsPerSecToMps(const VR* vr, float speedUnitsPerSec)
	{
		const float vrScale = (vr && vr->m_VRScale > 1.0f && vr->m_VRScale < 300.0f) ? vr->m_VRScale : 43.2f;
		if (!std::isfinite(speedUnitsPerSec) || speedUnitsPerSec <= 0.0f)
			return 0.0f;
		return speedUnitsPerSec / vrScale;
	}

	inline bool IsHandBusyWithInventory(bool physicalLeftHand)
	{
		return VRInventory::IsHandHoldingBattery(physicalLeftHand) ||
		       VRInventory::IsHandInInventoryZone(physicalLeftHand);
	}

	// Rate-limited short controller haptic pulse (never throws, never touches an invalid device index).
	void TriggerControllerHaptic(VR* vr, bool physicalLeftHand, unsigned short durationUs, float amplitude, uint64_t cooldownMs, uint64_t& inOutLastTickMs)
	{
		if (!vr || !vr->m_IsVREnabled)
			return;
		const uint64_t now = GetTickCount64();
		if (now >= inOutLastTickMs && (now - inOutLastTickMs) < cooldownMs)
			return;
		vr->TriggerHapticPulseOnHand(physicalLeftHand, ClampHapticDurationUs(durationUs), ClampF(amplitude, 0.0f, 1.0f));
		inOutLastTickMs = now;
	}

	// Effective cross-sectional half-thickness along -palmOutwardNormal so elongated props
	// (bottles, cans, pipes, tools) seat against the VVD inner palm skin by their barrel cross-section
	// rather than half their longitudinal height when grabbed at an angle.
	float ComputeEffectivePalmHalfThickness(
		const float R_obj[3][3],
		const Vector& obbHalfExtents,
		const matrix3x4_t& wristToWorld)
	{
		const Vector palmN(wristToWorld[0][1], wristToWorld[1][1], wristToWorld[2][1]);
		const Vector negPalmObj = InvRotateVec3x3(R_obj, Vector(-palmN.x, -palmN.y, -palmN.z));
		const float rawSupport =
			std::fabs(negPalmObj.x) * obbHalfExtents.x +
			std::fabs(negPalmObj.y) * obbHalfExtents.y +
			std::fabs(negPalmObj.z) * obbHalfExtents.z;

		float hSorted[3] = { obbHalfExtents.x, obbHalfExtents.y, obbHalfExtents.z };
		if (hSorted[0] > hSorted[1]) std::swap(hSorted[0], hSorted[1]);
		if (hSorted[1] > hSorted[2]) std::swap(hSorted[1], hSorted[2]);
		if (hSorted[0] > hSorted[1]) std::swap(hSorted[0], hSorted[1]);
		return std::min(rawSupport, hSorted[1]);
	}

	// Outward (+palmOutwardNormal) shift in Source units so a prop's cross-section has zero
	// penetration behind the VVD inner palm skin plane without creating an artificial hover gap.
	float ComputeMinimumPalmClearancePush(
		const float R_obj[3][3],
		const Vector& obbHalfExtents,
		const matrix3x4_t& wristToWorld,
		const Vector& wristLocalCenter,
		float palmSkinY)
	{
		const float effSupport = ComputeEffectivePalmHalfThickness(R_obj, obbHalfExtents, wristToWorld);
		const float have = wristLocalCenter.y - palmSkinY;
		return std::max(0.0f, effSupport - have);
	}

	// Client-side per-hand smoothed curl clamp state [0 = physical Right, 1 = physical Left]
	struct ClientCurlState
	{
		std::atomic<bool> active{ false };
		std::atomic<float> maxCurl[5]{
			{ 1.0f }, { 1.0f }, { 1.0f }, { 1.0f }, { 1.0f }
		};
		std::atomic<float> smoothed[5]{
			{ 1.0f }, { 1.0f }, { 1.0f }, { 1.0f }, { 1.0f }
		};
		std::atomic<int> lastEntIndex{ -1 };
	};

	ClientCurlState s_ClientCurl[2];

	void ResetClientCurlSlot(int slotIdx, bool physicalLeftHand)
	{
		ClientCurlState& st = s_ClientCurl[slotIdx];
		st.active.store(false, std::memory_order_release);
		for (int i = 0; i < 5; ++i)
		{
			st.maxCurl[i].store(1.0f, std::memory_order_relaxed);
			st.smoothed[i].store(1.0f, std::memory_order_relaxed);
		}
		st.lastEntIndex.store(-1, std::memory_order_relaxed);
#if VR_GRAB_HAS_VR_HANDS
		VRHands::ClearFingerCurlLimits(physicalLeftHand);
#endif
	}

	// Queries OBBMins and OBBMaxs directly from server-side ICollideable on the server thread
	// without touching client entities or client.dll CollisionToWorldTransform's static ring buffer.
	// Verified server.dll CBaseEntity vtable slot 3 (+0x0C) -> CCollisionProperty (vtable 0x10524E4C):
	//   slot 1 (+0x04): 0x10202040 const Vector& OBBMins() const
	//   slot 2 (+0x08): 0x100EE1B0 const Vector& OBBMaxs() const
	bool QueryServerCollideableBounds(
		const void* srvEnt,
		Vector& outMins,
		Vector& outMaxs,
		ICollideable** outCollideable = nullptr)
	{
		if (outCollideable)
			*outCollideable = nullptr;
		if (!srvEnt)
			return false;
		uintptr_t entVtbl = *reinterpret_cast<const uintptr_t*>(srvEnt);
		if (!entVtbl)
			return false;

		typedef ICollideable* (__thiscall* tGetCollideable)(const void*);
		auto fnGetCollideable = *reinterpret_cast<tGetCollideable const*>(entVtbl + 0x0Cu);
		if (!fnGetCollideable)
			return false;
		ICollideable* coll = fnGetCollideable(srvEnt);
		if (!coll)
			return false;

		uintptr_t collVtbl = *reinterpret_cast<const uintptr_t*>(coll);
		if (!collVtbl)
			return false;

		typedef const Vector& (__thiscall* tOBBVec)(const void*);
		auto fnMins = *reinterpret_cast<tOBBVec const*>(collVtbl + 0x04u);
		auto fnMaxs = *reinterpret_cast<tOBBVec const*>(collVtbl + 0x08u);
		if (!fnMins || !fnMaxs)
			return false;

		outMins = fnMins(coll);
		outMaxs = fnMaxs(coll);
		if (outCollideable)
			*outCollideable = coll;
		return true;
	}

	// Queries OBBMins, OBBMaxs, GetSolid, and CollisionToWorldTransform from client ICollideable
	// Verified client.dll ICollideable vtable (0x104A9EBC):
	//   slot 1 (+0x04): const Vector& OBBMins() const
	//   slot 2 (+0x08): const Vector& OBBMaxs() const
	//   slot 10 (+0x28): const matrix3x4_t& CollisionToWorldTransform() const
	//   slot 11 (+0x2C): SolidType_t GetSolid() const
	bool QueryClientCollideableBounds(
		Game* game,
		int entIndex,
		ICollideable*& outCollideable,
		Vector& outMins,
		Vector& outMaxs,
		matrix3x4_t& outCollToWorld,
		int& outSolidType)
	{
		outCollideable = nullptr;
		if (!game || entIndex <= 0)
			return false;
		void* clientEnt = game->GetClientEntity(entIndex);
		if (!clientEnt)
			return false;
		uintptr_t entVtbl = *reinterpret_cast<const uintptr_t*>(clientEnt);
		if (!entVtbl)
			return false;

		typedef ICollideable* (__thiscall* tGetCollideable)(void*);
		auto fnGetCollideable = *reinterpret_cast<tGetCollideable const*>(entVtbl + 0x0Cu); // slot 3
		if (!fnGetCollideable)
			return false;
		ICollideable* coll = fnGetCollideable(clientEnt);
		if (!coll)
			return false;

		uintptr_t collVtbl = *reinterpret_cast<const uintptr_t*>(coll);
		if (!collVtbl)
			return false;

		typedef const Vector& (__thiscall* tOBBVec)(const void*);
		typedef const matrix3x4_t& (__thiscall* tCollToWorld)(const void*);
		typedef int(__thiscall* tGetSolid)(const void*);

		auto fnMins = *reinterpret_cast<tOBBVec const*>(collVtbl + 0x04u);
		auto fnMaxs = *reinterpret_cast<tOBBVec const*>(collVtbl + 0x08u);
		auto fnXform = *reinterpret_cast<tCollToWorld const*>(collVtbl + 0x28u);
		auto fnSolid = *reinterpret_cast<tGetSolid const*>(collVtbl + 0x2Cu);
		if (!fnMins || !fnMaxs || !fnXform)
			return false;

		outMins = fnMins(coll);
		outMaxs = fnMaxs(coll);
		outCollToWorld = fnXform(coll);
		outSolidType = fnSolid ? fnSolid(coll) : 6;
		outCollideable = coll;
		return true;
	}

	// Exact swept-AABB (inflate OBB by padRadius) slab intersection in collideable local space.
	// Returns hit fraction in [0..1] (0.0f if starting inside inflated OBB, >1.0f if no hit).
	float SweepSphereAgainstLocalOBB(
		const Vector& localStart,
		const Vector& localEnd,
		const Vector& obbMins,
		const Vector& obbMaxs,
		float padRadius)
	{
		const float minB[3] = { obbMins.x - padRadius, obbMins.y - padRadius, obbMins.z - padRadius };
		const float maxB[3] = { obbMaxs.x + padRadius, obbMaxs.y + padRadius, obbMaxs.z + padRadius };
		const float s[3] = { localStart.x, localStart.y, localStart.z };
		const float d[3] = { localEnd.x - localStart.x, localEnd.y - localStart.y, localEnd.z - localStart.z };

		if (s[0] >= minB[0] && s[0] <= maxB[0] &&
		    s[1] >= minB[1] && s[1] <= maxB[1] &&
		    s[2] >= minB[2] && s[2] <= maxB[2])
		{
			return 0.0f;
		}

		float tEnter = 0.0f;
		float tExit = 1.0f;
		for (int a = 0; a < 3; ++a)
		{
			if (std::fabs(d[a]) < 1e-6f)
			{
				if (s[a] < minB[a] || s[a] > maxB[a])
					return 2.0f;
			}
			else
			{
				float invD = 1.0f / d[a];
				float t0 = (minB[a] - s[a]) * invD;
				float t1 = (maxB[a] - s[a]) * invD;
				if (t0 > t1)
				{
					float tmp = t0;
					t0 = t1;
					t1 = tmp;
				}
				if (t0 > tEnter)
					tEnter = t0;
				if (t1 < tExit)
					tExit = t1;
				if (tEnter > tExit)
					return 2.0f;
			}
		}
		return (tEnter >= 0.0f && tEnter <= 1.0f) ? tEnter : 2.0f;
	}

	// Invokes IEngineTrace::ClipRayToCollideable (verified engine.dll vtable slot 4, +0x10)
	bool ClipSweptSphereToCollideable(
		IEngineTrace* engineTrace,
		ICollideable* collideable,
		const Vector& startWorld,
		const Vector& endWorld,
		float radius,
		float& outFraction,
		bool& outStartSolid)
	{
		outFraction = 1.0f;
		outStartSolid = false;
		if (!engineTrace || !collideable)
			return false;

		uintptr_t traceVtbl = *reinterpret_cast<const uintptr_t*>(engineTrace);
		if (!traceVtbl)
			return false;

		typedef void(__thiscall* tClipRayToCollideable)(
			void* thisptr,
			const Ray_t& ray,
			unsigned int fMask,
			ICollideable* pCollideable,
			trace_t* pTrace);

		auto fnClip = *reinterpret_cast<tClipRayToCollideable const*>(traceVtbl + 0x10u);
		if (!fnClip)
			return false;

		Ray_t ray{};
		const Vector ext(-radius, -radius, -radius);
		const Vector extPos(radius, radius, radius);
		ray.Init(startWorld, endWorld, ext, extPos);

		CGameTrace tr{};
		tr.fraction = 1.0f;
		tr.startsolid = false;
		tr.allsolid = false;

		// Include solid, moveable, debris, and grate contents so props in COLLISION_GROUP_PASSABLE_DOOR (23)
		// or COLLISION_GROUP_DEBRIS_TRIGGER (2) always clip against their exact VPhysics / studio soup.
		constexpr unsigned int kClipMask = 0xFFFFFFFFu;
		fnClip(engineTrace, ray, kClipMask, collideable, &tr);

		outStartSolid = tr.startsolid || tr.allsolid;
		outFraction = tr.fraction;
		return outStartSolid || (tr.fraction < 1.0f);
	}

	// Disables Portal 2 singleplayer VM-grab / C_PlayerHeldObjectClone hijacking so:
	//   1) server CGrabController::SetTargetPosition (+0x3A7691) never early-outs on IsCarriedByLocalPlayer(),
	//   2) the real server prop never keeps EF_NODRAW (0x20) or FSOLID_NOT_SOLID from UseVMGrab (+0x3AB1A0),
	//   3) client CPortal_Player::ManageHeldObjects (+0x1E9DC0) never spawns a viewmodel clone or runs
	//      unhooked client UpdateObject / UpdateObjectVM / ClientAdjustRenderOrigin, while preserving
	//      CINFRA_Player::m_hHeldObject (server +0x189C / client +0x2514) intact.
	void NeutralizeVMGrabAndClientClone(
		Game* game,
		VR* vr,
		void* grabCtrl,
		void* pServerPlayer,
		void* fallbackSrvEnt = nullptr)
	{
		if (game)
		{
			ForceConVarIntZero(game->m_BaseServer, kSrvCvar_UseViewModel);
			ForceConVarIntZero(game->m_BaseClient, kCliCvar_UseViewModel);
		}

		if (!pServerPlayer && grabCtrl && game)
		{
			const uint32_t playerEh = *reinterpret_cast<const uint32_t*>(
				reinterpret_cast<uintptr_t>(grabCtrl) + kGrabOff_PlayerHandle);
			pServerPlayer = ResolveServerEntityByEHandle(game, playerEh);
		}

		if (pServerPlayer)
		{
			const uintptr_t spAddr = reinterpret_cast<uintptr_t>(pServerPlayer);
			*reinterpret_cast<uint8_t*>(spAddr + kSrvPlayer_UsingVMGrab) = 0u;

			if (*reinterpret_cast<const uint8_t*>(spAddr + kSrvPlayer_CarriedByLocal) != 0u)
			{
				if (grabCtrl)
				{
					const uintptr_t grabVtbl = *reinterpret_cast<const uintptr_t*>(grabCtrl);
					if (grabVtbl)
					{
						typedef void(__thiscall* tRestoreNonVMGrab)(void*, bool);
						auto fnRestore = *reinterpret_cast<tRestoreNonVMGrab const*>(grabVtbl + 0x10u);
						if (fnRestore)
							fnRestore(grabCtrl, false);
					}
				}
				*reinterpret_cast<uint8_t*>(spAddr + kSrvPlayer_CarriedByLocal) = 0u;
			}

			// Clear CPortal_Player::m_hAttachedObject (+0x13AC) while leaving CINFRA_Player::m_hHeldObject (+0x189C) intact
			*reinterpret_cast<uint32_t*>(spAddr + kSrvPlayer_AttachedObj) = 0xFFFFFFFFu;
		}

		void* srvEnt = fallbackSrvEnt;
		if (!srvEnt && grabCtrl && game)
		{
			const uint32_t eh = *reinterpret_cast<const uint32_t*>(
				reinterpret_cast<uintptr_t>(grabCtrl) + kGrabOff_AttachedEnt);
			if (eh != 0xFFFFFFFFu)
				srvEnt = ResolveServerEntityByEHandle(game, eh);
		}
		else if (!srvEnt && pServerPlayer && game && vr && vr->m_IsHoldingObject)
		{
			const uint32_t heldEh = *reinterpret_cast<const uint32_t*>(
				reinterpret_cast<uintptr_t>(pServerPlayer) + 0x189Cu);
			if (heldEh != 0xFFFFFFFFu)
				srvEnt = ResolveServerEntityByEHandle(game, heldEh);
			if (!srvEnt && s_SrvGrab.lastEHandle != 0xFFFFFFFFu)
				srvEnt = ResolveServerEntityByEHandle(game, s_SrvGrab.lastEHandle);
		}

		if (srvEnt)
		{
			const uintptr_t entAddr = reinterpret_cast<uintptr_t>(srvEnt);
			uint32_t& effects = *reinterpret_cast<uint32_t*>(entAddr + kSrvEnt_EffectsAddr);
			if ((effects & 0x20u) != 0u)
			{
				if (game && HasExecutableFn(game->m_BaseServer, kSrvFn_RemoveEffects))
				{
					typedef void(__thiscall* tRemoveEffects)(void*, int);
					auto fnRemoveEffects = reinterpret_cast<tRemoveEffects>(game->m_BaseServer + kSrvFn_RemoveEffects);
					fnRemoveEffects(srvEnt, 0x20);
				}
				effects &= ~0x20u;
			}
		}

		if (game && game->m_EngineClient && game->m_EngineClient->IsInGame())
		{
			const int localIdx = game->m_EngineClient->GetLocalPlayer();
			void* localClientPlayer = (localIdx > 0) ? game->GetClientEntity(localIdx) : nullptr;
			if (localClientPlayer)
			{
				const uintptr_t cpAddr = reinterpret_cast<uintptr_t>(localClientPlayer);
				*reinterpret_cast<uint8_t*>(cpAddr + kCliPlayer_UsingVMGrab) = 0u;
				*reinterpret_cast<uint8_t*>(cpAddr + kCliPlayer_CarriedByLocal) = 0u;
				*reinterpret_cast<uint32_t*>(cpAddr + kCliPlayer_AttachedObj) = 0xFFFFFFFFu;
			}
		}
	}

	// Thread-safe check whether the opposite (non-holding) physical hand's calibrated palm skin
	// is within <= 6.5 Source units of the currently carried object's OBB for curl hand-to-hand transfer.
	bool IsHandNearHeldObjectForTransfer(VR* vr, bool physicalLeftHand)
	{
		if (!vr || !vr->m_IsVREnabled || !vr->m_IsHoldingObject)
			return false;
		if (vr->m_HoldingWithLeftHand == physicalLeftHand)
			return false;
		if (IsHandBusyWithInventory(physicalLeftHand))
			return false;

		int snapIdx = -1;
		bool snapHoldLeft = false;
		float R_rel[3][3]{};
		Vector wristLocalOrig(0.0f, 0.0f, 0.0f);
		Vector obbCenter(0.0f, 0.0f, 0.0f);
		Vector obbHalf(1.5f, 1.5f, 1.5f);
		unsigned long long snapMs = 0ULL;
		const unsigned long long nowMs = GetTickCount64();
		if (!s_HeldSnapshot.Read(snapIdx, snapHoldLeft, R_rel, wristLocalOrig, snapMs, &obbCenter, &obbHalf))
			return false;
		if (nowMs >= snapMs && (nowMs - snapMs) > 300ULL)
			return false;

		LocalGripPose gpHold{}, gpOther{};
		if (!EvaluateHandGripPose(vr, snapHoldLeft, gpHold) || !EvaluateHandGripPose(vr, physicalLeftHand, gpOther))
			return false;

		float R_obj[3][3]{};
		Mul3x3(gpHold.wristR, R_rel, R_obj);
		const Vector origWorld = TransformPoint3x4(gpHold.wristToWorld, wristLocalOrig);
		const Vector centerWorld = origWorld + RotateVec3x3(R_obj, obbCenter);
		const Vector dObj = InvRotateVec3x3(R_obj, gpOther.palmSkinWorldPos - centerWorld);
		const float ex = std::max(0.0f, std::fabs(dObj.x) - obbHalf.x);
		const float ey = std::max(0.0f, std::fabs(dObj.y) - obbHalf.y);
		const float ez = std::max(0.0f, std::fabs(dObj.z) - obbHalf.z);
		constexpr float kTransferReach = 6.5f;
		return (ex * ex + ey * ey + ez * ez) <= (kTransferReach * kTransferReach);
	}

	inline Vector Cross3(const Vector& a, const Vector& b)
	{
		return Vector(
			a.y * b.z - a.z * b.y,
			a.z * b.x - a.x * b.z,
			a.x * b.y - a.y * b.x);
	}

	inline bool IsFiniteVec3(const Vector& v)
	{
		return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
	}

	// Computes the hand's instantaneous pure linear & angular velocity averaged over the last 35 ms
	// (3-4 VR frames). Short window preserves exact arc-tangent direction with zero stale-arc lag.
	bool ComputeRecentHandVelocity(bool physicalLeftHand, Vector& outLinVelWorld, Vector& outAngVelWorldDeg)
	{
		outLinVelWorld = Vector(0.0f, 0.0f, 0.0f);
		outAngVelWorldDeg = Vector(0.0f, 0.0f, 0.0f);

		const uint64_t nowMs = GetTickCount64();
		constexpr uint64_t kShortWindowMs = 35ULL;

		Vector sumLin(0.0f, 0.0f, 0.0f);
		Vector sumAng(0.0f, 0.0f, 0.0f);
		float wSum = 0.0f;

		{
			std::lock_guard<std::mutex> lock(s_ThrowHistMutex);
			const HandThrowHistory& hist = s_ThrowHist[physicalLeftHand ? 1 : 0];
			for (int i = 0; i < hist.count; ++i)
			{
				const int idx = (hist.writeIdx - 1 - i + HandThrowHistory::kCapacity) % HandThrowHistory::kCapacity;
				const HandThrowSample& s = hist.samples[idx];
				if (s.tickMs == 0ULL || nowMs < s.tickMs || (nowMs - s.tickMs) > kShortWindowMs)
					continue;
				const float ageFrac = static_cast<float>(nowMs - s.tickMs) / static_cast<float>(kShortWindowMs);
				const float w = 1.0f - 0.4f * ageFrac;
				sumLin = sumLin + s.linVelWorld * w;
				sumAng = sumAng + s.angVelWorldDeg * w;
				wSum += w;
			}
		}

		if (wSum <= 1e-4f)
			return false;

		outLinVelWorld = sumLin * (1.0f / wSum);
		outAngVelWorldDeg = sumAng * (1.0f / wSum);
		return true;
	}

	bool QueryServerEntityPhysicsVelocity(void* srvEnt, Vector& outLinVel, Vector& outAngVel)
	{
		outLinVel = Vector(0.0f, 0.0f, 0.0f);
		outAngVel = Vector(0.0f, 0.0f, 0.0f);
		if (!srvEnt)
			return false;

		void* physObj = *reinterpret_cast<void* const*>(
			reinterpret_cast<uintptr_t>(srvEnt) + kSrvEnt_PhysicsObject);
		if (!physObj)
			return false;

		const uintptr_t pVtbl = *reinterpret_cast<const uintptr_t*>(physObj);
		if (!pVtbl)
			return false;

		typedef void(__thiscall* tGetVel)(void*, Vector*, Vector*);
		auto fnGetVel = *reinterpret_cast<tGetVel const*>(pVtbl + 0xD0u);
		if (!fnGetVel)
			return false;

		fnGetVel(physObj, &outLinVel, &outAngVel);
		if (!IsFiniteVec3(outLinVel) || !IsFiniteVec3(outAngVel))
			return false;
		return true;
	}

	// Restores the prop's exact release velocity (1:1, no artificial boost) onto all moveable IPhysicsObject(s)
	// immediately after CGrabController::DetachEntity so DetachEntity's PhysClearVelocity / ClampPhysicsVelocity
	// cannot strip the prop's existing momentum.
	void RestoreReleaseVelocityOnEntity(
		Game* game,
		void* srvEnt,
		const Vector& releaseLinVelWorld,
		const Vector& releaseAngVelLocalDeg)
	{
		if (!game || !srvEnt || !HasModuleRange(game->m_BaseServer, kSrvPhysEnvGlobalRva, sizeof(void*)))
			return;
		if (!IsFiniteVec3(releaseLinVelWorld))
			return;

		void* physEnv = *reinterpret_cast<void* const*>(game->m_BaseServer + kSrvPhysEnvGlobalRva);
		if (!physEnv)
			return;
		const uintptr_t envVtbl = *reinterpret_cast<const uintptr_t*>(physEnv);
		if (envVtbl)
		{
			typedef bool(__thiscall* tIsInSim)(void*);
			auto fnInSim = *reinterpret_cast<tIsInSim const*>(envVtbl + 0x13Cu);
			if (fnInSim && fnInSim(physEnv))
				return;
		}

		// Collect all IPhysicsObject* on the entity via VPhysicsGetObjectList (vtable +0x284)
		void* physList[16]{};
		int numPhys = 0;
		const uintptr_t entVtbl = *reinterpret_cast<const uintptr_t*>(srvEnt);
		if (entVtbl)
		{
			typedef int(__thiscall* tGetObjList)(void*, void**, int);
			auto fnGetList = *reinterpret_cast<tGetObjList const*>(entVtbl + kSrvEnt_VPhysObjListVt);
			if (fnGetList)
				numPhys = fnGetList(srvEnt, physList, 16);
		}
		if (numPhys <= 0)
		{
			void* singlePhys = *reinterpret_cast<void* const*>(
				reinterpret_cast<uintptr_t>(srvEnt) + kSrvEnt_PhysicsObject);
			if (singlePhys)
			{
				physList[0] = singlePhys;
				numPhys = 1;
			}
		}
		if (numPhys <= 0)
			return;

		Vector finalLinVel = releaseLinVelWorld;
		const float finalSpeed = Len3(finalLinVel);
		if (finalSpeed > 1000.0f)
			finalLinVel = finalLinVel * (1000.0f / finalSpeed);

		Vector finalAngVel = IsFiniteVec3(releaseAngVelLocalDeg)
			? releaseAngVelLocalDeg
			: Vector(0.0f, 0.0f, 0.0f);
		const float angMag = Len3(finalAngVel);
		if (angMag > 720.0f)
			finalAngVel = finalAngVel * (720.0f / angMag);

		typedef bool(__thiscall* tPhysBool)(void*);
		typedef void(__thiscall* tPhysVoid)(void*);
		typedef void(__thiscall* tSetVelInst)(void*, const Vector*, const Vector*);

		for (int i = 0; i < numPhys; ++i)
		{
			void* physObj = physList[i];
			if (!physObj)
				continue;
			const uintptr_t pVtbl = *reinterpret_cast<const uintptr_t*>(physObj);
			if (!pVtbl)
				continue;

			auto fnIsStatic   = *reinterpret_cast<tPhysBool const*>(pVtbl + 0x04u);
			auto fnIsMoveable = *reinterpret_cast<tPhysBool const*>(pVtbl + 0x28u);
			auto fnWake       = *reinterpret_cast<tPhysVoid const*>(pVtbl + 0x60u);
			auto fnSetVelInst = *reinterpret_cast<tSetVelInst const*>(pVtbl + kPhysObj_SetVelInstVt);
			if (!fnSetVelInst || (fnIsStatic && fnIsStatic(physObj)) || (fnIsMoveable && !fnIsMoveable(physObj)))
				continue;

			if (fnWake)
				fnWake(physObj);
			fnSetVelInst(physObj, &finalLinVel, &finalAngVel);
		}
	}
}

namespace VRGrab
{
	bool IsHoldingObject(const VR* vr)
	{
		return vr && vr->m_IsVREnabled && vr->m_IsHoldingObject;
	}

	void GetGimbalFreeControllerBasis(
		VR* vr,
		bool physicalLeftHand,
		const Vector& eyePosition,
		Vector& outPos,
		Vector& outFwd,
		Vector& outRight,
		Vector& outUp,
		QAngle& outAngles)
	{
		if (!vr)
		{
			outPos = Vector(0.0f, 0.0f, 0.0f);
			outFwd = Vector(1.0f, 0.0f, 0.0f);
			outRight = Vector(0.0f, -1.0f, 0.0f);
			outUp = Vector(0.0f, 0.0f, 1.0f);
			outAngles = QAngle(0.0f, 0.0f, 0.0f);
			return;
		}

		outPos = physicalLeftHand ? vr->GetLeftControllerAbsPos(eyePosition) : vr->GetRightControllerAbsPos(eyePosition);
		const Vector rawF = physicalLeftHand ? vr->m_LeftControllerForward : vr->m_RightControllerForward;
		const Vector rawR = physicalLeftHand ? vr->m_LeftControllerRight   : vr->m_RightControllerRight;
		const Vector rawU = physicalLeftHand ? vr->m_LeftControllerUp      : vr->m_RightControllerUp;
		const QAngle fbAng = physicalLeftHand ? vr->GetLeftControllerAbsAngle() : vr->GetRightControllerAbsAngle();

		EvaluateOrthonormalControllerBasis(rawF, rawR, rawU, fbAng, outFwd, outRight, outUp);
		QAngle::VectorAngles(outFwd, outUp, outAngles);
	}

	void NotifyHandCloseTargetProximity(bool physicalLeftHand, bool hasCloseCandidate)
	{
		if (IsHandBusyWithInventory(physicalLeftHand))
			hasCloseCandidate = false;
		HandCurlGripState& st = s_HandGrip[physicalLeftHand ? 1 : 0];
		st.hasCloseTarget.store(hasCloseCandidate, std::memory_order_relaxed);
		st.closeTargetMs.store(GetTickCount64(), std::memory_order_relaxed);
	}

	void NotifyServerUseControllerActive(bool active)
	{
		s_ServerUseCtrlActive.store(active, std::memory_order_relaxed);
	}

	bool EvaluateHandGripInput(VR* vr, bool physicalLeftHand, bool currentlyHoldingOnThisHand)
	{
		if (!vr || !vr->m_IsVREnabled)
			return false;

		std::lock_guard<std::mutex> gripLock(s_HandGripMutex);

		const int slot = physicalLeftHand ? 1 : 0;
		HandCurlGripState& st = s_HandGrip[slot];
		const unsigned long long nowMs = GetTickCount64();

		const bool digitalGrip = physicalLeftHand
			? vr->PressedDigitalAction(vr->m_ActionCrouch)
			: vr->PressedDigitalAction(vr->m_ActionReload);

		const bool usesVm0 = vr->m_LeftHanded ? physicalLeftHand : !physicalLeftHand;
		const bool handFreeForCurl = usesVm0 ? vr->m_RightHandEmpty : !vr->m_FlashlightEquipped;
		const bool useCtrlActive = s_ServerUseCtrlActive.load(std::memory_order_relaxed);
		const int skelState = (physicalLeftHand ? vr->m_SkeletalTrackingStateLeft : vr->m_SkeletalTrackingStateRight)
			.load(std::memory_order_acquire);
		const bool hasCloseTarget = st.hasCloseTarget.load(std::memory_order_relaxed);
		const float* curls = physicalLeftHand ? vr->m_LeftFingerCurl : vr->m_RightFingerCurl;
		const bool isHoldingAny = vr->m_IsHoldingObject;
		const bool transferring = isHoldingAny && !currentlyHoldingOnThisHand;

		if (!transferring &&
			st.hasEvalCache &&
			nowMs >= st.lastEvalTickMs &&
			(nowMs - st.lastEvalTickMs) < 2ULL &&
			st.lastDigitalGrip == digitalGrip &&
			st.lastHoldingOnThisHand == currentlyHoldingOnThisHand &&
			st.lastHandFreeForCurl == handFreeForCurl &&
			st.lastUseCtrlActive == useCtrlActive &&
			st.lastIsHoldingObject == isHoldingAny &&
			st.lastSkelState == skelState &&
			st.lastHasCloseTarget == hasCloseTarget &&
			std::fabs(st.lastCurls[0] - curls[2]) < 1e-4f &&
			std::fabs(st.lastCurls[1] - curls[3]) < 1e-4f &&
			std::fabs(st.lastCurls[2] - curls[4]) < 1e-4f)
		{
			return st.lastCachedResult;
		}

		bool armed = st.curlArmed.load(std::memory_order_relaxed);
		bool latched = st.curlLatched.load(std::memory_order_relaxed);
		bool sustained = st.curlSustained.load(std::memory_order_relaxed);
		bool graspUnopened = st.curlGraspUnopened.load(std::memory_order_relaxed);
		unsigned long long startMs = st.latchStartMs.load(std::memory_order_relaxed);
		unsigned long long graspStartMs = st.curlGraspStartMs.load(std::memory_order_relaxed);

		if ((handFreeForCurl || currentlyHoldingOnThisHand) && (skelState != 0))
		{
			const float meanMRP = (curls[2] + curls[3] + curls[4]) * (1.0f / 3.0f);
			const int count70 =
				(curls[2] >= 0.70f ? 1 : 0) +
				(curls[3] >= 0.70f ? 1 : 0) +
				(curls[4] >= 0.70f ? 1 : 0);

			if (meanMRP <= 0.48f)
				graspUnopened = false;

			if (latched)
			{
				const bool activeHold = currentlyHoldingOnThisHand || useCtrlActive;
				constexpr unsigned long long kUnattachedCurlExpireMs = 120ULL;
				if (meanMRP <= 0.42f)
				{
					if (sustained)
						s_LastPropDropMs.store(nowMs, std::memory_order_relaxed);
					latched = false;
					sustained = false;
					graspUnopened = false;
					armed = true;
				}
				else if (activeHold)
				{
					sustained = true;
				}
				else if (sustained)
				{
					s_LastPropDropMs.store(nowMs, std::memory_order_relaxed);
					latched = false;
					sustained = false;
					graspUnopened = false;
					armed = false;
				}
				else if (transferring &&
				         IsHandNearHeldObjectForTransfer(vr, physicalLeftHand))
				{
					startMs = nowMs;
				}
				else if (nowMs >= startMs && (nowMs - startMs) > kUnattachedCurlExpireMs)
				{
					latched = false;
					sustained = false;
					armed = false;
				}
			}
			else
			{
				if (meanMRP <= 0.50f)
				{
					armed = true;
				}
				else if (armed && meanMRP >= 0.76f && count70 >= 2)
				{
					// Edge-triggered closure: consume arming flag immediately. Only latch if close-touch
					// candidate is in range (or transferring a held prop / sustaining an active hold);
					// closing a fist in empty air cleanly disarms without firing IN_USE or far-ray!
					armed = false;
					const unsigned long long proxMs = st.closeTargetMs.load(std::memory_order_relaxed);
					const bool freshCloseProx =
						hasCloseTarget &&
						(nowMs < proxMs || (nowMs - proxMs) <= 300ULL);
					const bool canLatch =
						currentlyHoldingOnThisHand ||
						freshCloseProx ||
						IsHandNearHeldObjectForTransfer(vr, physicalLeftHand);
					if (canLatch)
					{
						latched = true;
						sustained = currentlyHoldingOnThisHand || useCtrlActive;
						startMs = nowMs;
						graspUnopened = true;
						graspStartMs = nowMs;
					}
				}
			}
		}
		else
		{
			if (sustained)
				s_LastPropDropMs.store(nowMs, std::memory_order_relaxed);
			latched = false;
			sustained = false;
			graspUnopened = false;
			armed = false;
		}

		st.curlArmed.store(armed, std::memory_order_relaxed);
		st.curlLatched.store(latched, std::memory_order_relaxed);
		st.curlSustained.store(sustained, std::memory_order_relaxed);
		st.curlGraspUnopened.store(graspUnopened, std::memory_order_relaxed);
		st.latchStartMs.store(startMs, std::memory_order_relaxed);
		st.curlGraspStartMs.store(graspStartMs, std::memory_order_relaxed);

		const bool result = digitalGrip || latched;
		st.hasEvalCache = true;
		st.lastEvalTickMs = nowMs;
		st.lastDigitalGrip = digitalGrip;
		st.lastHoldingOnThisHand = currentlyHoldingOnThisHand;
		st.lastHandFreeForCurl = handFreeForCurl;
		st.lastUseCtrlActive = useCtrlActive;
		st.lastIsHoldingObject = isHoldingAny;
		st.lastSkelState = skelState;
		st.lastHasCloseTarget = hasCloseTarget;
		st.lastCurls[0] = curls[2];
		st.lastCurls[1] = curls[3];
		st.lastCurls[2] = curls[4];
		st.lastCachedResult = result;
		return result;
	}

	void UpdateCreateMoveUseButton(VR* vr, CUserCmd* cmd)
	{
		if (!vr || !vr->m_IsVREnabled || !cmd)
			return;

		const unsigned long long nowMs = GetTickCount64();
		const unsigned long long lastDropMs = s_LastPropDropMs.load(std::memory_order_relaxed);
		const bool holding = vr->m_IsHoldingObject;
		const bool holdLeft = vr->m_HoldingWithLeftHand;
		const bool useCtrlBusy = s_ServerUseCtrlActive.load(std::memory_order_relaxed);

		const bool digR = vr->PressedDigitalAction(vr->m_ActionReload);
		const bool digL = vr->PressedDigitalAction(vr->m_ActionCrouch);
		EvaluateHandGripInput(vr, false, holding && !holdLeft);
		EvaluateHandGripInput(vr, true,  holding &&  holdLeft);
		const bool curlR = s_HandGrip[0].curlLatched.load(std::memory_order_relaxed);
		const bool curlL = s_HandGrip[1].curlLatched.load(std::memory_order_relaxed);

		const bool blockInvR = IsHandBusyWithInventory(false) &&
		                       !(holding && !holdLeft) &&
		                       !(useCtrlBusy && s_CreateMoveUse.handCycleActive[0] && s_CreateMoveUse.hadHoldInCycle[0]);
		const bool blockInvL = IsHandBusyWithInventory(true) &&
		                       !(holding && holdLeft) &&
		                       !(useCtrlBusy && s_CreateMoveUse.handCycleActive[1] && s_CreateMoveUse.hadHoldInCycle[1]);

		const bool rawDig[2]  = { blockInvR ? false : digR,  blockInvL ? false : digL  };
		const bool rawCurl[2] = { blockInvR ? false : curlR, blockInvL ? false : curlL };
		bool effectiveGrip[2]{ false, false };
		bool newHandEdge[2]{ false, false };

		for (int h = 0; h < 2; ++h)
		{
			const bool prevRawDig  = s_CreateMoveUse.prevDig[h];
			const bool prevRawCurl = s_CreateMoveUse.prevCurl[h];
			const bool digJustPressed  = rawDig[h]  && !prevRawDig;
			const bool curlJustPressed = rawCurl[h] && !prevRawCurl;

			const bool curlUnopened = s_HandGrip[h].curlGraspUnopened.load(std::memory_order_relaxed);
			const unsigned long long curlStart = s_HandGrip[h].curlGraspStartMs.load(std::memory_order_relaxed);
			const bool curlWindowAlive =
				curlUnopened &&
				!holding &&
				!useCtrlBusy &&
				(nowMs >= curlStart) &&
				((nowMs - curlStart) <= 450ULL);

			if (lastDropMs != 0ULL && lastDropMs >= s_CreateMoveUse.lastImpulseReleaseMs[h])
			{
				s_CreateMoveUse.lastImpulseReleaseMs[h] = 0ULL;
				s_CreateMoveUse.bounceBlocked[h] = false;
			}

			if (!rawDig[h])
				s_CreateMoveUse.staggeredSqueezeSuppressed[h] = false;

			if (!rawDig[h] && !rawCurl[h])
			{
				s_CreateMoveUse.bounceBlocked[h] = false;
				if (s_CreateMoveUse.handCycleActive[h] && !curlWindowAlive)
				{
					s_CreateMoveUse.handCycleActive[h] = false;
					s_CreateMoveUse.hadHoldInCycle[h] = false;
				}
			}
			else
			{
				if (holding || useCtrlBusy)
				{
					s_CreateMoveUse.staggeredSqueezeSuppressed[h] = false;
					s_CreateMoveUse.bounceBlocked[h] = false;
				}

				if (!s_CreateMoveUse.handCycleActive[h])
				{
					const unsigned long long lastRel = s_CreateMoveUse.lastImpulseReleaseMs[h];
					const bool inBounceWindow =
						!holding && !useCtrlBusy &&
						(lastRel != 0ULL) && (nowMs >= lastRel) && ((nowMs - lastRel) < 100ULL);
					if (inBounceWindow && (digJustPressed || curlJustPressed))
					{
						s_CreateMoveUse.bounceBlocked[h] = true;
					}
					else if (!s_CreateMoveUse.bounceBlocked[h])
					{
						s_CreateMoveUse.handCycleActive[h] = true;
						s_CreateMoveUse.hadHoldInCycle[h] = (holding || useCtrlBusy);
						newHandEdge[h] = true;
					}
				}
				else if (digJustPressed && !rawCurl[h] && curlWindowAlive && !holding && !useCtrlBusy)
				{
					// Same physical grasp: finger curl fired an impulse at 0..120ms and expired while fingers
					// remained closed, then the grip force sensor crossed threshold during follow-through.
					// Suppress digital grip until released so one grasp never fires two PlayerUse edges.
					s_CreateMoveUse.staggeredSqueezeSuppressed[h] = true;
				}
			}

			if (s_CreateMoveUse.handCycleActive[h] && (holding || useCtrlBusy))
				s_CreateMoveUse.hadHoldInCycle[h] = true;

			const bool digEff  = rawDig[h]  && !s_CreateMoveUse.bounceBlocked[h] && !s_CreateMoveUse.staggeredSqueezeSuppressed[h];
			const bool curlEff = rawCurl[h] && !s_CreateMoveUse.bounceBlocked[h];
			effectiveGrip[h] = digEff || curlEff;

			if (s_CreateMoveUse.prevEffGrip[h] && !effectiveGrip[h] &&
			    !s_CreateMoveUse.hadHoldInCycle[h] && !holding && !useCtrlBusy)
			{
				s_CreateMoveUse.lastImpulseReleaseMs[h] = nowMs;
			}

			s_CreateMoveUse.prevDig[h] = rawDig[h];
			s_CreateMoveUse.prevCurl[h] = rawCurl[h];
			s_CreateMoveUse.prevEffGrip[h] = effectiveGrip[h];
		}

		const bool anyGrip = effectiveGrip[0] || effectiveGrip[1];
		const bool newRisingEdge = newHandEdge[0] || newHandEdge[1];

		if (!anyGrip && !s_CreateMoveUse.reassertNextCmd)
		{
			cmd->buttons &= ~IN_USE;
			s_CreateMoveUse.prevInUseSent = false;
			s_LastSentInUse.store(false, std::memory_order_relaxed);
			return;
		}

		// CBasePlayer::PlayerUse (server.dll+0xDBCF9) gates FCAP_IMPULSE_USE pickup/switch/door activation on
		// m_afButtonPressed & IN_USE (0 -> 1 rising edge). If IN_USE was already 1 on the previous CreateMove
		// while not holding an object/use-controller (e.g. one hand held digital Grip in empty air) and a new
		// 0 -> 1 grip edge occurs on the opposite hand, drop IN_USE to 0 for 1 CUserCmd then re-assert 1 next Cmd.
		if (s_CreateMoveUse.prevInUseSent && !holding && !useCtrlBusy && newRisingEdge)
		{
			cmd->buttons &= ~IN_USE;
			s_CreateMoveUse.prevInUseSent = false;
			s_CreateMoveUse.reassertNextCmd = true;
			s_LastSentInUse.store(false, std::memory_order_relaxed);
			return;
		}

		if (s_CreateMoveUse.reassertNextCmd)
		{
			s_CreateMoveUse.reassertNextCmd = false;
			if (anyGrip)
			{
				if (effectiveGrip[1] && curlL && !s_HandGrip[1].curlSustained.load(std::memory_order_relaxed))
					s_HandGrip[1].latchStartMs.store(nowMs, std::memory_order_relaxed);
				if (effectiveGrip[0] && curlR && !s_HandGrip[0].curlSustained.load(std::memory_order_relaxed))
					s_HandGrip[0].latchStartMs.store(nowMs, std::memory_order_relaxed);
				cmd->buttons |= IN_USE;
				s_CreateMoveUse.prevInUseSent = true;
				s_LastSentInUse.store(true, std::memory_order_relaxed);
			}
			else
			{
				cmd->buttons &= ~IN_USE;
				s_CreateMoveUse.prevInUseSent = false;
				s_LastSentInUse.store(false, std::memory_order_relaxed);
			}
			return;
		}

		cmd->buttons |= IN_USE;
		s_CreateMoveUse.prevInUseSent = true;
		s_LastSentInUse.store(true, std::memory_order_relaxed);
	}

	bool IsDebouncedUseGripDown(VR* vr)
	{
		if (!vr || !vr->m_IsVREnabled)
			return false;
		return s_LastSentInUse.load(std::memory_order_relaxed);
	}

	bool GetCalibratedHandSelectPose(
		VR* vr,
		bool physicalLeftHand,
		const Vector& eyePosition,
		Vector& outPalmSkinPos,
		Vector& outPinchPos,
		Vector& outPointDir)
	{
		LocalGripPose gp{};
		if (EvaluateHandGripPoseAtEye(vr, physicalLeftHand, eyePosition, gp))
		{
			outPalmSkinPos = gp.palmSkinWorldPos;
			const Vector localPinch = gp.isLeftHandMesh
				? Vector(4.85f, 1.32f, -0.78f)
				: Vector(4.80f, 1.18f, 0.55f);
			outPinchPos = TransformPoint3x4(gp.wristToWorld, localPinch);
			const Vector ptrFallback = physicalLeftHand ? vr->m_LeftPointerFwd : vr->m_RightPointerFwd;
			outPointDir = NormalizeSafe(
				gp.fingerDistalDir * 0.88f + gp.palmOutwardNormal * 0.22f,
				NormalizeSafe(ptrFallback, Vector(1.0f, 0.0f, 0.0f)));
			return true;
		}

		if (vr)
		{
			const Vector ptrPos = physicalLeftHand ? vr->GetLeftPointerAbsPos() : vr->GetRightPointerAbsPos();
			const Vector ptrFwd = NormalizeSafe(
				physicalLeftHand ? vr->m_LeftPointerFwd : vr->m_RightPointerFwd,
				Vector(1.0f, 0.0f, 0.0f));
			outPalmSkinPos = ptrPos;
			outPinchPos = ptrPos + ptrFwd * 1.5f;
			outPointDir = ptrFwd;
		}
		return false;
	}

	void OnObjectDropped(VR* vr)
	{
		if (vr)
			vr->m_IsHoldingObject = false;

		s_SrvGrab.lastGrabCtrl = nullptr;
		s_SrvGrab.lastEHandle = 0xFFFFFFFFu;
		s_SrvGrab.heldEntIndex = -1;
		s_SrvGrab.hasLocalCenter = false;
		s_SrvGrab.hasServerOBB = false;
		s_SrvGrab.hasCalibratedSeating = false;
		s_SrvGrab.yawInitialized = false;
		s_SrvGrab.seatDurationMs = 0;
		s_SrvGrab.hasSettledInHand = false;
		s_SrvGrab.attachTickMs = 0;
		s_SrvGrab.wasWallBlocked = false;
		s_SrvGrab.hasLastTarget = false;
		s_SrvGrab.ClearPropVel();
		s_HeldSnapshot.Clear();
#if VR_GRAB_HAS_VR_HANDS
		VRHands::ClearHeldHandPhysicsFeedback();
#endif

		ResetClientCurlSlot(0, false);
		ResetClientCurlSlot(1, true);
		for (int i = 0; i < 2; ++i)
		{
			s_HandGrip[i].curlGraspUnopened.store(false, std::memory_order_relaxed);
			s_HandGrip[i].curlGraspStartMs.store(0ULL, std::memory_order_relaxed);
		}
		s_LastPropDropMs.store(GetTickCount64(), std::memory_order_relaxed);
		s_LastServerImpulseUse = LastServerImpulseUse{};
	}

	void RecordHandTrackingVelocity(VR* vr)
	{
		if (!vr || !vr->m_IsVREnabled)
			return;

		const uint64_t nowMs = GetTickCount64();
		const float vrScale = (vr->m_VRScale > 1.0f && vr->m_VRScale < 300.0f) ? vr->m_VRScale : 43.2f;
		const float yawOffDeg = vr->m_RotationOffset.y;

		for (int handIdx = 0; handIdx < 2; ++handIdx)
		{
			const bool physicalLeft = (handIdx == 1);
			const TrackedDevicePoseData& pose = physicalLeft ? vr->m_LeftControllerPose : vr->m_RightControllerPose;
			Vector rawVel = pose.TrackedDeviceVel;
			Vector rawAngVelDeg(pose.TrackedDeviceAngVel.x, pose.TrackedDeviceAngVel.y, pose.TrackedDeviceAngVel.z);
			if (!IsFiniteVec3(rawVel) || !IsFiniteVec3(rawAngVelDeg))
				continue;

			// Rotate pure OpenVR tracking-space velocities into Source world XY orientation and scale to Source units/s.
			VectorPivotXY(rawVel, Vector(0.0f, 0.0f, 0.0f), yawOffDeg);
			VectorPivotXY(rawAngVelDeg, Vector(0.0f, 0.0f, 0.0f), yawOffDeg);

			Vector linVelWorld = rawVel * vrScale;

			// Clamp single-sample spikes against tracking glitches
			const float sp = Len3(linVelWorld);
			if (sp > 1200.0f)
				linVelWorld = linVelWorld * (1200.0f / sp);

			std::lock_guard<std::mutex> lock(s_ThrowHistMutex);
			s_ThrowHist[handIdx].Push(nowMs, linVelWorld, rawAngVelDeg);
		}
	}

	void ReleaseCarriedObject(Game* game, VR* vr, void* grabCtrl, void* pServerPlayer)
	{
		if (!vr)
			return;

		// 1. Capture the carried entity handle & its LIVE VPhysics velocity BEFORE ForceDropOfCarriedPhysObjects
		//    runs DetachEntity (which would otherwise zero or clamp velocity to 1.5 * m_flMaxspeed).
		uint32_t savedEh = 0xFFFFFFFFu;
		if (grabCtrl)
		{
			savedEh = *reinterpret_cast<const uint32_t*>(
				reinterpret_cast<uintptr_t>(grabCtrl) + kGrabOff_AttachedEnt);
		}
		if (savedEh == 0xFFFFFFFFu && pServerPlayer)
		{
			savedEh = *reinterpret_cast<const uint32_t*>(
				reinterpret_cast<uintptr_t>(pServerPlayer) + 0x189Cu); // CINFRA_Player::m_hHeldObject
		}
		if (savedEh == 0xFFFFFFFFu)
		{
			savedEh = s_SrvGrab.lastEHandle;
		}

		const bool wasHoldingLeft = vr->m_HoldingWithLeftHand;
		const bool wasWallBlocked = s_SrvGrab.wasWallBlocked;
		const uint64_t nowMs = GetTickCount64();

		bool hasReleaseVel = false;
		Vector releaseLinVel(0.0f, 0.0f, 0.0f);
		Vector releaseAngVelLocal(0.0f, 0.0f, 0.0f);

		void* preDropEnt = (game && savedEh != 0xFFFFFFFFu)
			? ResolveServerEntityByEHandle(game, savedEh)
			: nullptr;
		if (preDropEnt)
		{
			const uint32_t eFlags = *reinterpret_cast<const uint32_t*>(
				reinterpret_cast<uintptr_t>(preDropEnt) + kSrvEnt_EFlags);
			if ((eFlags & 0x1u) == 0u)
			{
				Vector liveLin(0.0f, 0.0f, 0.0f), liveAng(0.0f, 0.0f, 0.0f);
				if (QueryServerEntityPhysicsVelocity(preDropEnt, liveLin, liveAng))
				{
					releaseLinVel = liveLin;
					releaseAngVelLocal = liveAng;
					hasReleaseVel = true;

					// If the user's fingers took 1-2 ticks (<=45 ms) to open and the prop just started
					// decelerating along the same direction, keep the prop's own pre-deceleration velocity.
					const float liveSpeed = Len3(liveLin);
					const Vector liveDir = NormalizeSafe(liveLin, Vector(0.0f, 0.0f, 0.0f));
					float bestSpeed = liveSpeed;
					for (int i = 0; i < s_SrvGrab.propVelCount; ++i)
					{
						const int idx = (s_SrvGrab.propVelWriteIdx - 1 - i + ServerGrabSession::kPropVelHistCap) %
							ServerGrabSession::kPropVelHistCap;
						const auto& s = s_SrvGrab.propVelHist[idx];
						if (s.tickMs == 0ULL || nowMs < s.tickMs || (nowMs - s.tickMs) > 45ULL)
							continue;
						const float sp = Len3(s.linVel);
						if (sp > bestSpeed)
						{
							const bool sameDir = (liveSpeed < 25.0f) ||
								(Dot3(NormalizeSafe(s.linVel, liveDir), liveDir) > 0.55f);
							if (sameDir)
							{
								bestSpeed = sp;
								releaseLinVel = s.linVel;
								releaseAngVelLocal = s.angVel;
							}
						}
					}
				}

				// Fallback ONLY if the prop is not wall-blocked and its VPhysics velocity lagged noticeably
				// behind the hand's instantaneous 35 ms linear velocity (+ player velocity) — pure 1:1, no boost.
				if (!wasWallBlocked)
				{
					Vector handLin(0.0f, 0.0f, 0.0f), handAngDeg(0.0f, 0.0f, 0.0f);
					if (ComputeRecentHandVelocity(wasHoldingLeft, handLin, handAngDeg))
					{
						Vector playerVel(0.0f, 0.0f, 0.0f);
						if (pServerPlayer)
						{
							const Vector rawPV = *reinterpret_cast<const Vector*>(
								reinterpret_cast<uintptr_t>(pServerPlayer) + kSrvEnt_AbsVelocity);
							if (IsFiniteVec3(rawPV) && Len3(rawPV) < 800.0f)
								playerVel = rawPV;
						}
						const Vector handTotalVel = handLin + playerVel;
						const float handTotalSpeed = Len3(handTotalVel);
						const float propSpeed = hasReleaseVel ? Len3(releaseLinVel) : 0.0f;
						if (!hasReleaseVel || propSpeed < handTotalSpeed * 0.70f)
						{
							releaseLinVel = handTotalVel;
							const matrix3x4_t& coord = *reinterpret_cast<const matrix3x4_t*>(
								reinterpret_cast<uintptr_t>(preDropEnt) + kSrvEnt_CoordFrame);
							float R_obj[3][3]{};
							for (int r = 0; r < 3; ++r)
							{
								for (int c = 0; c < 3; ++c)
									R_obj[r][c] = coord[r][c];
							}
							releaseAngVelLocal = InvRotateVec3x3(R_obj, handAngDeg);
							hasReleaseVel = true;
						}
					}
				}
			}
		}

		const bool shouldForceDrop = pServerPlayer && (
			grabCtrl != nullptr ||
			*reinterpret_cast<const uint32_t*>(reinterpret_cast<uintptr_t>(pServerPlayer) + 0x189Cu) != 0xFFFFFFFFu ||
			*reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(pServerPlayer) + 0x1668u) != 0u);

		// 2. Let Source cleanly detach the shadow controller and restore mass/gravity/collision flags.
		if (shouldForceDrop)
		{
			const uintptr_t playerVtbl = *reinterpret_cast<const uintptr_t*>(pServerPlayer);
			if (playerVtbl)
			{
				typedef void(__thiscall* tForceDrop)(void*, void*);
				auto fnForceDrop = *reinterpret_cast<tForceDrop const*>(playerVtbl + 0x6E8u);
				if (fnForceDrop)
					fnForceDrop(pServerPlayer, nullptr);
			}
		}

		// 3. Immediately restore the prop's exact pre-drop velocity onto the detached IPhysicsObject(s)
		//    so DetachEntity's PhysClearVelocity / ClampPhysicsVelocity cannot strip its momentum.
		int droppedEntIdx = -1;
		if (game && savedEh != 0xFFFFFFFFu)
		{
			void* srvEnt = ResolveServerEntityByEHandle(game, savedEh);
			if (srvEnt)
			{
				const uint32_t eFlags = *reinterpret_cast<const uint32_t*>(
					reinterpret_cast<uintptr_t>(srvEnt) + kSrvEnt_EFlags);
				if ((eFlags & 0x1u) == 0u) // not EFL_KILLME
				{
					droppedEntIdx = ResolveServerEntityIndex(game, srvEnt, savedEh);
					if (hasReleaseVel)
					{
						RestoreReleaseVelocityOnEntity(game, srvEnt, releaseLinVel, releaseAngVelLocal);
					}
				}
			}
		}

		s_LastDroppedEntIdx.store(droppedEntIdx, std::memory_order_relaxed);
		OnObjectDropped(vr);
	}

	void FilterCreateMoveButtons(VR* vr, CUserCmd* cmd)
	{
		if (!vr || !vr->m_IsVREnabled || !cmd)
			return;

		// In CPlayerPickupController::Use (server.dll+0x3A7FE8), m_nButtons & IN_ATTACK (bit 0)
		// immediately aborts UpdateObject, calls Shutdown(true), and punts the carried prop forward
		// along EyeVectors at player_throwforce (1000+ u/s). In VR, closing the right index finger
		// pulls Right Trigger (m_ActionPrimaryAttack -> IN_ATTACK), so suppress IN_ATTACK while holding.
		if (vr->m_IsHoldingObject)
		{
			cmd->buttons &= ~IN_ATTACK;
		}
	}

	void MaintainServerGrabState(Game* game, VR* vr, void* grabCtrl, void* pServerPlayer)
	{
		if (!vr || !vr->m_IsVREnabled)
			return;

		NeutralizeVMGrabAndClientClone(game, vr, grabCtrl, pServerPlayer);

		if (pServerPlayer && vr->m_IsHoldingObject)
		{
			*reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(pServerPlayer) + kSrvPlayer_Buttons) &= ~IN_ATTACK;
		}

		if (grabCtrl)
		{
			// CGrabController::ComputeError (server.dll+0x3A7829) checks:
			//   if (0.0f >= m_errorTime [+0x2038]) return 0.0f;
			// Keeping m_errorTime at -1000.0f guarantees ComputeError() returns 0.0f in BOTH
			// unhooked CPlayerPickupController::Use (+0x3A7FA0, 40.0f threshold) and UpdateObject (+0x3A9B53),
			// preventing forced drops when tilting the VR hand >70 degrees backwards or behind the HMD.
			*reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(grabCtrl) + kGrabOff_ErrorTime) = -1000.0f;
			*reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(grabCtrl) + kGrabOff_Error) = 0.0f;
			*reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(grabCtrl) + kGrabOff_AngleAlign) = 1.0f;

			float& maxAng = *reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(grabCtrl) + kGrabOff_MaxAngular);
			float& maxDamp = *reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(grabCtrl) + kGrabOff_MaxDampAng);
			if (maxAng < 3600.0f)
				maxAng = 3600.0f;
			if (maxDamp < 3600.0f)
				maxDamp = 3600.0f;

			uint32_t eh = *reinterpret_cast<const uint32_t*>(reinterpret_cast<uintptr_t>(grabCtrl) + kGrabOff_AttachedEnt);
			if (eh != 0xFFFFFFFFu)
			{
				void* srvEnt = ResolveServerEntityByEHandle(game, eh);
				WakeServerEntityPhysics(srvEnt);
			}
		}
	}

	// Moves carry ownership to the opposite physical hand without dropping the prop or snapping it:
	// the object's current world transform is re-expressed in the new wrist frame and seated smoothly onto skin.
	static bool TransferCarryToHand(Game* game, VR* vr, bool newHoldLeft)
	{
		LocalGripPose gpNew{};
		if (!EvaluateHandGripPose(vr, newHoldLeft, gpNew))
			return false;

		void* srvEnt = (s_SrvGrab.lastEHandle != 0xFFFFFFFFu)
			? ResolveServerEntityByEHandle(game, s_SrvGrab.lastEHandle)
			: nullptr;
		if (!srvEnt || !s_SrvGrab.hasLocalCenter)
			return false;

		const matrix3x4_t& coord = *reinterpret_cast<const matrix3x4_t*>(
			reinterpret_cast<uintptr_t>(srvEnt) + kSrvEnt_CoordFrame);
		float R_obj[3][3]{};
		for (int r = 0; r < 3; ++r)
		{
			for (int c = 0; c < 3; ++c)
				R_obj[r][c] = coord[r][c];
		}
		const Vector origin(coord[0][3], coord[1][3], coord[2][3]);
		const Vector centerWorld = origin + RotateVec3x3(R_obj, s_SrvGrab.localOBBCenter);

		MulTransposeA3x3(gpNew.wristR, R_obj, s_SrvGrab.R_rel);
		const Vector initLocal = InvTransformPointOrtho3x4(gpNew.wristToWorld, centerWorld);
		s_SrvGrab.initialWristLocalCenter = initLocal;
		if (s_SrvGrab.isLargeObject)
		{
			s_SrvGrab.targetWristLocalCenter = initLocal;
			s_SrvGrab.seatDurationMs = 0ULL;
		}
		else
		{
			const Vector ps = GetPalmSkinLocalCoords(gpNew.isLeftHandMesh, gpNew.usePhoneOpenTable);
			const float effHalfThick = ComputeEffectivePalmHalfThickness(R_obj, s_SrvGrab.obbHalfExtents, gpNew.wristToWorld);
			s_SrvGrab.targetWristLocalCenter = Vector(
				ClampF(initLocal.x, 2.45f, 5.05f),
				ps.y + effHalfThick,
				ClampF(initLocal.z, ps.z - 1.25f, ps.z + 1.25f));
			s_SrvGrab.seatDurationMs = 80ULL;
		}
		s_SrvGrab.hasCalibratedSeating = true;
		s_SrvGrab.hasSettledInHand = true;
		s_SrvGrab.attachTickMs = GetTickCount64();
		s_SrvGrab.wasWallBlocked = false;
		s_SrvGrab.hasLastTarget = false;

		vr->m_HoldingWithLeftHand = newHoldLeft;
		const int newSlot = newHoldLeft ? 1 : 0;
		const int oldSlot = newHoldLeft ? 0 : 1;
		if (s_HandGrip[newSlot].curlLatched.load(std::memory_order_relaxed))
			s_HandGrip[newSlot].curlSustained.store(true, std::memory_order_relaxed);
		s_HandGrip[oldSlot].curlSustained.store(false, std::memory_order_relaxed);
		s_HeldSnapshot.Clear();
#if VR_GRAB_HAS_VR_HANDS
		VRHands::ClearHeldHandPhysicsFeedback();
#endif
		ResetClientCurlSlot(0, false);
		ResetClientCurlSlot(1, true);
		return true;
	}

	bool EvaluateCarryGripHeld(Game* game, VR* vr, void* /*pServerPlayer*/)
	{
		if (!vr || !vr->m_IsVREnabled)
			return true;

		const bool holdLeft = vr->m_HoldingWithLeftHand;
		const bool leftGrip = EvaluateHandGripInput(vr, true, vr->m_IsHoldingObject && holdLeft);
		const bool rightGrip = EvaluateHandGripInput(vr, false, vr->m_IsHoldingObject && !holdLeft);
		if (!vr->m_IsHoldingObject)
			return (leftGrip && !IsHandBusyWithInventory(true)) || (rightGrip && !IsHandBusyWithInventory(false));

		if (holdLeft ? leftGrip : rightGrip)
			return true;

		// Holding hand opened: transfer only if the opposite hand is gripping right at the held prop and not busy with inventory.
		const bool otherGrip = holdLeft ? rightGrip : leftGrip;
		if (!otherGrip || IsHandBusyWithInventory(!holdLeft))
			return false;

		void* srvEnt = (s_SrvGrab.lastEHandle != 0xFFFFFFFFu)
			? ResolveServerEntityByEHandle(game, s_SrvGrab.lastEHandle)
			: nullptr;
		if (!srvEnt)
			return false;

		Vector mins(0.0f, 0.0f, 0.0f), maxs(0.0f, 0.0f, 0.0f);
		if (!QueryServerCollideableBounds(srvEnt, mins, maxs))
			return false;

		LocalGripPose gpOther{};
		if (!EvaluateHandGripPose(vr, !holdLeft, gpOther))
			return false;

		const matrix3x4_t& coord = *reinterpret_cast<const matrix3x4_t*>(
			reinterpret_cast<uintptr_t>(srvEnt) + kSrvEnt_CoordFrame);
		const Vector local = InvTransformPointOrtho3x4(coord, gpOther.palmSkinWorldPos);
		const float dx = std::max(0.0f, std::max(mins.x - local.x, local.x - maxs.x));
		const float dy = std::max(0.0f, std::max(mins.y - local.y, local.y - maxs.y));
		const float dz = std::max(0.0f, std::max(mins.z - local.z, local.z - maxs.z));
		constexpr float kTransferReach = 6.5f;
		if (std::sqrt(dx * dx + dy * dy + dz * dz) > kTransferReach)
			return false;

		return TransferCarryToHand(game, vr, !holdLeft);
	}

	bool BeginPickupUpdate(
		Game* game,
		VR* vr,
		void* grabCtrl,
		void* pServerPlayer,
		VRGrabUpdateScope& outScope,
		float& inOutMaxError)
	{
		outScope = VRGrabUpdateScope{};
		if (!vr || !vr->m_IsVREnabled)
			return true;

		const bool leftGrip = EvaluateHandGripInput(vr, true, vr->m_IsHoldingObject && vr->m_HoldingWithLeftHand);
		const bool rightGrip = EvaluateHandGripInput(vr, false, vr->m_IsHoldingObject && !vr->m_HoldingWithLeftHand);
		const bool gripHeld = EvaluateCarryGripHeld(game, vr, pServerPlayer);

		if (!gripHeld)
		{
			ReleaseCarriedObject(game, vr, grabCtrl, pServerPlayer);
			return false;
		}

		if (!vr->m_IsHoldingObject)
		{
			const bool leftCanGrab = leftGrip && !IsHandBusyWithInventory(true);
			const bool rightCanGrab = rightGrip && !IsHandBusyWithInventory(false);
			if (!leftCanGrab && !rightCanGrab)
			{
				ReleaseCarriedObject(game, vr, grabCtrl, pServerPlayer);
				return false;
			}
			vr->m_HoldingWithLeftHand = (leftCanGrab != rightCanGrab)
				? leftCanGrab
				: (vr->m_LeftHanded ? !vr->m_UseWithLeftHand : vr->m_UseWithLeftHand);
		}

		MaintainServerGrabState(game, vr, grabCtrl, pServerPlayer);

		outScope.grabCtrl = grabCtrl;
		outScope.serverPlayer = pServerPlayer;
		outScope.prevOverrideEyeAngles = vr->m_OverrideEyeAngles;
		outScope.prevInPickupUpdate = vr->m_InPickupUpdate;
		outScope.preTargetPos = grabCtrl
			? *reinterpret_cast<const Vector*>(reinterpret_cast<uintptr_t>(grabCtrl) + kGrabOff_TargetPos)
			: Vector(0.0f, 0.0f, 0.0f);
		outScope.preTargetAng = grabCtrl
			? *reinterpret_cast<const QAngle*>(reinterpret_cast<uintptr_t>(grabCtrl) + kGrabOff_TargetAng)
			: QAngle(0.0f, 0.0f, 0.0f);
		outScope.active = true;

		s_SrvGrab.setTargetCalledInUpdate = false;
		vr->m_OverrideEyeAngles = true;
		vr->m_InPickupUpdate = true;
		inOutMaxError = 999999.0f;
		return true;
	}

	void NotifySetTargetPositionCalled()
	{
		s_SrvGrab.setTargetCalledInUpdate = true;
	}

	bool WasSetTargetPositionCalledInScope()
	{
		return s_SrvGrab.setTargetCalledInUpdate;
	}

	void EndPickupUpdate(
		Game* game,
		VR* vr,
		bool updateSucceeded,
		bool /*bIsTeleport*/,
		VRGrabUpdateScope& scope)
	{
		if (!scope.active || !vr)
			return;

		if (updateSucceeded && scope.grabCtrl && vr->m_IsVREnabled)
		{
			MaintainServerGrabState(game, vr, scope.grabCtrl, scope.serverPlayer);

			// Normalize m_angCarriedObject_CurAngToTargetAng (+0x13C8) into [-180, +180] per axis so
			// UpdateObject's raw (targetAng - entityAngles) subtraction never produces +/-360 deg spin spikes
			// across +/-180 deg Euler yaw/roll branch cuts.
			void* srvPlayer = scope.serverPlayer;
			if (!srvPlayer && game)
			{
				const uint32_t playerEh = *reinterpret_cast<const uint32_t*>(
					reinterpret_cast<uintptr_t>(scope.grabCtrl) + kGrabOff_PlayerHandle);
				srvPlayer = ResolveServerEntityByEHandle(game, playerEh);
			}
			if (srvPlayer)
			{
				QAngle& carryAng = *reinterpret_cast<QAngle*>(
					reinterpret_cast<uintptr_t>(srvPlayer) + kSrvPlayer_CarryOffAng);
				carryAng.x = WrapDeg180(carryAng.x);
				carryAng.y = WrapDeg180(carryAng.y);
				carryAng.z = WrapDeg180(carryAng.z);
			}
		}

		vr->m_InPickupUpdate = scope.prevInPickupUpdate;
		if (!scope.prevOverrideEyeAngles)
			vr->m_OverrideEyeAngles = false;
		scope.active = false;
	}

	Vector GetPickupShootPosition(VR* vr)
	{
		if (!vr || !vr->m_IsVREnabled)
			return Vector(0.0f, 0.0f, 0.0f);

		LocalGripPose gp{};
		if (EvaluateHandGripPose(vr, vr->m_HoldingWithLeftHand, gp))
			return gp.gripWorldPos;

		return vr->m_HoldingWithLeftHand ? vr->GetLeftPointerAbsPos() : vr->GetRightPointerAbsPos();
	}

	QAngle& GetPickupEyeAngles(VR* vr)
	{
		if (!vr || !vr->m_IsVREnabled)
			return s_SrvGrab.cachedPickupEyeAngles;

		LocalGripPose gp{};
		if (EvaluateHandGripPose(vr, vr->m_HoldingWithLeftHand, gp))
		{
			// Track horizontal yaw smoothly without flipping 180 degrees when the hand tilts past vertical
			// (>70..180 degrees backwards). When fingerDistalDir goes nearly vertical or inverted relative to
			// the previous horizontal direction, blend with palmOutwardNormal / HMD forward and enforce
			// hemisphere continuity against prevHorizontalDir so ComputePlayerMatrix never snaps 180 deg.
			Vector horiz(gp.fingerDistalDir.x, gp.fingerDistalDir.y, 0.0f);
			float horizLenSq = horiz.x * horiz.x + horiz.y * horiz.y;

			if (!s_SrvGrab.yawInitialized)
			{
				Vector initDir(vr->m_HmdForward.x, vr->m_HmdForward.y, 0.0f);
				if (horizLenSq > 0.12f)
					initDir = horiz;
				initDir = NormalizeSafe(initDir, Vector(1.0f, 0.0f, 0.0f));
				s_SrvGrab.prevHorizontalDir = initDir;
				s_SrvGrab.stablePickupYaw = std::atan2(initDir.y, initDir.x) * kRad2Deg;
				s_SrvGrab.yawInitialized = true;
			}
			else if (horizLenSq > 0.16f) // > ~66 deg from vertical: update only if not an abrupt inversion
			{
				float invLen = 1.0f / std::sqrt(horizLenSq);
				Vector cand(horiz.x * invLen, horiz.y * invLen, 0.0f);
				float cosDelta = cand.x * s_SrvGrab.prevHorizontalDir.x + cand.y * s_SrvGrab.prevHorizontalDir.y;
				if (cosDelta > 0.25f)
				{
					Vector blended = NormalizeSafe(
						s_SrvGrab.prevHorizontalDir * 0.65f + cand * 0.35f,
						s_SrvGrab.prevHorizontalDir);
					s_SrvGrab.prevHorizontalDir = blended;
					s_SrvGrab.stablePickupYaw = std::atan2(blended.y, blended.x) * kRad2Deg;
				}
			}

			// Pitch = 0.0f, Roll = 0.0f keeps UpdateObject (+0x3A9A40) & ComputePlayerMatrix (+0x3A6D90)
			// strictly away from the +/-75 deg pitch clamp and guarantees up.z == +1.0 > 0 at every hand tilt.
			s_SrvGrab.cachedPickupEyeAngles = QAngle(0.0f, s_SrvGrab.stablePickupYaw, 0.0f);
			return s_SrvGrab.cachedPickupEyeAngles;
		}

		s_SrvGrab.cachedPickupEyeAngles = QAngle(0.0f, vr->m_HmdAngAbs.y, 0.0f);
		return s_SrvGrab.cachedPickupEyeAngles;
	}

	bool ComputeTargetTransform(
		Game* game,
		VR* vr,
		void* grabCtrl,
		void* fallbackServerEnt,
		const Vector& engineTargetPos,
		const QAngle& engineTargetAng,
		bool bIsTeleport,
		Vector& outTargetPos,
		QAngle& outTargetAng)
	{
		if (!vr || !vr->m_IsVREnabled || !grabCtrl)
			return false;

		NotifySetTargetPositionCalled();
		const bool wasHolding = vr->m_IsHoldingObject;
		vr->m_IsHoldingObject = true;
		vr->m_LastGrabUpdateTime = std::chrono::steady_clock::now();

		const uint32_t eh = *reinterpret_cast<const uint32_t*>(
			reinterpret_cast<uintptr_t>(grabCtrl) + kGrabOff_AttachedEnt);

		// AttachEntity (server.dll+0x3A91C9) invokes SetTargetPosition while m_InPickupUpdate is false
		// (immediately before writing m_attachedEntity at +0x2054). Detect fresh attachment cleanly without
		// resetting s_R_rel on normal continuation ticks.
		const bool isInitialAttach =
			!vr->m_InPickupUpdate ||
			grabCtrl != s_SrvGrab.lastGrabCtrl ||
			(eh != 0xFFFFFFFFu && s_SrvGrab.lastEHandle != 0xFFFFFFFFu && eh != s_SrvGrab.lastEHandle);

		if (isInitialAttach)
		{
			const bool leftGrip = EvaluateHandGripInput(vr, true, wasHolding && vr->m_HoldingWithLeftHand);
			const bool rightGrip = EvaluateHandGripInput(vr, false, wasHolding && !vr->m_HoldingWithLeftHand);
			const bool leftCanGrab = leftGrip && (wasHolding || !IsHandBusyWithInventory(true));
			const bool rightCanGrab = rightGrip && (wasHolding || !IsHandBusyWithInventory(false));
			vr->m_HoldingWithLeftHand = (leftCanGrab != rightCanGrab)
				? leftCanGrab
				: (vr->m_LeftHanded ? !vr->m_UseWithLeftHand : vr->m_UseWithLeftHand);

			s_SrvGrab.lastGrabCtrl = grabCtrl;
			s_SrvGrab.lastEHandle = eh;
			s_SrvGrab.hasLocalCenter = false;
			s_SrvGrab.hasServerOBB = false;
			s_SrvGrab.hasCalibratedSeating = false;
			s_SrvGrab.isLargeObject = false;
			s_SrvGrab.seatDurationMs = 0ULL;
			s_SrvGrab.yawInitialized = false;
			s_SrvGrab.hasSettledInHand = false;
			s_SrvGrab.attachTickMs = GetTickCount64();
			s_SrvGrab.wasWallBlocked = false;
			s_SrvGrab.lastHapticTickMs = 0;
			s_SrvGrab.hasLastTarget = false;
			s_SrvGrab.ClearPropVel();
			s_LastServerImpulseUse = LastServerImpulseUse{};
#if VR_GRAB_HAS_VR_HANDS
			VRHands::ClearHeldHandPhysicsFeedback();
#endif
		}
		else if (eh != 0xFFFFFFFFu)
		{
			s_SrvGrab.lastEHandle = eh;
		}

		LocalGripPose gp{};
		if (!EvaluateHandGripPose(vr, vr->m_HoldingWithLeftHand, gp))
			return false;

		if (isInitialAttach)
		{
			BuildRotFromAngles(engineTargetAng, s_SrvGrab.initialObjRot);
			MulTransposeA3x3(gp.wristR, s_SrvGrab.initialObjRot, s_SrvGrab.R_rel);
			s_SrvGrab.initialEngineTargetPos = engineTargetPos;
			s_SrvGrab.initialGripPose = gp;
		}

		void* srvEnt = (eh != 0xFFFFFFFFu) ? ResolveServerEntityByEHandle(game, eh) : nullptr;
		if (srvEnt)
		{
			const uint32_t eFlags = *reinterpret_cast<const uint32_t*>(reinterpret_cast<uintptr_t>(srvEnt) + kSrvEnt_EFlags);
			if ((eFlags & 0x1u) != 0u) // EFL_KILLME
				srvEnt = nullptr;
		}
		int fallbackIdx = -1;
		if (!srvEnt && eh == 0xFFFFFFFFu && IsLiveServerEntityInList(game, fallbackServerEnt, &fallbackIdx))
			srvEnt = fallbackServerEnt;
		int entIdx = ResolveServerEntityIndex(game, srvEnt, eh);
		if (entIdx <= 0 && fallbackIdx > 0)
			entIdx = fallbackIdx;
		if (entIdx > 0)
			s_SrvGrab.heldEntIndex = entIdx;

		// Resolve object-local OBB bounds strictly from server-side ICollideable (or grabCtrl +0x2078 fallback)
		// without ever accessing client entities or client.dll CollisionToWorldTransform on the server thread.
		if (srvEnt && !s_SrvGrab.hasServerOBB)
		{
			const float rawRad = *reinterpret_cast<const float*>(reinterpret_cast<uintptr_t>(srvEnt) + kSrvEnt_CollisionRad);
			s_SrvGrab.objRadius = std::isfinite(rawRad) ? ClampF(rawRad, 0.0f, 120.0f) : 0.0f;

			Vector mins(0.0f, 0.0f, 0.0f), maxs(0.0f, 0.0f, 0.0f);
			if (QueryServerCollideableBounds(srvEnt, mins, maxs))
			{
				s_SrvGrab.localOBBCenter = (mins + maxs) * 0.5f;
				s_SrvGrab.obbHalfExtents = Vector(
					std::max(0.25f, (maxs.x - mins.x) * 0.5f),
					std::max(0.25f, (maxs.y - mins.y) * 0.5f),
					std::max(0.25f, (maxs.z - mins.z) * 0.5f));
				if (s_SrvGrab.objRadius <= 0.01f)
					s_SrvGrab.objRadius = Len3(s_SrvGrab.obbHalfExtents);
				s_SrvGrab.hasServerOBB = true;
				s_SrvGrab.hasLocalCenter = true;
				s_SrvGrab.hasCalibratedSeating = false;
			}
		}

		if (!s_SrvGrab.hasLocalCenter)
		{
			if (eh != 0xFFFFFFFFu)
			{
				s_SrvGrab.localOBBCenter = *reinterpret_cast<const Vector*>(
					reinterpret_cast<uintptr_t>(grabCtrl) + kGrabOff_ObjSpacePos);
				float he = std::max(0.5f, s_SrvGrab.objRadius * 0.577f);
				s_SrvGrab.obbHalfExtents = Vector(he, he, he);
				s_SrvGrab.hasLocalCenter = true;
			}
			else if (srvEnt)
			{
				// Fallback on initial AttachEntity tick via CBaseEntity::WorldSpaceCenter() (vtable + 0x254)
				uintptr_t srvVtbl = *reinterpret_cast<const uintptr_t*>(srvEnt);
				if (srvVtbl)
				{
					typedef const Vector& (__thiscall* tWorldSpaceCenter)(void*);
					auto fnCenter = *reinterpret_cast<tWorldSpaceCenter const*>(srvVtbl + 0x254u);
					if (fnCenter)
					{
						const Vector& wCenter = fnCenter(srvEnt);
						const matrix3x4_t& coordFrame = *reinterpret_cast<const matrix3x4_t*>(
							reinterpret_cast<uintptr_t>(srvEnt) + kSrvEnt_CoordFrame);
						s_SrvGrab.localOBBCenter = InvTransformPointOrtho3x4(coordFrame, wCenter);
						float he = std::max(0.5f, s_SrvGrab.objRadius * 0.577f);
						s_SrvGrab.obbHalfExtents = Vector(he, he, he);
						s_SrvGrab.hasLocalCenter = true;
					}
				}
			}
		}

		// Calibrate natural 6-DOF VVD-flush palm seating as soon as local bounds resolve
		if (!s_SrvGrab.hasCalibratedSeating && s_SrvGrab.hasLocalCenter)
		{
			const LocalGripPose& calGp = s_SrvGrab.initialGripPose;
			s_SrvGrab.isLargeObject = (s_SrvGrab.objRadius > 16.5f);

			const Vector objCenterInit = s_SrvGrab.initialEngineTargetPos +
				RotateVec3x3(s_SrvGrab.initialObjRot, s_SrvGrab.localOBBCenter);
			const Vector initWristLocal = InvTransformPointOrtho3x4(calGp.wristToWorld, objCenterInit);
			s_SrvGrab.initialWristLocalCenter = initWristLocal;

			const Vector palmSkinLocal = GetPalmSkinLocalCoords(calGp.isLeftHandMesh, calGp.usePhoneOpenTable);
			const Vector skinInObj = InvRotateVec3x3(s_SrvGrab.initialObjRot, calGp.palmSkinWorldPos - objCenterInit);
			const Vector& he = s_SrvGrab.obbHalfExtents;
			const float sdx = std::max(0.0f, std::fabs(skinInObj.x) - he.x);
			const float sdy = std::max(0.0f, std::fabs(skinInObj.y) - he.y);
			const float sdz = std::max(0.0f, std::fabs(skinInObj.z) - he.z);
			const float skinSurfaceDist = std::sqrt(sdx * sdx + sdy * sdy + sdz * sdz);

			if (s_SrvGrab.isLargeObject)
			{
				// Large two-handed/heavy props (planks, crates, barrels, chairs): preserve the grabbed point
				// relative to the wrist, clamped within a comfortable carry envelope.
				Vector deltaWorld = objCenterInit - calGp.gripWorldPos;
				float dist = Len3(deltaWorld);
				Vector dir = (dist > 0.001f) ? (deltaWorld * (1.0f / dist)) : calGp.palmOutwardNormal;
				float minKeep = ClampF(s_SrvGrab.objRadius * 0.45f, 3.0f, 22.0f);
				float maxKeep = std::max(minKeep, s_SrvGrab.objRadius + 6.0f);
				Vector clampedCenterWorld = calGp.gripWorldPos + dir * ClampF(dist, minKeep, maxKeep);
				Vector clampedLocal = InvTransformPointOrtho3x4(calGp.wristToWorld, clampedCenterWorld);
				s_SrvGrab.initialWristLocalCenter = clampedLocal;
				s_SrvGrab.targetWristLocalCenter = clampedLocal;
				s_SrvGrab.seatDurationMs = 0ULL;
			}
			else if (s_SrvGrab.hasServerOBB && skinSurfaceDist <= 2.0f)
			{
				// Close / direct-hand contact grab (hand placed right onto a bottle/can/tool):
				// preserve natural 6-DOF rotation R_rel and tangential (x, z) grip position along the prop,
				// while seating the prop cross-section flush (0.0 mm gap) onto the VVD inner palm skin over 90 ms.
				const float effHalfThick = ComputeEffectivePalmHalfThickness(
					s_SrvGrab.initialObjRot, s_SrvGrab.obbHalfExtents, calGp.wristToWorld);
				const float tx = ClampF(initWristLocal.x, 2.45f, 5.05f);
				const float ty = palmSkinLocal.y + effHalfThick;
				const float tz = ClampF(initWristLocal.z, palmSkinLocal.z - 1.25f, palmSkinLocal.z + 1.25f);
				s_SrvGrab.targetWristLocalCenter = Vector(tx, ty, tz);
				const float seatDelta = Len3(s_SrvGrab.targetWristLocalCenter - initWristLocal);
				s_SrvGrab.seatDurationMs = (bIsTeleport || seatDelta <= 0.12f) ? 0ULL : 90ULL;
			}
			else
			{
				// Reach / pointing grab (>2.0u from palm skin): glide smoothly over 115 ms into the
				// anatomical thumb-web palmar cradle flush against VVD inner palm skin (0.0 mm hover gap).
				const float effHalfThick = ComputeEffectivePalmHalfThickness(
					s_SrvGrab.initialObjRot, s_SrvGrab.obbHalfExtents, calGp.wristToWorld);
				s_SrvGrab.targetWristLocalCenter = Vector(
					palmSkinLocal.x,
					palmSkinLocal.y + effHalfThick,
					palmSkinLocal.z);
				s_SrvGrab.seatDurationMs = bIsTeleport ? 0ULL : 115ULL;
			}
			s_SrvGrab.hasCalibratedSeating = true;
		}

		// Rigid 6-DOF wrist-locked orientation: R_obj = R_wrist * R_rel
		float R_obj[3][3]{};
		Mul3x3(gp.wristR, s_SrvGrab.R_rel, R_obj);
		outTargetAng = AnglesFromRot3x3(R_obj);

		// Pure rigid wrist-locked placement (zero world-space / player-XY projection -> invariant at every tilt angle)
		const uint64_t seatNowMs = GetTickCount64();
		float seatBlend = 1.0f;
		if (!bIsTeleport && s_SrvGrab.seatDurationMs > 0ULL &&
			seatNowMs >= s_SrvGrab.attachTickMs &&
			(seatNowMs - s_SrvGrab.attachTickMs) < s_SrvGrab.seatDurationMs)
		{
			seatBlend = SmoothStep01(
				static_cast<float>(seatNowMs - s_SrvGrab.attachTickMs) /
				static_cast<float>(s_SrvGrab.seatDurationMs));
		}

		Vector activeLocalCenter = s_SrvGrab.initialWristLocalCenter +
			(s_SrvGrab.targetWristLocalCenter - s_SrvGrab.initialWristLocalCenter) * seatBlend;

		if (!s_SrvGrab.isLargeObject && seatBlend >= 1.0f)
		{
			const Vector livePalmSkinLocal = GetPalmSkinLocalCoords(gp.isLeftHandMesh, gp.usePhoneOpenTable);
			const float push = ComputeMinimumPalmClearancePush(
				R_obj, s_SrvGrab.obbHalfExtents, gp.wristToWorld, activeLocalCenter, livePalmSkinLocal.y);
			activeLocalCenter.y += ClampF(push, 0.0f, 6.0f);
		}

		const Vector desiredCenterWorld = TransformPoint3x4(gp.wristToWorld, activeLocalCenter);
		outTargetPos = desiredCenterWorld - RotateVec3x3(R_obj, s_SrvGrab.localOBBCenter);

		// Sync shadow controller fields, neutralize any VM-grab/EF_NODRAW hijack, and lock m_errorTime < 0
		*reinterpret_cast<Vector*>(reinterpret_cast<uintptr_t>(grabCtrl) + kGrabOff_TargetPos) = outTargetPos;
		*reinterpret_cast<QAngle*>(reinterpret_cast<uintptr_t>(grabCtrl) + kGrabOff_TargetAng) = outTargetAng;
		NeutralizeVMGrabAndClientClone(game, vr, grabCtrl, nullptr, srvEnt);
		MaintainServerGrabState(game, vr, grabCtrl, nullptr);
		if (srvEnt)
		{
			WakeServerEntityPhysics(srvEnt);
			Vector propLin(0.0f, 0.0f, 0.0f), propAng(0.0f, 0.0f, 0.0f);
			if (QueryServerEntityPhysicsVelocity(srvEnt, propLin, propAng))
				s_SrvGrab.PushPropVel(seatNowMs, propLin, propAng);
		}

		// Publish wrist-local object origin for zero-lag client finger collision & hand-to-hand transfer check
		if (s_SrvGrab.heldEntIndex > 0)
		{
			Vector wristLocalOrig = InvTransformPointOrtho3x4(gp.wristToWorld, outTargetPos);
			s_HeldSnapshot.Publish(
				s_SrvGrab.heldEntIndex,
				vr->m_HoldingWithLeftHand,
				s_SrvGrab.R_rel,
				wristLocalOrig,
				s_SrvGrab.localOBBCenter,
				s_SrvGrab.obbHalfExtents,
				seatNowMs);
		}

#if VR_GRAB_HAS_VR_HANDS
		// Server-physics feedback for the rendered hand (and haptics): compare where the prop actually is
		// (server coordinate frame) with where the raw, uncoupled controller wants it.
		if (srvEnt && s_SrvGrab.heldEntIndex > 0)
		{
			const uint64_t nowMs = seatNowMs;
			const matrix3x4_t& coord = *reinterpret_cast<const matrix3x4_t*>(
				reinterpret_cast<uintptr_t>(srvEnt) + kSrvEnt_CoordFrame);
			float R_act[3][3]{};
			for (int r = 0; r < 3; ++r)
			{
				for (int c = 0; c < 3; ++c)
					R_act[r][c] = coord[r][c];
			}
			const Vector actualPos(coord[0][3], coord[1][3], coord[2][3]);
			const QAngle actualAng = AnglesFromRot3x3(R_act);
			const float posErr = Len3(actualPos - outTargetPos);

			// Far-grabs take several ticks to fly into the hand; do not yank the rendered hand toward the prop
			// (or buzz haptics) until seating has finished and the prop has arrived (<= 4 units) or 350 ms elapsed.
			if (!s_SrvGrab.hasSettledInHand && seatBlend >= 1.0f &&
				(posErr <= 4.0f || nowMs < s_SrvGrab.attachTickMs || (nowMs - s_SrvGrab.attachTickMs) >= 350ULL))
			{
				s_SrvGrab.hasSettledInHand = true;
			}

			float curTargetSpeed = s_SrvGrab.lastTargetSpeed;
			if (s_SrvGrab.hasLastTarget && nowMs >= s_SrvGrab.lastTargetTickMs + 5ULL)
			{
				const float dtSec = static_cast<float>(nowMs - s_SrvGrab.lastTargetTickMs) * 0.001f;
				curTargetSpeed = (dtSec <= 0.25f)
					? Len3(outTargetPos - s_SrvGrab.lastTargetPos) / dtSec
					: 0.0f;
				s_SrvGrab.lastTargetSpeed = curTargetSpeed;
				s_SrvGrab.lastTargetPos = outTargetPos;
				s_SrvGrab.lastTargetTickMs = nowMs;
			}
			else if (!s_SrvGrab.hasLastTarget)
			{
				s_SrvGrab.lastTargetPos = outTargetPos;
				s_SrvGrab.lastTargetTickMs = nowMs;
				s_SrvGrab.lastTargetSpeed = 0.0f;
				s_SrvGrab.hasLastTarget = true;
				curTargetSpeed = 0.0f;
			}

			bool blocked = false;
			if (s_SrvGrab.hasSettledInHand)
			{
				// The shadow controller always trails a moving hand by a few ticks. Only treat the prop as blocked
				// (wall / table / body) when its deficit exceeds that natural lag (~60 ms of target motion).
				const float allowedLag = 3.0f + 0.06f * curTargetSpeed;
				// Hysteresis so the visual hand tether does not chatter at the threshold.
				blocked = s_SrvGrab.wasWallBlocked ? (posErr > allowedLag * 0.5f) : (posErr > allowedLag);
				if (blocked)
				{
					const float impactSpeedMps = std::max(
						UnitsPerSecToMps(vr, curTargetSpeed),
						GetControllerSpeedMps(vr, vr->m_HoldingWithLeftHand));
					const float impactAmp = VR::ComputeImpactHapticAmplitude(impactSpeedMps, 0.10f, 2.3f);
					if (!s_SrvGrab.wasWallBlocked)
						TriggerControllerHaptic(vr, vr->m_HoldingWithLeftHand, 900, impactAmp, 60ULL, s_SrvGrab.lastHapticTickMs);
					else if (posErr > allowedLag + 3.0f)
						TriggerControllerHaptic(vr, vr->m_HoldingWithLeftHand, 750, impactAmp, 120ULL, s_SrvGrab.lastHapticTickMs);
				}
			}
			s_SrvGrab.wasWallBlocked = blocked;

			VRHands::HeldHandPhysicsFeedback fb{};
			fb.heldEntIndex = s_SrvGrab.heldEntIndex;
			fb.holdingLeftHand = vr->m_HoldingWithLeftHand;
			fb.targetEntPos = outTargetPos;
			fb.targetEntAng = outTargetAng;
			fb.actualEntPos = s_SrvGrab.hasSettledInHand ? actualPos : outTargetPos;
			fb.actualEntAng = s_SrvGrab.hasSettledInHand ? actualAng : outTargetAng;
			fb.targetWristToWorld = gp.wristToWorld;
			fb.objRadius = s_SrvGrab.objRadius;
			fb.isWallBlocked = blocked;
			VRHands::PublishHeldHandPhysicsFeedback(fb);
		}
#endif

		return true;
	}

	bool UpdateHeldObjectFingerCollision(Game* game, VR* vr, float outMaxFingerCurl[5])
	{
		if (outMaxFingerCurl)
		{
			for (int i = 0; i < 5; ++i)
				outMaxFingerCurl[i] = 1.0f;
		}

		if (!game || !vr || !vr->m_IsVREnabled || !vr->m_ShowHands || !game->m_EngineClient || !game->m_EngineClient->IsInGame())
		{
			ResetClientCurlSlot(0, false);
			ResetClientCurlSlot(1, true);
			return false;
		}

		// Keep client-side C_Portal_Player VM-grab / attached-clone handles cleared on the client main thread
		const int localIdx = game->m_EngineClient->GetLocalPlayer();
		void* localClientPlayer = (localIdx > 0) ? game->GetClientEntity(localIdx) : nullptr;
		if (localClientPlayer)
		{
			const uintptr_t cpAddr = reinterpret_cast<uintptr_t>(localClientPlayer);
			*reinterpret_cast<uint8_t*>(cpAddr + kCliPlayer_UsingVMGrab) = 0u;
			*reinterpret_cast<uint8_t*>(cpAddr + kCliPlayer_CarriedByLocal) = 0u;
			*reinterpret_cast<uint32_t*>(cpAddr + kCliPlayer_AttachedObj) = 0xFFFFFFFFu;
		}

		// Resolve held client entity index and which physical hand is holding it
		int heldIdx = -1;
		bool holdLeft = vr->m_HoldingWithLeftHand;
		float R_rel[3][3]{
			{ 1.0f, 0.0f, 0.0f },
			{ 0.0f, 1.0f, 0.0f },
			{ 0.0f, 0.0f, 1.0f }
		};
		Vector wristLocalObjOrigin(0.0f, 0.0f, 0.0f);
		unsigned long long snapTickMs = 0;
		bool hasSnap = vr->m_IsHoldingObject &&
			s_HeldSnapshot.Read(heldIdx, holdLeft, R_rel, wristLocalObjOrigin, snapTickMs);

		if (heldIdx <= 0 && localClientPlayer)
		{
			uint32_t ehInfra = *reinterpret_cast<const uint32_t*>(
				reinterpret_cast<uintptr_t>(localClientPlayer) + kCliPlayer_HeldObj);
			if (ehInfra != 0xFFFFFFFFu)
				heldIdx = static_cast<int>(ehInfra & 0x0FFFu);
		}

		if (!vr->m_IsHoldingObject || heldIdx <= 0)
		{
			ResetClientCurlSlot(0, false);
			ResetClientCurlSlot(1, true);
			return false;
		}

		// Ensure the non-holding hand is never clamped
		const int activeSlot = holdLeft ? 1 : 0;
		const int otherSlot = holdLeft ? 0 : 1;
		ResetClientCurlSlot(otherSlot, !holdLeft);

		ICollideable* collideable = nullptr;
		Vector obbMins(0.0f, 0.0f, 0.0f), obbMaxs(0.0f, 0.0f, 0.0f);
		matrix3x4_t collToWorld{};
		int solidType = 0;
		if (!QueryClientCollideableBounds(game, heldIdx, collideable, obbMins, obbMaxs, collToWorld, solidType))
		{
			ResetClientCurlSlot(activeSlot, holdLeft);
			return false;
		}

#if VR_GRAB_HAS_VR_HANDS
		VRHandGripPose handPose{};
		if (!VRHands::ComputeHandGripPose(vr, holdLeft, vr->m_SetupOrigin, handPose, true) || !handPose.valid)
		{
			ResetClientCurlSlot(activeSlot, holdLeft);
			return false;
		}

		// Compensate 1-tick client entity interpolation lag while preserving true wall-push deflection:
		// T_live is the wrist-locked object transform at the live 90-144 Hz rendered hand pose.
		// When the prop is unhindered (||origin_client - origin_live|| <= 4.0 units), mapping finger joints
		// through T_client * T_live^-1 evaluates ClipRayToCollideable in the exact VPhysics shape frame with
		// zero motion jitter when waving the hand rapidly.
		bool useLiveRigidMap = false;
		matrix3x4_t liveObjToWorld{};
		if (hasSnap && (GetTickCount64() - snapTickMs) <= 250ULL)
		{
			float wristR[3][3]{};
			for (int r = 0; r < 3; ++r)
			{
				for (int c = 0; c < 3; ++c)
					wristR[r][c] = handPose.wristToWorld[r][c];
			}
			float R_obj_live[3][3]{};
			Mul3x3(wristR, R_rel, R_obj_live);
			Vector origLive = TransformPoint3x4(handPose.wristToWorld, wristLocalObjOrigin);
			Vector origClient(collToWorld[0][3], collToWorld[1][3], collToWorld[2][3]);
			if (LenSq3(origClient - origLive) <= 16.0f) // <= 4.0 units (~9.3 cm): pure interpolation delta
			{
				for (int r = 0; r < 3; ++r)
				{
					liveObjToWorld[r][0] = R_obj_live[r][0];
					liveObjToWorld[r][1] = R_obj_live[r][1];
					liveObjToWorld[r][2] = R_obj_live[r][2];
				}
				liveObjToWorld[0][3] = origLive.x;
				liveObjToWorld[1][3] = origLive.y;
				liveObjToWorld[2][3] = origLive.z;
				useLiveRigidMap = true;
			}
		}

		auto mapHandPointToCollideableWorld = [&](const Vector& pWorld) -> Vector
		{
			if (!useLiveRigidMap)
				return pWorld;
			Vector pObjLocal = InvTransformPointOrtho3x4(liveObjToWorld, pWorld);
			return TransformPoint3x4(collToWorld, pObjLocal);
		};

		const auto& liveSplays = holdLeft ? vr->m_LeftFingerSplay : vr->m_RightFingerSplay;
		constexpr int kCurlSteps = 12;
		const bool canClipTrace = (game->m_EngineTrace != nullptr) && (solidType != 0);
		const Vector objLocalCenter = (obbMins + obbMaxs) * 0.5f;
		const Vector objWorldCenter = TransformPoint3x4(collToWorld, objLocalCenter);

		auto sweepPadSphere = [&](const Vector& p0, const Vector& p1, float rawRadius, bool allowInitialStartSolid) -> float
		{
			const float padRad = ClampF(rawRadius * 0.50f, 0.14f, 0.24f);
			const Vector segDelta = p1 - p0;
			const Vector toCenter = objWorldCenter - p0;
			const bool movingInward = (Dot3(segDelta, toCenter) > 0.0f);

			if (canClipTrace)
			{
				float trFrac = 1.0f;
				bool startSolid = false;
				if (ClipSweptSphereToCollideable(game->m_EngineTrace, collideable, p0, p1, padRad, trFrac, startSolid))
				{
					if (startSolid)
						return (allowInitialStartSolid && movingInward) ? 0.0f : 2.0f;
					return trFrac;
				}
				return 2.0f;
			}

			Vector loc0 = InvTransformPointOrtho3x4(collToWorld, p0);
			Vector loc1 = InvTransformPointOrtho3x4(collToWorld, p1);
			float hitFrac = SweepSphereAgainstLocalOBB(loc0, loc1, obbMins, obbMaxs, padRad);
			if (hitFrac <= 0.0f)
				return (allowInitialStartSolid && movingInward) ? 0.0f : 2.0f;
			return hitFrac;
		};

		float rawMaxCurl[5]{ 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };

		for (int f = 0; f < 5; ++f)
		{
			const float splay = (f == 0) ? 0.0f : liveSplays[f - 1];
			Vector prevJoints[5]{};
			float segRadii[4]{};
			VRHands::EvaluateFingerWorldChain(handPose, f, 0.0f, splay, prevJoints, segRadii);
			for (int j = 0; j < 5; ++j)
				prevJoints[j] = mapHandPointToCollideableWorld(prevJoints[j]);

			float stopCurl = 1.0f;
			for (int step = 1; step <= kCurlSteps; ++step)
			{
				const float cPrev = static_cast<float>(step - 1) / static_cast<float>(kCurlSteps);
				const float cNext = static_cast<float>(step) / static_cast<float>(kCurlSteps);

				Vector nextJoints[5]{};
				VRHands::EvaluateFingerWorldChain(handPose, f, cNext, splay, nextJoints, segRadii);
				for (int j = 0; j < 5; ++j)
					nextJoints[j] = mapHandPointToCollideableWorld(nextJoints[j]);

				// Sweep 3 overlapping distal gripping pad spheres:
				// distal fingertip (4), distal phalanx midpoint (3..4), and DIP pad (3).
				// Proximal/palm joints naturally cradle the back of held props at curl=0, so startSolid
				// at curl=0 only halts immediately if the distal fingertip itself is already embedded.
				float earliestFrac = 2.0f;

				const Vector prevSamples[3] = {
					prevJoints[4],
					(prevJoints[3] + prevJoints[4]) * 0.5f,
					prevJoints[3]
				};
				const Vector nextSamples[3] = {
					nextJoints[4],
					(nextJoints[3] + nextJoints[4]) * 0.5f,
					nextJoints[3]
				};
				const float sampleRadii[3] = {
					segRadii[3],
					segRadii[3],
					segRadii[2]
				};

				for (int sIdx = 0; sIdx < 3; ++sIdx)
				{
					const bool allowStartSolid = (step > 1) || (sIdx == 0);
					float hitFrac = sweepPadSphere(prevSamples[sIdx], nextSamples[sIdx], sampleRadii[sIdx], allowStartSolid);
					if (hitFrac < earliestFrac)
						earliestFrac = hitFrac;
				}

				if (earliestFrac <= 1.0f)
				{
					// Leave a tiny natural open-hand floor (0.04) if already touching at curl=0 so fingers never hyperextend
					stopCurl = ClampF(cPrev + (cNext - cPrev) * ClampF(earliestFrac, 0.0f, 1.0f), 0.04f, 1.0f);
					break;
				}

				for (int j = 0; j < 5; ++j)
					prevJoints[j] = nextJoints[j];
			}

			rawMaxCurl[f] = stopCurl;
		}

		ClientCurlState& st = s_ClientCurl[activeSlot];
		const bool freshObject = (st.lastEntIndex.load(std::memory_order_relaxed) != heldIdx) ||
		                         !st.active.load(std::memory_order_relaxed);
		st.lastEntIndex.store(heldIdx, std::memory_order_relaxed);

		float finalLimits[5]{};
		for (int i = 0; i < 5; ++i)
		{
			float sm = st.smoothed[i].load(std::memory_order_relaxed);
			if (freshObject)
			{
				sm = rawMaxCurl[i];
			}
			else
			{
				// Fast tighten on surface contact, smooth release so fingers rest rock-steady on curved props
				float alpha = (rawMaxCurl[i] < sm) ? 0.65f : 0.30f;
				sm += (rawMaxCurl[i] - sm) * alpha;
			}
			st.smoothed[i].store(sm, std::memory_order_relaxed);
			finalLimits[i] = ClampF(sm, 0.04f, 1.0f);
			st.maxCurl[i].store(finalLimits[i], std::memory_order_relaxed);
			if (outMaxFingerCurl)
				outMaxFingerCurl[i] = finalLimits[i];
		}
		st.active.store(true, std::memory_order_release);

		VRHands::SetFingerCurlLimits(holdLeft, finalLimits);
		return true;
#else
		(void)solidType;
		return false;
#endif
	}

	bool GetFingerCurlLimits(bool physicalLeftHand, float outMaxFingerCurl[5])
	{
		const ClientCurlState& st = s_ClientCurl[physicalLeftHand ? 1 : 0];
		if (!st.active.load(std::memory_order_acquire))
		{
			if (outMaxFingerCurl)
			{
				for (int i = 0; i < 5; ++i)
					outMaxFingerCurl[i] = 1.0f;
			}
			return false;
		}
		if (outMaxFingerCurl)
		{
			for (int i = 0; i < 5; ++i)
				outMaxFingerCurl[i] = st.maxCurl[i].load(std::memory_order_relaxed);
		}
		return true;
	}
}


// -------------------------------------------------------------------------------------------------
// Open-hand server-side prop / door interaction
//
// Free (non-gripping, non-curling) VR hands push, knock and wake movable server props and nudge doors
// only when the calibrated VVD palm skin slab actually contacts the object during a deliberate swat.
// Uses an exact 15-axis oriented-box test in wrist space + IEngineTrace (EngineTraceServer003)
// ClipRayToCollideable against the prop's VPhysics convex shape so cylindrical OBB air corners and
// gentle grab approaches never prematurely knock props away before gripping.
// -------------------------------------------------------------------------------------------------
namespace
{
	// Verified server.dll / vphysics.dll layout
	constexpr uintptr_t kSrvPhysEnvRva     = 0x793C1C; // IPhysicsEnvironment* physenv (zeroed in PhysShutdown)
	constexpr uintptr_t kSrvEnt_Classname  = 0x0060;   // string_t m_iClassname (const char*)
	constexpr uintptr_t kSrvEnt_MoveType   = 0x00DA;   // uint8 m_MoveType (6 == MOVETYPE_VPHYSICS)
	constexpr uint8_t   kMoveTypeVPhysics  = 6;
	constexpr uintptr_t kEntObjectCapsVtblOffset = 0x9C; // int CBaseEntity::ObjectCaps() const
	constexpr uintptr_t kEntUseVtblOffset  = 0x194;    // CBaseEntity::Use(pActivator, pCaller, USE_TYPE, float) ret 0x10
	constexpr int       kUseOff            = 0;        // USE_OFF
	constexpr int       kUseOn             = 1;        // USE_ON
	constexpr int       kUseToggle         = 3;        // USE_TOGGLE
	constexpr int       kFcapImpulseUse    = 0x01;
	constexpr int       kFcapContinuousUse = 0x02;
	constexpr int       kFcapOnOffUse      = 0x04;
	constexpr uintptr_t kEnvIsInSimulation = 0x13C;    // bool IPhysicsEnvironment::IsInSimulation() const
	constexpr uintptr_t kSrvEnt_Effects    = 0x00A8;   // uint32 m_fEffects (EF_NODRAW = 0x20)
	constexpr uintptr_t kSrvEnt_SolidFlags = 0x0108;   // uint16 m_Collision.m_usSolidFlags (NOT_SOLID | TRIGGER = 0x0C)
	constexpr uintptr_t kSrvEnt_SolidType  = 0x010A;   // uint8  m_Collision.m_nSolidType (SOLID_NONE = 0)
	constexpr uintptr_t kSrvPlayer_GroundEntity = 0x0158; // uint32 CBasePlayer::m_hGroundEntity
	constexpr uintptr_t kSrvFn_CalcAbsolutePosition = 0xB99E0; // CBaseEntity::CalcAbsolutePosition (thiscall)
	typedef void (__thiscall* tOH_Ent)(void*);

	constexpr float kHandEffectiveMassKg       = 2.0f;
	constexpr float kHandRestitution           = 0.2f;
	constexpr float kHeavyPropMinSwatSpeed     = 14.0f;   // units/s (~32 cm/s) for heavy props (> 15 kg)
	constexpr float kLightPropMinSwatSpeed     = 28.0f;   // units/s (~65 cm/s) deliberate swat for light pickupable props
	constexpr float kMaxClosingSpeed           = 450.0f;
	constexpr float kDoorTouchEngageRadius     = 0.65f;   // ~1.5 cm from door surface for gentle palm/finger touch
	constexpr float kDoorTouchReleaseRadius    = 2.50f;   // ~6 cm re-arm hysteresis so hand must pull back before retoggling
	constexpr uint64_t kDoorTouchClearMs       = 120ULL;
	constexpr uint64_t kDoorCooldownMs         = 1100ULL;
	constexpr int   kMaxImpulsesPerHand        = 8;

	constexpr float kButtonTouchEngageRadius    = 0.28f; // ~6.5 mm tight fingertip-pad radius (avoids hitting adjacent 2u switches)
	constexpr float kButtonTouchReleaseRadius   = 0.85f; // ~20 mm re-arm hysteresis
	constexpr uint64_t kButtonTouchClearMs      = 70ULL;
	constexpr uint64_t kButtonTouchCooldownMs   = 350ULL;
	constexpr uint64_t kButtonHandGlobalCooldownMs = 250ULL;
	constexpr float kButtonMinApproachSpeed     = 0.45f; // ~1 cm/s gentle intentional press

	struct OpenHandSide
	{
		bool valid = false;
		Vector prevPalm{ 0.0f, 0.0f, 0.0f };
		matrix3x4_t prevWrist{};
		bool tipValid = false;
		Vector prevIndexTip{ 0.0f, 0.0f, 0.0f };
		Vector prevRawIndexTip{ 0.0f, 0.0f, 0.0f };
		uint64_t prevTickMs = 0;
		uint64_t lastHapticMs = 0;
		uint64_t lastButtonClickMs = 0ULL;
	};

	struct OpenHandState
	{
		bool keyValid = false;
		bool traceQueried = false;
		IEngineTrace* srvTrace = nullptr;
		void* physEnv = nullptr;
		void* worldEnt = nullptr;
		uint32_t worldSerial = 0;
		void* player = nullptr;
		OpenHandSide sides[2]; // [0] physical right, [1] physical left
	};

	OpenHandState s_OpenHand;

	typedef bool  (__thiscall* tOH_Bool)(void*);
	typedef int   (__thiscall* tOH_Int)(void*);
	typedef void  (__thiscall* tOH_Void)(void*);
	typedef void* (__thiscall* tOH_Ptr)(void*);
	typedef float (__thiscall* tOH_Float)(void*);
	typedef void  (__thiscall* tOH_GetVelocity)(void*, Vector*, Vector*);
	typedef void  (__thiscall* tOH_ForceOffset)(void*, const Vector&, const Vector&);
	typedef void  (__thiscall* tOH_Use)(void*, void*, void*, int, float);

	template <typename T>
	T OH_Fn(const void* obj, uintptr_t offset)
	{
		const uintptr_t vtbl = *reinterpret_cast<const uintptr_t*>(obj);
		if (!vtbl)
			return nullptr;
		return *reinterpret_cast<T const*>(vtbl + offset);
	}

	inline Vector OH_ClampToBox(const Vector& p, const Vector& mins, const Vector& maxs)
	{
		return Vector(
			ClampF(p.x, mins.x, maxs.x),
			ClampF(p.y, mins.y, maxs.y),
			ClampF(p.z, mins.z, maxs.z));
	}

	// Exact 15-axis Separating Axis Theorem (SAT) overlap test between two oriented bounding boxes.
	bool OH_IntersectOrientedBoxes(
		const matrix3x4_t& xformA,
		const Vector& minsA,
		const Vector& maxsA,
		const matrix3x4_t& xformB,
		const Vector& minsB,
		const Vector& maxsB)
	{
		const Vector cALocal = (minsA + maxsA) * 0.5f;
		const Vector cBLocal = (minsB + maxsB) * 0.5f;
		const float eA[3] = {
			std::max(0.01f, (maxsA.x - minsA.x) * 0.5f),
			std::max(0.01f, (maxsA.y - minsA.y) * 0.5f),
			std::max(0.01f, (maxsA.z - minsA.z) * 0.5f)
		};
		const float eB[3] = {
			std::max(0.01f, (maxsB.x - minsB.x) * 0.5f),
			std::max(0.01f, (maxsB.y - minsB.y) * 0.5f),
			std::max(0.01f, (maxsB.z - minsB.z) * 0.5f)
		};

		const Vector uA[3] = {
			Vector(xformA[0][0], xformA[1][0], xformA[2][0]),
			Vector(xformA[0][1], xformA[1][1], xformA[2][1]),
			Vector(xformA[0][2], xformA[1][2], xformA[2][2])
		};
		const Vector uB[3] = {
			Vector(xformB[0][0], xformB[1][0], xformB[2][0]),
			Vector(xformB[0][1], xformB[1][1], xformB[2][1]),
			Vector(xformB[0][2], xformB[1][2], xformB[2][2])
		};

		float R[3][3]{};
		float AbsR[3][3]{};
		for (int i = 0; i < 3; ++i)
		{
			for (int j = 0; j < 3; ++j)
			{
				R[i][j] = Dot3(uA[i], uB[j]);
				AbsR[i][j] = std::fabs(R[i][j]) + 1e-6f;
			}
		}

		const Vector cAWorld = TransformPoint3x4(xformA, cALocal);
		const Vector cBWorld = TransformPoint3x4(xformB, cBLocal);
		const Vector dWorld = cBWorld - cAWorld;
		const float t[3] = { Dot3(dWorld, uA[0]), Dot3(dWorld, uA[1]), Dot3(dWorld, uA[2]) };

		for (int i = 0; i < 3; ++i)
		{
			const float ra = eA[i];
			const float rb = eB[0] * AbsR[i][0] + eB[1] * AbsR[i][1] + eB[2] * AbsR[i][2];
			if (std::fabs(t[i]) > ra + rb)
				return false;
		}

		for (int j = 0; j < 3; ++j)
		{
			const float ra = eA[0] * AbsR[0][j] + eA[1] * AbsR[1][j] + eA[2] * AbsR[2][j];
			const float rb = eB[j];
			const float proj = std::fabs(t[0] * R[0][j] + t[1] * R[1][j] + t[2] * R[2][j]);
			if (proj > ra + rb)
				return false;
		}

		for (int i = 0; i < 3; ++i)
		{
			const int i1 = (i + 1) % 3;
			const int i2 = (i + 2) % 3;
			for (int j = 0; j < 3; ++j)
			{
				const int j1 = (j + 1) % 3;
				const int j2 = (j + 2) % 3;
				const float ra = eA[i1] * AbsR[i2][j] + eA[i2] * AbsR[i1][j];
				const float rb = eB[j1] * AbsR[i][j2] + eB[j2] * AbsR[i][j1];
				const float proj = std::fabs(t[i2] * R[i1][j] - t[i1] * R[i2][j]);
				if (proj > ra + rb)
					return false;
			}
		}
		return true;
	}

	bool OH_IsDoorClass(const void* ent)
	{
		const char* cls = *reinterpret_cast<const char* const*>(reinterpret_cast<uintptr_t>(ent) + kSrvEnt_Classname);
		if (!cls || reinterpret_cast<uintptr_t>(cls) < 0x10000u)
			return false;
		return std::strcmp(cls, "prop_door_rotating") == 0 ||
		       std::strcmp(cls, "func_door_rotating") == 0 ||
		       std::strcmp(cls, "func_door") == 0;
	}

	DoorCooldownSlot* FindDoorCooldownSlot(uint32_t entIdx, uint32_t serial)
	{
		for (DoorCooldownSlot& s : s_DoorCooldowns)
		{
			if (s.entIdx == entIdx && s.serial == serial)
				return &s;
		}
		return nullptr;
	}

	DoorCooldownSlot* FindOrAllocateDoorCooldownSlot(uint32_t entIdx, uint32_t serial)
	{
		DoorCooldownSlot* freeSlot = nullptr;
		DoorCooldownSlot* oldestUnlatched = nullptr;
		DoorCooldownSlot* oldestAny = &s_DoorCooldowns[0];
		for (DoorCooldownSlot& s : s_DoorCooldowns)
		{
			if (s.entIdx == entIdx && s.serial == serial)
				return &s;
			if (s.entIdx == 0 && !freeSlot)
				freeSlot = &s;
			if (!s.latchedByHand[0] && !s.latchedByHand[1] &&
			    (!oldestUnlatched || s.nextMs < oldestUnlatched->nextMs))
			{
				oldestUnlatched = &s;
			}
			if (s.nextMs < oldestAny->nextMs)
				oldestAny = &s;
		}
		DoorCooldownSlot* use = freeSlot ? freeSlot : (oldestUnlatched ? oldestUnlatched : oldestAny);
		*use = DoorCooldownSlot{};
		use->entIdx = entIdx;
		use->serial = serial;
		return use;
	}

	bool OH_IsTouchButtonClass(const void* ent)
	{
		const char* cls = *reinterpret_cast<const char* const*>(reinterpret_cast<uintptr_t>(ent) + kSrvEnt_Classname);
		if (!cls || reinterpret_cast<uintptr_t>(cls) < 0x10000u)
			return false;
		return std::strcmp(cls, "func_button") == 0 ||
		       std::strcmp(cls, "func_rot_button") == 0 ||
		       std::strcmp(cls, "infra_button") == 0 ||
		       std::strcmp(cls, "prop_button") == 0 ||
		       std::strcmp(cls, "prop_under_button") == 0;
	}

	inline float ComputePointToEntityOBBDist(
		const Vector& worldPt,
		const matrix3x4_t& coord,
		const Vector& mins,
		const Vector& maxs)
	{
		const Vector localPt = InvTransformPointOrtho3x4(coord, worldPt);
		const Vector clamped = OH_ClampToBox(localPt, mins, maxs);
		return Len3(localPt - clamped);
	}

	ButtonTouchSlot* FindButtonTouchSlot(uint32_t entIdx, uint32_t serial)
	{
		for (ButtonTouchSlot& s : s_ButtonTouchSlots)
		{
			if (s.entIdx == entIdx && s.serial == serial)
				return &s;
		}
		return nullptr;
	}

	ButtonTouchSlot* FindOrAllocateButtonTouchSlot(uint32_t entIdx, uint32_t serial)
	{
		ButtonTouchSlot* freeSlot = nullptr;
		ButtonTouchSlot* oldestUnlatched = nullptr;
		ButtonTouchSlot* oldestAny = &s_ButtonTouchSlots[0];
		for (ButtonTouchSlot& s : s_ButtonTouchSlots)
		{
			if (s.entIdx == entIdx && s.serial == serial)
				return &s;
			if (s.entIdx == 0 && !freeSlot)
				freeSlot = &s;
			if (!s.latchedByHand[0] && !s.latchedByHand[1] &&
			    (!oldestUnlatched || s.cooldownUntilMs < oldestUnlatched->cooldownUntilMs))
			{
				oldestUnlatched = &s;
			}
			if (s.cooldownUntilMs < oldestAny->cooldownUntilMs)
				oldestAny = &s;
		}
		ButtonTouchSlot* use = freeSlot ? freeSlot : (oldestUnlatched ? oldestUnlatched : oldestAny);
		*use = ButtonTouchSlot{};
		use->entIdx = entIdx;
		use->serial = serial;
		return use;
	}

	void StampDoorGripUseCooldown(uint32_t entIdx, uint32_t serial, uint64_t nowMs, uint64_t cooldownMs)
	{
		DoorCooldownSlot* slot = FindOrAllocateDoorCooldownSlot(entIdx, serial);
		const uint64_t targetMs = nowMs + cooldownMs;
		if (targetMs > slot->nextMs)
			slot->nextMs = targetMs;
	}

	Vector OH_ComputeContactAndInwardNormal(
		const matrix3x4_t& wristToWorld,
		const Vector& boxMins,
		const Vector& boxMaxs,
		const matrix3x4_t& entCoord,
		const Vector& entMins,
		const Vector& entMaxs,
		const Vector& palmVel,
		Vector& outClosestWorld)
	{
		const Vector entCenterWorld = TransformPoint3x4(entCoord, (entMins + entMaxs) * 0.5f);
		const Vector entCenterInWrist = InvTransformPointOrtho3x4(wristToWorld, entCenterWorld);
		const Vector handContactWorld = TransformPoint3x4(
			wristToWorld,
			OH_ClampToBox(entCenterInWrist, boxMins, boxMaxs));
		const Vector handInEnt = InvTransformPointOrtho3x4(entCoord, handContactWorld);
		const Vector closestLocal = OH_ClampToBox(handInEnt, entMins, entMaxs);
		outClosestWorld = TransformPoint3x4(entCoord, closestLocal);

		Vector toObj = outClosestWorld - handContactWorld;
		if (Len3(toObj) < 1e-3f)
		{
			// Contact point is slightly inside [entMins..entMaxs]: pick the nearest penetrated OBB face's
			// inward normal so pushing a wide door near its handle pushes perpendicular into the door.
			const float faceDists[6] = {
				handInEnt.x - entMins.x, entMaxs.x - handInEnt.x,
				handInEnt.y - entMins.y, entMaxs.y - handInEnt.y,
				handInEnt.z - entMins.z, entMaxs.z - handInEnt.z
			};
			const Vector faceNormalsLocal[6] = {
				Vector( 1.0f,  0.0f,  0.0f), Vector(-1.0f,  0.0f,  0.0f),
				Vector( 0.0f,  1.0f,  0.0f), Vector( 0.0f, -1.0f,  0.0f),
				Vector( 0.0f,  0.0f,  1.0f), Vector( 0.0f,  0.0f, -1.0f)
			};
			int bestFace = 0;
			for (int fi = 1; fi < 6; ++fi)
			{
				if (faceDists[fi] < faceDists[bestFace])
					bestFace = fi;
			}
			const Vector& nl = faceNormalsLocal[bestFace];
			toObj = Vector(
				entCoord[0][0] * nl.x + entCoord[0][1] * nl.y + entCoord[0][2] * nl.z,
				entCoord[1][0] * nl.x + entCoord[1][1] * nl.y + entCoord[1][2] * nl.z,
				entCoord[2][0] * nl.x + entCoord[2][1] * nl.y + entCoord[2][2] * nl.z);
		}
		return NormalizeSafe(toObj, NormalizeSafe(palmVel, Vector(1.0f, 0.0f, 0.0f)));
	}

	Vector OH_ComputeDoorInwardNormal(
		const matrix3x4_t& wristToWorld,
		const matrix3x4_t& entCoord,
		const Vector& entMins,
		const Vector& entMaxs,
		const Vector& fallbackDir)
	{
		// Anchor strictly on the player's side of the door so thin (~2u) rotating door slabs never
		// flip the inward normal backwards once the palm or fingertips cross the leaf midplane.
		const Vector forearmWorld = TransformPoint3x4(wristToWorld, Vector(-2.5f, 0.0f, 0.0f));
		const Vector forearmLocal = InvTransformPointOrtho3x4(entCoord, forearmWorld);
		const Vector closestLocal = OH_ClampToBox(forearmLocal, entMins, entMaxs);
		const Vector deltaLocal = closestLocal - forearmLocal;
		const Vector refDir = (Len3(fallbackDir) > 1e-3f)
			? fallbackDir
			: Vector(wristToWorld[0][0], wristToWorld[1][0], wristToWorld[2][0]);
		if (Len3(deltaLocal) > 1e-3f)
		{
			const Vector worldDir(
				entCoord[0][0] * deltaLocal.x + entCoord[0][1] * deltaLocal.y + entCoord[0][2] * deltaLocal.z,
				entCoord[1][0] * deltaLocal.x + entCoord[1][1] * deltaLocal.y + entCoord[1][2] * deltaLocal.z,
				entCoord[2][0] * deltaLocal.x + entCoord[2][1] * deltaLocal.y + entCoord[2][2] * deltaLocal.z);
			return NormalizeSafe(worldDir, NormalizeSafe(refDir, Vector(1.0f, 0.0f, 0.0f)));
		}

		// Fallback if the forearm anchor itself is inside the door OBB: pick its thinnest leaf axis.
		const float ext[3] = {
			entMaxs.x - entMins.x,
			entMaxs.y - entMins.y,
			entMaxs.z - entMins.z
		};
		int thinAxis = 0;
		if (ext[1] < ext[thinAxis])
			thinAxis = 1;
		if (ext[2] < ext[thinAxis])
			thinAxis = 2;
		Vector axisWorld(entCoord[0][thinAxis], entCoord[1][thinAxis], entCoord[2][thinAxis]);
		if (Dot3(axisWorld, refDir) < 0.0f)
			axisWorld = axisWorld * -1.0f;
		return NormalizeSafe(axisWorld, NormalizeSafe(refDir, Vector(1.0f, 0.0f, 0.0f)));
	}
}

void VRGrab::ResetOpenHandPhysics()
{
	s_OpenHand = OpenHandState{};
	ClearDoorAndImpulseCooldowns();
	s_SrvGrab = ServerGrabSession{};
	s_HeldSnapshot.Clear();
#if VR_GRAB_HAS_VR_HANDS
	VRHands::ClearHeldHandPhysicsFeedback();
#endif
	ResetClientCurlSlot(0, false);
	ResetClientCurlSlot(1, true);
	{
		std::lock_guard<std::mutex> gripLock(s_HandGripMutex);
		s_HandGrip[0].Reset();
		s_HandGrip[1].Reset();
	}
	s_CreateMoveUse.Reset();
	s_ServerUseCtrlActive.store(false, std::memory_order_relaxed);
	s_LastSentInUse.store(false, std::memory_order_relaxed);
	s_LastPropDropMs.store(0ULL, std::memory_order_relaxed);
	s_LastDroppedEntIdx.store(-1, std::memory_order_relaxed);
	{
		std::lock_guard<std::mutex> throwLock(s_ThrowHistMutex);
		s_ThrowHist[0].Clear();
		s_ThrowHist[1].Clear();
	}
}

bool VRGrab::FilterServerPlayerUseTarget(Game* game, void* srvEnt, int entIndex, bool isNewUsePress)
{
	if (!srvEnt || !game || !HasModuleRange(game->m_BaseServer, kSrvGlobalEntListRva, sizeof(uintptr_t)))
		return true;

	if (VR* vr = game->m_VR)
	{
		if (vr->m_IsVREnabled && !vr->m_IsHoldingObject)
		{
			const bool leftGrip = EvaluateHandGripInput(vr, true, false);
			const bool rightGrip = EvaluateHandGripInput(vr, false, false);
			const bool leftCanAcquire = leftGrip && !IsHandBusyWithInventory(true);
			const bool rightCanAcquire = rightGrip && !IsHandBusyWithInventory(false);
			if (!leftCanAcquire && !rightCanAcquire)
				return false;
		}
	}

	if (!isNewUsePress)
		return true;

	const uintptr_t entArr = *reinterpret_cast<const uintptr_t*>(game->m_BaseServer + kSrvGlobalEntListRva);
	if (!entArr)
		return true;

	int idx = (entIndex > 0 && entIndex < 4096) ? entIndex : -1;
	if (idx <= 0 && game->m_Offsets && game->m_Offsets->CBaseEntity_entindex.address)
	{
		typedef int(__thiscall* tEntIndex)(void*);
		auto fnEntIndex = reinterpret_cast<tEntIndex>(game->m_Offsets->CBaseEntity_entindex.address);
		const int queried = fnEntIndex(srvEnt);
		if (queried > 0 && queried < 4096)
			idx = queried;
	}
	if (idx <= 0)
	{
		for (uint32_t i = 1; i < 4096u; ++i)
		{
			if (*reinterpret_cast<void* const*>(entArr + 4u + static_cast<uintptr_t>(i) * 24u) == srvEnt)
			{
				idx = static_cast<int>(i);
				break;
			}
		}
	}
	if (idx <= 0)
		return true;

	const uintptr_t info = entArr + 4u + static_cast<uintptr_t>(idx) * 24u;
	if (*reinterpret_cast<void* const*>(info) != srvEnt)
		return true;
	const uint32_t serial = *reinterpret_cast<const uint32_t*>(info + 4u);

	const uint64_t nowMs = GetTickCount64();
	const bool isDoor = OH_IsDoorClass(srvEnt);
	const bool isButton = !isDoor && OH_IsTouchButtonClass(srvEnt);

	// 1. Cross-trigger guard: if open-hand palm push just toggled this door, suppress immediate Grip PlayerUse.
	if (isDoor)
	{
		for (const DoorCooldownSlot& s : s_DoorCooldowns)
		{
			if (s.entIdx == static_cast<uint32_t>(idx) && s.serial == serial &&
			    s.lastOpenHandPushMs != 0ULL && nowMs >= s.lastOpenHandPushMs &&
			    (nowMs - s.lastOpenHandPushMs) < 650ULL)
			{
				return false;
			}
		}
	}
	else if (isButton)
	{
		if (ButtonTouchSlot* bs = FindButtonTouchSlot(static_cast<uint32_t>(idx), serial))
		{
			if (bs->lastTouchClickMs != 0ULL && nowMs >= bs->lastTouchClickMs &&
			    (nowMs - bs->lastTouchClickMs) < 500ULL)
			{
				return false;
			}
		}
	}

	// 2. Rapid retoggle guard on the exact same impulse target (doors / switches / buttons).
	if (s_LastServerImpulseUse.entIdx == static_cast<uint32_t>(idx) &&
	    s_LastServerImpulseUse.serial == serial &&
	    s_LastServerImpulseUse.tickMs != 0ULL &&
	    nowMs >= s_LastServerImpulseUse.tickMs)
	{
		const uint64_t minGapMs = isDoor ? 450ULL : 300ULL;
		if ((nowMs - s_LastServerImpulseUse.tickMs) < minGapMs)
			return false;
	}

	s_LastServerImpulseUse.entIdx = static_cast<uint32_t>(idx);
	s_LastServerImpulseUse.serial = serial;
	s_LastServerImpulseUse.tickMs = nowMs;

	if (isDoor)
		StampDoorGripUseCooldown(static_cast<uint32_t>(idx), serial, nowMs, 1200ULL);
	else if (isButton)
	{
		ButtonTouchSlot* bs = FindOrAllocateButtonTouchSlot(static_cast<uint32_t>(idx), serial);
		const uint64_t targetMs = nowMs + 500ULL;
		if (targetMs > bs->cooldownUntilMs)
			bs->cooldownUntilMs = targetMs;
	}

	return true;
}

void VRGrab::UpdateOpenHandPhysics(Game* game, VR* vr, void* pServerPlayer)
{
	if (game && vr && vr->m_IsVREnabled)
	{
		// Keep player_held_object_use_view_model == 0 and m_bUsingVMGrab == 0 on every server tick
		// BEFORE the very first pickup so AttachEntity never enters Portal 2's singleplayer VM-grab path.
		NeutralizeVMGrabAndClientClone(game, vr, nullptr, pServerPlayer);
	}

	if (!game || !vr || !vr->m_IsVREnabled || !vr->m_ShowHands || vr->m_In3DViewControlMenu ||
		!game->m_BaseServer || !pServerPlayer)
	{
		ResetOpenHandPhysics();
		return;
	}

	if (!HasModuleRange(game->m_BaseServer, kSrvPhysEnvRva, sizeof(void*)) ||
		!HasModuleRange(game->m_BaseServer, kSrvGlobalEntListRva, sizeof(uintptr_t)))
	{
		ResetOpenHandPhysics();
		return;
	}

	void* physEnv = *reinterpret_cast<void* const*>(game->m_BaseServer + kSrvPhysEnvRva);
	const uintptr_t entArr = *reinterpret_cast<const uintptr_t*>(game->m_BaseServer + kSrvGlobalEntListRva);
	if (!physEnv || !entArr)
	{
		ResetOpenHandPhysics();
		return;
	}

	// Entity slot 0 is worldspawn; its (pointer, serial) changes on every level load / save-reload.
	void* worldEnt = *reinterpret_cast<void* const*>(entArr + 4u);
	const uint32_t worldSerial = *reinterpret_cast<const uint32_t*>(entArr + 8u);
	if (!worldEnt)
	{
		ResetOpenHandPhysics();
		return;
	}

	if (!s_OpenHand.keyValid || s_OpenHand.physEnv != physEnv || s_OpenHand.worldEnt != worldEnt ||
		s_OpenHand.worldSerial != worldSerial || s_OpenHand.player != pServerPlayer)
	{
		if (s_OpenHand.keyValid)
			ClearDoorAndImpulseCooldowns();
		s_OpenHand = OpenHandState{};
		s_OpenHand.keyValid = true;
		s_OpenHand.physEnv = physEnv;
		s_OpenHand.worldEnt = worldEnt;
		s_OpenHand.worldSerial = worldSerial;
		s_OpenHand.player = pServerPlayer;
	}

	if (!s_OpenHand.traceQueried)
	{
		s_OpenHand.traceQueried = true;
		s_OpenHand.srvTrace = reinterpret_cast<IEngineTrace*>(
			game->GetInterface("engine.dll", "EngineTraceServer003"));
	}

	// Never touch the environment from inside a physics callback / simulation step.
	const auto fnInSim = OH_Fn<tOH_Bool>(physEnv, kEnvIsInSimulation);
	if (fnInSim && fnInSim(physEnv))
		return;

	const uint64_t nowMs = GetTickCount64();
	const int heldIdx = (vr->m_IsHoldingObject) ? s_SrvGrab.heldEntIndex : -1;
	const uint64_t lastDropMs = s_LastPropDropMs.load(std::memory_order_relaxed);
	const int recentlyDroppedIdx = (lastDropMs != 0ULL && nowMs >= lastDropMs && (nowMs - lastDropMs) < 200ULL)
		? s_LastDroppedEntIdx.load(std::memory_order_relaxed)
		: -1;
	const uint32_t groundHandle = *reinterpret_cast<const uint32_t*>(
		reinterpret_cast<uintptr_t>(pServerPlayer) + kSrvPlayer_GroundEntity);
	const int groundIdx = (groundHandle != 0xFFFFFFFFu) ? static_cast<int>(groundHandle & 0x0FFFu) : -1;
	const tOH_Ent fnCalcAbsPosition = HasExecutableFn(game->m_BaseServer, kSrvFn_CalcAbsolutePosition)
		? reinterpret_cast<tOH_Ent>(game->m_BaseServer + kSrvFn_CalcAbsolutePosition)
		: nullptr;

	for (int handIdx = 0; handIdx < 2; ++handIdx)
	{
		const bool physicalLeft = (handIdx == 1);
		OpenHandSide& side = s_OpenHand.sides[handIdx];

		struct PendingButtonRelease
		{
			uint32_t idx;
			uint32_t serial;
			void* ent;
			tOH_Use fnUse;
			bool momentary;
		};
		PendingButtonRelease pendingReleases[16]{};
		int numBtnReleases = 0;

		auto collectImmediateButtonUnlatch = [&]()
		{
			for (ButtonTouchSlot& bs : s_ButtonTouchSlots)
			{
				if (!bs.latchedByHand[handIdx])
					continue;
				const bool wasMomentary = bs.momentaryOn[handIdx];
				bs.latchedByHand[handIdx] = false;
				bs.clearStartMs[handIdx] = 0ULL;
				bs.momentaryOn[handIdx] = false;
				if (wasMomentary && bs.entIdx > 0u && bs.entIdx < 4096u && numBtnReleases < 16)
				{
					const uintptr_t info = entArr + 4u + static_cast<uintptr_t>(bs.entIdx) * 24u;
					void* bEnt = *reinterpret_cast<void* const*>(info);
					const uint32_t bSer = *reinterpret_cast<const uint32_t*>(info + 4u);
					if (bEnt && bSer == bs.serial &&
					    (*reinterpret_cast<const uint32_t*>(reinterpret_cast<uintptr_t>(bEnt) + kSrvEnt_EFlags) & 0x1u) == 0u)
					{
						if (const auto fnUse = OH_Fn<tOH_Use>(bEnt, kEntUseVtblOffset))
							pendingReleases[numBtnReleases++] = PendingButtonRelease{ bs.entIdx, bSer, bEnt, fnUse, true };
					}
				}
			}
		};

		auto executePendingReleases = [&]()
		{
			for (int i = 0; i < numBtnReleases; ++i)
			{
				const PendingButtonRelease& r = pendingReleases[i];
				const uintptr_t info = entArr + 4u + static_cast<uintptr_t>(r.idx) * 24u;
				if (*reinterpret_cast<void* const*>(info) == r.ent &&
					*reinterpret_cast<const uint32_t*>(info + 4u) == r.serial &&
					(*reinterpret_cast<const uint32_t*>(reinterpret_cast<uintptr_t>(r.ent) + kSrvEnt_EFlags) & 0x1u) == 0u)
				{
					r.fnUse(r.ent, pServerPlayer, pServerPlayer, kUseOff, 0.0f);
				}
			}
			numBtnReleases = 0;
		};

		// Suppress open-hand pushes when gripping or holding a prop; bare hands with extended index finger
		// can still touch world buttons/switches even while middle/ring/pinky fingers are curled.
		const bool holdsProp = vr->m_IsHoldingObject && (vr->m_HoldingWithLeftHand == physicalLeft);
		const bool busyWithInventory = IsHandBusyWithInventory(physicalLeft);
		const bool gripping = EvaluateHandGripInput(vr, physicalLeft, holdsProp);
		const float* curls = physicalLeft ? vr->m_LeftFingerCurl : vr->m_RightFingerCurl;
		const float meanMRP = (curls[2] + curls[3] + curls[4]) * (1.0f / 3.0f);
		const bool preGrabCurling = (meanMRP >= 0.48f) || (curls[1] >= 0.55f);
		const bool useVm0 = vr->m_LeftHanded ? physicalLeft : !physicalLeft;
		const bool isBareHand = useVm0 ? vr->m_RightHandEmpty : !vr->m_FlashlightEquipped;
		const bool canTouchButtonsBase = !gripping && !holdsProp && !busyWithInventory && isBareHand && (curls[1] < 0.72f);
		const bool canPushDoors = !gripping && !holdsProp && !busyWithInventory;

		LocalGripPose gp{};
		const bool gpValid = EvaluateHandGripPose(vr, physicalLeft, gp);
		if (gripping || holdsProp || busyWithInventory || !gpValid)
		{
			side.valid = false;
			side.tipValid = false;
			for (DoorCooldownSlot& ds : s_DoorCooldowns)
			{
				ds.latchedByHand[handIdx] = false;
				ds.clearStartMs[handIdx] = 0ULL;
			}
			collectImmediateButtonUnlatch();
			executePendingReleases();
			continue;
		}

		// Skin-accurate wrist-local VVD palm slab (slabMaxs.y sits within <=0.5 mm of VVD inner palm skin:
		// 0.48u Right cam/light, 0.76u Right v_phone, 0.50u Left v_light; zero 5-7 cm invisible halo!).
		const Vector slabMins = gp.isLeftHandMesh
			? Vector(1.35f, -1.20f, -1.75f)
			: Vector(1.35f, -1.05f, -1.55f);
		const Vector slabMaxs = gp.isLeftHandMesh
			? Vector(4.25f, 0.52f, 1.55f)
			: Vector(4.25f, gp.usePhoneOpenTable ? 0.78f : 0.50f, 1.75f);
		const Vector slabCenterLocal = (slabMins + slabMaxs) * 0.5f;
		const Vector palm = TransformPoint3x4(gp.wristToWorld, slabCenterLocal);
		const float slabBoundRadius = Len3((slabMaxs - slabMins) * 0.5f);

		bool hasIndexTip = false;
		bool hasVisPalm = false;
		Vector visIndexTip(0.0f, 0.0f, 0.0f);
		Vector rawIndexTip(0.0f, 0.0f, 0.0f);
		Vector visPalmSkin = gp.palmSkinWorldPos;
		#if VR_GRAB_HAS_VR_HANDS
		{
			Vector qVisTip(0.0f, 0.0f, 0.0f), qRawTip(0.0f, 0.0f, 0.0f), qVisPalm(0.0f, 0.0f, 0.0f);
			if (VRHands::GetVisualIndexTip(vr, physicalLeft, vr->m_SetupOrigin, qVisTip, &qRawTip, &qVisPalm))
			{
				visIndexTip = qVisTip;
				rawIndexTip = qRawTip;
				visPalmSkin = qVisPalm;
				hasVisPalm = true;
				hasIndexTip = isBareHand;
			}
		}
		#endif

		Vector palmVel(0.0f, 0.0f, 0.0f);
		matrix3x4_t sweepPrevWrist = gp.wristToWorld;
		bool hasSweepStep = false;
		float stepDist = 0.0f;
		float visTipSpeed = 0.0f;
		float rawTipSpeed = 0.0f;
		Vector prevTipForSweep = hasIndexTip ? visIndexTip : palm;
		if (side.valid)
		{
			// Same-tick / sub-5ms re-entry (several usercmd batches in one frame): keep the previous sample.
			if (nowMs <= side.prevTickMs || (nowMs - side.prevTickMs) < 5ULL)
				continue;
			const uint64_t dtMs = nowMs - side.prevTickMs;
			const Vector step = palm - side.prevPalm;
			stepDist = Len3(step);
			if (dtMs <= 150ULL && stepDist <= 48.0f)
			{
				palmVel = step * (1000.0f / static_cast<float>(dtMs));
				const float speed = Len3(palmVel);
				if (speed > 600.0f)
					palmVel = palmVel * (600.0f / speed);
				sweepPrevWrist = side.prevWrist;
				hasSweepStep = true;
			}
			if (hasIndexTip && side.tipValid && dtMs <= 150ULL)
			{
				const float visTipDist = Len3(visIndexTip - side.prevIndexTip);
				if (visTipDist <= 48.0f)
				{
					visTipSpeed = visTipDist * (1000.0f / static_cast<float>(dtMs));
					prevTipForSweep = side.prevIndexTip;
				}
				const float rawTipDist = Len3(rawIndexTip - side.prevRawIndexTip);
				if (rawTipDist <= 48.0f)
					rawTipSpeed = rawTipDist * (1000.0f / static_cast<float>(dtMs));
			}
		}
		side.valid = true;
		side.prevPalm = palm;
		side.prevWrist = gp.wristToWorld;
		side.prevTickMs = nowMs;
		if (hasIndexTip)
		{
			side.tipValid = true;
			side.prevIndexTip = visIndexTip;
			side.prevRawIndexTip = rawIndexTip;
		}
		else
		{
			side.tipValid = false;
		}

		const float tipPressDepth = hasIndexTip ? Len3(rawIndexTip - visIndexTip) : 0.0f;
		const bool canTouchButtons = canTouchButtonsBase && hasIndexTip;
		bool anyLatchedButtonStillClose = false;

		// Update latched button hysteresis & queue momentary USE_OFF when the fingertip pulls away
		for (ButtonTouchSlot& bs : s_ButtonTouchSlots)
		{
			if (!bs.latchedByHand[handIdx])
				continue;

			const uintptr_t info = (bs.entIdx > 0u && bs.entIdx < 4096u)
				? (entArr + 4u + static_cast<uintptr_t>(bs.entIdx) * 24u)
				: 0u;
			void* bEnt = info ? *reinterpret_cast<void* const*>(info) : nullptr;
			const uint32_t bSer = info ? *reinterpret_cast<const uint32_t*>(info + 4u) : 0u;
			if (!bEnt || bSer != bs.serial ||
			    (*reinterpret_cast<const uint32_t*>(reinterpret_cast<uintptr_t>(bEnt) + kSrvEnt_EFlags) & 0x1u) != 0u)
			{
				bs.latchedByHand[handIdx] = false;
				bs.clearStartMs[handIdx] = 0ULL;
				bs.momentaryOn[handIdx] = false;
				continue;
			}

			bool shouldUnlatchNow = !canTouchButtons;
			if (canTouchButtons)
			{
				const uintptr_t bAddr = reinterpret_cast<uintptr_t>(bEnt);
				const uint32_t bEFlags = *reinterpret_cast<const uint32_t*>(bAddr + kSrvEnt_EFlags);
				if ((bEFlags & 0x800u) != 0u && fnCalcAbsPosition)
					fnCalcAbsPosition(bEnt);
				const matrix3x4_t& bCoord = *reinterpret_cast<const matrix3x4_t*>(bAddr + kSrvEnt_CoordFrame);
				Vector bMins(0.0f, 0.0f, 0.0f), bMaxs(0.0f, 0.0f, 0.0f);
				float d = 1e30f;
				if (QueryServerCollideableBounds(bEnt, bMins, bMaxs))
					d = ComputePointToEntityOBBDist(visIndexTip, bCoord, bMins, bMaxs);

				if (d <= 0.50f)
					anyLatchedButtonStillClose = true;

				if (d > kButtonTouchReleaseRadius)
				{
					if (bs.clearStartMs[handIdx] == 0ULL)
						bs.clearStartMs[handIdx] = nowMs;
					if (nowMs >= bs.clearStartMs[handIdx] && (nowMs - bs.clearStartMs[handIdx]) >= kButtonTouchClearMs)
						shouldUnlatchNow = true;
				}
				else
				{
					bs.clearStartMs[handIdx] = 0ULL;
				}
			}

			if (shouldUnlatchNow)
			{
				const bool wasMomentary = bs.momentaryOn[handIdx];
				bs.latchedByHand[handIdx] = false;
				bs.clearStartMs[handIdx] = 0ULL;
				bs.momentaryOn[handIdx] = false;
				if (wasMomentary && numBtnReleases < 16)
				{
					if (const auto fnUse = OH_Fn<tOH_Use>(bEnt, kEntUseVtblOffset))
						pendingReleases[numBtnReleases++] = PendingButtonRelease{ bs.entIdx, bSer, bEnt, fnUse, true };
				}
			}
		}

		// Update latched door hysteresis so a resting hand cannot rapid-retoggle a door.
		for (DoorCooldownSlot& ds : s_DoorCooldowns)
		{
			if (!ds.latchedByHand[handIdx])
				continue;

			const uintptr_t info = (ds.entIdx > 0u && ds.entIdx < 4096u)
				? (entArr + 4u + static_cast<uintptr_t>(ds.entIdx) * 24u)
				: 0u;
			void* dEnt = info ? *reinterpret_cast<void* const*>(info) : nullptr;
			const uint32_t dSer = info ? *reinterpret_cast<const uint32_t*>(info + 4u) : 0u;
			if (!dEnt || dSer != ds.serial ||
			    (*reinterpret_cast<const uint32_t*>(reinterpret_cast<uintptr_t>(dEnt) + kSrvEnt_EFlags) & 0x1u) != 0u)
			{
				ds.latchedByHand[handIdx] = false;
				ds.clearStartMs[handIdx] = 0ULL;
				continue;
			}

			bool shouldUnlatchDoor = !canPushDoors;
			if (canPushDoors)
			{
				const uintptr_t dAddr = reinterpret_cast<uintptr_t>(dEnt);
				const uint32_t dEFlags = *reinterpret_cast<const uint32_t*>(dAddr + kSrvEnt_EFlags);
				if ((dEFlags & 0x800u) != 0u && fnCalcAbsPosition)
					fnCalcAbsPosition(dEnt);
				const matrix3x4_t& dCoord = *reinterpret_cast<const matrix3x4_t*>(dAddr + kSrvEnt_CoordFrame);
				Vector dMins(0.0f, 0.0f, 0.0f), dMaxs(0.0f, 0.0f, 0.0f);
				float dDoor = 1e30f;
				if (QueryServerCollideableBounds(dEnt, dMins, dMaxs))
				{
					const float dPalm = ComputePointToEntityOBBDist(visPalmSkin, dCoord, dMins, dMaxs);
					const float dTip = hasIndexTip ? ComputePointToEntityOBBDist(visIndexTip, dCoord, dMins, dMaxs) : 1e30f;
					dDoor = std::min(dPalm, dTip);
				}

				if (dDoor > kDoorTouchReleaseRadius)
				{
					if (ds.clearStartMs[handIdx] == 0ULL)
						ds.clearStartMs[handIdx] = nowMs;
					if (nowMs >= ds.clearStartMs[handIdx] && (nowMs - ds.clearStartMs[handIdx]) >= kDoorTouchClearMs)
						shouldUnlatchDoor = true;
				}
				else
				{
					ds.clearStartMs[handIdx] = 0ULL;
				}
			}

			if (shouldUnlatchDoor)
			{
				ds.latchedByHand[handIdx] = false;
				ds.clearStartMs[handIdx] = 0ULL;
			}
		}

		const float palmSpeed = Len3(palmVel);
		const bool canSwatProps = !gripping && !holdsProp && !preGrabCurling && (palmSpeed >= kHeavyPropMinSwatSpeed);
		if (!canSwatProps && !canPushDoors && !canTouchButtons)
		{
			executePendingReleases();
			continue;
		}

		matrix3x4_t midWrist = gp.wristToWorld;
		if (hasSweepStep && stepDist > 1.2f)
		{
			midWrist[0][3] = 0.5f * (sweepPrevWrist[0][3] + gp.wristToWorld[0][3]);
			midWrist[1][3] = 0.5f * (sweepPrevWrist[1][3] + gp.wristToWorld[1][3]);
			midWrist[2][3] = 0.5f * (sweepPrevWrist[2][3] + gp.wristToWorld[2][3]);
		}

		struct BestDoorTouch
		{
			bool valid = false;
			uint32_t idx = 0;
			uint32_t serial = 0;
			void* ent = nullptr;
			tOH_Use fnUse = nullptr;
			float dist = 1e30f;
			float impactSpeedUnits = 0.0f;
		};
		struct PendingPropImpulse
		{
			uint32_t idx;
			uint32_t serial;
			void* ent;
			void* physObj;
			tOH_Void fnWake;
			tOH_ForceOffset fnForceOffset;
			Vector impulse;
			Vector closestWorld;
			float closing;
		};
		struct BestButtonTouch
		{
			bool valid = false;
			uint32_t idx = 0;
			uint32_t serial = 0;
			void* ent = nullptr;
			tOH_Use fnUse = nullptr;
			bool isOnOff = false;
			float dist = 1e30f;
			float centerDist = 1e30f;
		};
		BestDoorTouch bestDoor{};
		PendingPropImpulse pendingProps[kMaxImpulsesPerHand]{};
		int numProps = 0;
		BestButtonTouch bestButton{};

		for (uint32_t idx = 1; idx < 4096u && (canTouchButtons || canPushDoors || numProps < kMaxImpulsesPerHand); ++idx)
		{
			const uintptr_t info = entArr + 4u + static_cast<uintptr_t>(idx) * 24u;
			void* ent = *reinterpret_cast<void* const*>(info);
			if (!ent || ent == pServerPlayer || ent == worldEnt || static_cast<int>(idx) == heldIdx ||
			    static_cast<int>(idx) == recentlyDroppedIdx || static_cast<int>(idx) == groundIdx)
				continue;

			const uint32_t serial = *reinterpret_cast<const uint32_t*>(info + 4u);
			const uintptr_t entAddr = reinterpret_cast<uintptr_t>(ent);
			// Check EFL_KILLME first before inspecting any collision/physics fields.
			const uint32_t eFlags = *reinterpret_cast<const uint32_t*>(entAddr + kSrvEnt_EFlags);
			if ((eFlags & 0x1u) != 0u)
				continue;

			const bool isTouchButton = canTouchButtons && OH_IsTouchButtonClass(ent);
			const bool isDoor = !isTouchButton && canPushDoors && OH_IsDoorClass(ent);
			if (!isTouchButton && !isDoor && !canSwatProps)
				continue;

			const uint8_t solidType = *reinterpret_cast<const uint8_t*>(entAddr + kSrvEnt_SolidType);
			const uint32_t entEffects = *reinterpret_cast<const uint32_t*>(entAddr + kSrvEnt_Effects);
			const uint16_t solidFlags = *reinterpret_cast<const uint16_t*>(entAddr + kSrvEnt_SolidFlags);
			if (!isTouchButton)
			{
				const float entRadius = *reinterpret_cast<const float*>(entAddr + kSrvEnt_CollisionRad);
				if (!(entRadius > 0.01f) || entRadius > 4096.0f || solidType == 0u)
					continue;
				if (((entEffects & 0x20u) != 0u) || ((solidFlags & 0x0Cu) != 0u))
					continue;
			}

			if ((eFlags & 0x800u) != 0u && fnCalcAbsPosition) // EFL_DIRTY_ABSTRANSFORM
				fnCalcAbsPosition(ent);

			const matrix3x4_t& coord = *reinterpret_cast<const matrix3x4_t*>(entAddr + kSrvEnt_CoordFrame);
			const Vector origin(coord[0][3], coord[1][3], coord[2][3]);

			if (isTouchButton)
			{
				if (nowMs < s_ButtonTouchSuppressUntilMs || anyLatchedButtonStillClose ||
				    (side.lastButtonClickMs != 0ULL && nowMs >= side.lastButtonClickMs &&
				     (nowMs - side.lastButtonClickMs) < kButtonHandGlobalCooldownMs))
					continue;

				const auto fnCaps = OH_Fn<tOH_Int>(ent, kEntObjectCapsVtblOffset);
				if (!fnCaps)
					continue;
				const int caps = fnCaps(ent);
				if ((caps & (kFcapImpulseUse | kFcapOnOffUse)) == 0)
					continue;

				Vector mins(0.0f, 0.0f, 0.0f), maxs(0.0f, 0.0f, 0.0f);
				ICollideable* entColl = nullptr;
				if (!QueryServerCollideableBounds(ent, mins, maxs, &entColl))
					continue;
				const float obbDiag = Len3(maxs - mins);
				if (maxs.x < mins.x || maxs.y < mins.y || maxs.z < mins.z || obbDiag < 0.05f)
					continue;

				const Vector obbCenterWorld = TransformPoint3x4(coord, (mins + maxs) * 0.5f);
				const float obbHalfDiag = obbDiag * 0.5f;
				if (Len3(obbCenterWorld - visIndexTip) > obbHalfDiag + kButtonTouchEngageRadius + 2.5f)
					continue;

				const float dTip = ComputePointToEntityOBBDist(visIndexTip, coord, mins, maxs);
				if (dTip > kButtonTouchEngageRadius)
					continue;

				if (solidType == 6u && (solidFlags & 0x04u) == 0u && s_OpenHand.srvTrace && entColl)
				{
					float trFrac = 1.0f;
					bool startSolid = false;
					if (!ClipSweptSphereToCollideable(s_OpenHand.srvTrace, entColl, prevTipForSweep, visIndexTip, 0.28f, trFrac, startSolid))
						continue;
				}

				if (ButtonTouchSlot* bs = FindButtonTouchSlot(idx, serial))
				{
					if (bs->latchedByHand[handIdx] || nowMs < bs->cooldownUntilMs)
						continue;
				}
				if (std::max(rawTipSpeed, visTipSpeed) < kButtonMinApproachSpeed && tipPressDepth < 0.08f)
					continue;

				const float cDist = Len3(obbCenterWorld - visIndexTip);
				const auto fnUse = OH_Fn<tOH_Use>(ent, kEntUseVtblOffset);
				if (fnUse && (!bestButton.valid || dTip + 0.12f < bestButton.dist ||
				    (std::fabs(dTip - bestButton.dist) <= 0.12f && cDist < bestButton.centerDist)))
				{
					bestButton.valid = true;
					bestButton.idx = idx;
					bestButton.serial = serial;
					bestButton.ent = ent;
					bestButton.fnUse = fnUse;
					bestButton.isOnOff = ((caps & kFcapOnOffUse) != 0);
					bestButton.dist = dTip;
					bestButton.centerDist = cDist;
				}
				continue;
			}

			if (isDoor)
			{
				const auto fnCaps = OH_Fn<tOH_Int>(ent, kEntObjectCapsVtblOffset);
				if (fnCaps && (fnCaps(ent) & (kFcapImpulseUse | kFcapOnOffUse)) == 0)
					continue;

				Vector mins(0.0f, 0.0f, 0.0f), maxs(0.0f, 0.0f, 0.0f);
				ICollideable* entColl = nullptr;
				if (!QueryServerCollideableBounds(ent, mins, maxs, &entColl))
					continue;

				const Vector obbCenterWorld = TransformPoint3x4(coord, (mins + maxs) * 0.5f);
				const float obbHalfDiag = Len3(maxs - mins) * 0.5f;
				if (Len3(obbCenterWorld - palm) > obbHalfDiag + 8.5f + stepDist)
					continue;

				if (DoorCooldownSlot* ds = FindDoorCooldownSlot(idx, serial))
				{
					if (ds->latchedByHand[handIdx] || nowMs < ds->nextMs)
						continue;
				}

				const float dVisPalm = ComputePointToEntityOBBDist(visPalmSkin, coord, mins, maxs);
				const float dVisTip  = hasIndexTip ? ComputePointToEntityOBBDist(visIndexTip, coord, mins, maxs) : 1e30f;
				const float dRawPalm = ComputePointToEntityOBBDist(gp.palmSkinWorldPos, coord, mins, maxs);
				const float dMinVis  = std::min(dVisPalm, dVisTip);
				const Vector doorHandMins(1.20f, -1.35f, -1.85f);
				const Vector doorHandMaxs(7.20f,  1.20f,  1.85f);
				const bool doorHit =
					(dMinVis <= kDoorTouchEngageRadius) ||
					(dRawPalm <= 0.45f) ||
					OH_IntersectOrientedBoxes(gp.wristToWorld, doorHandMins, doorHandMaxs, coord, mins, maxs) ||
					(hasSweepStep && stepDist > 0.8f &&
						OH_IntersectOrientedBoxes(midWrist, doorHandMins, doorHandMaxs, coord, mins, maxs));
				if (!doorHit)
					continue;

				const Vector n = OH_ComputeDoorInwardNormal(gp.wristToWorld, coord, mins, maxs, palmVel);
				const float push = Dot3(palmVel, n);
				const float pressDepth = hasVisPalm ? std::max(0.0f, Dot3(gp.palmSkinWorldPos - visPalmSkin, n)) : 0.0f;
				const bool movingIntoDoor   = (push >= 1.5f);
				const bool pressingIntoDoor = (pressDepth >= 0.12f) || (tipPressDepth >= 0.10f);
				const bool pokingWithTip    = hasIndexTip && (dVisTip <= 0.45f) && (std::max(rawTipSpeed, visTipSpeed) >= 1.5f) && (push >= -1.0f);
				if (!movingIntoDoor && !pressingIntoDoor && !pokingWithTip)
					continue;

				const auto fnUse = OH_Fn<tOH_Use>(ent, kEntUseVtblOffset);
				if (fnUse && (!bestDoor.valid || dMinVis < bestDoor.dist))
				{
					bestDoor.valid = true;
					bestDoor.idx = idx;
					bestDoor.serial = serial;
					bestDoor.ent = ent;
					bestDoor.fnUse = fnUse;
					bestDoor.dist = dMinVis;
					bestDoor.impactSpeedUnits = std::max({ palmSpeed, std::max(rawTipSpeed, visTipSpeed), std::max(0.0f, push) });
				}
				continue;
			}

			if (!canSwatProps)
				continue;
			const float entRadius = *reinterpret_cast<const float*>(entAddr + kSrvEnt_CollisionRad);
			if (Len3(origin - palm) > entRadius * 2.0f + slabBoundRadius + stepDist + 2.0f)
				continue;

			void* physObj = *reinterpret_cast<void* const*>(entAddr + kSrvEnt_PhysicsObject);
			if (!physObj)
				continue;

			Vector mins(0.0f, 0.0f, 0.0f), maxs(0.0f, 0.0f, 0.0f);
			ICollideable* entColl = nullptr;
			if (!QueryServerCollideableBounds(ent, mins, maxs, &entColl))
				continue;

			const bool slabHit =
				OH_IntersectOrientedBoxes(gp.wristToWorld, slabMins, slabMaxs, coord, mins, maxs) ||
				(hasSweepStep && stepDist > 1.2f &&
					OH_IntersectOrientedBoxes(midWrist, slabMins, slabMaxs, coord, mins, maxs));
			if (!slabHit)
				continue;

			Vector closestWorld(0.0f, 0.0f, 0.0f);
			const Vector n = OH_ComputeContactAndInwardNormal(
				gp.wristToWorld, slabMins, slabMaxs, coord, mins, maxs, palmVel, closestWorld);

			// Require deliberate inward motion toward the contact rather than a tangential graze.
			if (Dot3(palmVel, n) < 0.35f * palmSpeed)
				continue;

			if (*reinterpret_cast<const uint8_t*>(entAddr + kSrvEnt_MoveType) != kMoveTypeVPhysics)
				continue;

			// Clip a 3x3 grid of skin-flush palm-pad spheres against the entity's exact VPhysics shape
			// (EngineTraceServer003::ClipRayToCollideable) so cylindrical OBB air corners never false-hit.
			if (s_OpenHand.srvTrace && entColl)
			{
				constexpr float kPadRad = 0.36f;
				const bool dorsalStrike = (Dot3(palmVel, gp.palmOutwardNormal) < -0.20f * palmSpeed);
				const float faceY = dorsalStrike ? (slabMins.y + kPadRad) : (slabMaxs.y - kPadRad);
				const float xs[3] = { 1.85f, 2.85f, 3.85f };
				const float zLo = slabMins.z + 0.42f;
				const float zHi = slabMaxs.z - 0.42f;
				const float zs[3] = { 0.5f * (zLo + zHi), zHi, zLo };

				bool hitVPhys = false;
				for (int xi = 0; xi < 3 && !hitVPhys; ++xi)
				{
					for (int zi = 0; zi < 3; ++zi)
					{
						const Vector sLocal(xs[xi], faceY, zs[zi]);
						const Vector pStart = TransformPoint3x4(sweepPrevWrist, sLocal);
						const Vector pEnd = TransformPoint3x4(gp.wristToWorld, sLocal);
						float trFrac = 1.0f;
						bool startSolid = false;
						if (ClipSweptSphereToCollideable(s_OpenHand.srvTrace, entColl, pStart, pEnd, kPadRad, trFrac, startSolid))
						{
							hitVPhys = true;
							break;
						}
					}
				}
				if (!hitVPhys)
					continue;
			}

			const uintptr_t objVtbl = *reinterpret_cast<const uintptr_t*>(physObj);
			if (!objVtbl)
				continue;
			const auto fnIsStatic = OH_Fn<tOH_Bool>(physObj, 0x04);
			const auto fnIsMoveable = OH_Fn<tOH_Bool>(physObj, 0x28);
			const auto fnGetGameData = OH_Fn<tOH_Ptr>(physObj, 0x44);
			const auto fnWake = OH_Fn<tOH_Void>(physObj, 0x60);
			const auto fnGetMass = OH_Fn<tOH_Float>(physObj, 0x74);
			const auto fnGetVelocity = OH_Fn<tOH_GetVelocity>(physObj, 0xD0);
			const auto fnForceOffset = OH_Fn<tOH_ForceOffset>(physObj, 0xF4);
			if (!fnIsStatic || !fnIsMoveable || !fnGetGameData || !fnWake || !fnGetMass || !fnGetVelocity || !fnForceOffset)
				continue;

			if (fnGetGameData(physObj) != ent || fnIsStatic(physObj) || !fnIsMoveable(physObj))
				continue;
			const float mass = fnGetMass(physObj);
			if (!(mass > 0.01f) || mass > 1.0e6f)
				continue;

			const float minRequiredSpeed = (mass <= 15.0f) ? kLightPropMinSwatSpeed : kHeavyPropMinSwatSpeed;
			if (palmSpeed < minRequiredSpeed)
				continue;

			Vector objVel(0.0f, 0.0f, 0.0f), objAngVel(0.0f, 0.0f, 0.0f);
			fnGetVelocity(physObj, &objVel, &objAngVel);

			const float closing = ClampF(Dot3(palmVel - objVel, n), 0.0f, kMaxClosingSpeed);
			if (closing < minRequiredSpeed)
				continue;

			// Two-body impulse of a 2 kg hand against the prop (heavy props barely budge, light ones fly smoothly).
			const float reduced = (kHandEffectiveMassKg * mass) / (kHandEffectiveMassKg + mass);
			const float J = (1.0f + kHandRestitution) * reduced * closing;
			const Vector impulse = n * J;

			if (numProps < kMaxImpulsesPerHand)
			{
				pendingProps[numProps++] = PendingPropImpulse{
					idx, serial, ent, physObj, fnWake, fnForceOffset, impulse, closestWorld, closing
				};
			}
		}

		// Execute deferred side effects safely after finishing the global entity list scan.
		executePendingReleases();

		if (bestButton.valid)
		{
			const uintptr_t info = entArr + 4u + static_cast<uintptr_t>(bestButton.idx) * 24u;
			if (*reinterpret_cast<void* const*>(info) == bestButton.ent &&
				*reinterpret_cast<const uint32_t*>(info + 4u) == bestButton.serial &&
				(*reinterpret_cast<const uint32_t*>(reinterpret_cast<uintptr_t>(bestButton.ent) + kSrvEnt_EFlags) & 0x1u) == 0u)
			{
				bestButton.fnUse(bestButton.ent, pServerPlayer, pServerPlayer, bestButton.isOnOff ? kUseOn : kUseToggle, 0.0f);
				ButtonTouchSlot* bs = FindOrAllocateButtonTouchSlot(bestButton.idx, bestButton.serial);
				bs->latchedByHand[handIdx] = true;
				bs->clearStartMs[handIdx] = 0ULL;
				bs->momentaryOn[handIdx] = bestButton.isOnOff;
				bs->cooldownUntilMs = nowMs + kButtonTouchCooldownMs;
				bs->lastTouchClickMs = nowMs;
				side.lastButtonClickMs = nowMs;
				s_LastServerImpulseUse.entIdx = bestButton.idx;
				s_LastServerImpulseUse.serial = bestButton.serial;
				s_LastServerImpulseUse.tickMs = nowMs;
				const float btnSpeedMps = std::max(
					UnitsPerSecToMps(vr, std::max(rawTipSpeed, visTipSpeed)),
					GetControllerSpeedMps(vr, physicalLeft));
				const float btnAmp = VR::ComputeImpactHapticAmplitude(btnSpeedMps, 0.08f, 2.3f);
				vr->TriggerHapticPulseOnHand(physicalLeft, 800, btnAmp);
			}
		}

		if (bestDoor.valid)
		{
			const uintptr_t info = entArr + 4u + static_cast<uintptr_t>(bestDoor.idx) * 24u;
			if (*reinterpret_cast<void* const*>(info) == bestDoor.ent &&
				*reinterpret_cast<const uint32_t*>(info + 4u) == bestDoor.serial &&
				(*reinterpret_cast<const uint32_t*>(reinterpret_cast<uintptr_t>(bestDoor.ent) + kSrvEnt_EFlags) & 0x1u) == 0u)
			{
				bestDoor.fnUse(bestDoor.ent, pServerPlayer, pServerPlayer, kUseToggle, 0.0f);
				DoorCooldownSlot* ds = FindOrAllocateDoorCooldownSlot(bestDoor.idx, bestDoor.serial);
				ds->nextMs = nowMs + kDoorCooldownMs;
				ds->lastOpenHandPushMs = nowMs;
				ds->latchedByHand[handIdx] = true;
				ds->clearStartMs[handIdx] = 0ULL;
				s_LastServerImpulseUse.entIdx = bestDoor.idx;
				s_LastServerImpulseUse.serial = bestDoor.serial;
				s_LastServerImpulseUse.tickMs = nowMs;
				const float doorSpeedMps = std::max(
					UnitsPerSecToMps(vr, bestDoor.impactSpeedUnits),
					GetControllerSpeedMps(vr, physicalLeft));
				const float doorAmp = VR::ComputeImpactHapticAmplitude(doorSpeedMps, 0.10f, 2.3f);
				TriggerControllerHaptic(vr, physicalLeft, 850, doorAmp, 80ULL, side.lastHapticMs);
			}
		}

		if (numProps > 0 && *reinterpret_cast<void* const*>(game->m_BaseServer + kSrvPhysEnvRva) == physEnv)
		{
			for (int i = 0; i < numProps; ++i)
			{
				const PendingPropImpulse& p = pendingProps[i];
				const uintptr_t info = entArr + 4u + static_cast<uintptr_t>(p.idx) * 24u;
				if (*reinterpret_cast<void* const*>(info) == p.ent &&
					*reinterpret_cast<const uint32_t*>(info + 4u) == p.serial &&
					(*reinterpret_cast<const uint32_t*>(reinterpret_cast<uintptr_t>(p.ent) + kSrvEnt_EFlags) & 0x1u) == 0u &&
					*reinterpret_cast<void* const*>(reinterpret_cast<uintptr_t>(p.ent) + kSrvEnt_PhysicsObject) == p.physObj)
				{
					p.fnWake(p.physObj);
					p.fnForceOffset(p.physObj, p.impulse, p.closestWorld);
					const float propSpeedMps = std::max(
						UnitsPerSecToMps(vr, p.closing),
						GetControllerSpeedMps(vr, physicalLeft));
					const float propAmp = VR::ComputeImpactHapticAmplitude(propSpeedMps, 0.10f, 2.3f);
					TriggerControllerHaptic(vr, physicalLeft, 900, propAmp, 70ULL, side.lastHapticMs);
				}
			}
		}
	}
}
