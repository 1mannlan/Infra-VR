#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "vr_hands.h"
#include "vr.h"
#include "game.h"
#include "sdk.h"
#include "trace.h"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>

namespace
{
	constexpr float kPi = 3.14159265358979323846f;
	constexpr float kDeg2Rad = kPi / 180.0f;
	constexpr float kRad2Deg = 180.0f / kPi;

	// Visual hand collision & coupling constants calibrated to exact player_hands VVD skin geometry.
	constexpr float kPalmCoreRadius = 0.44f;         // ~10.2 mm compact anti-tunneling core strictly inside mid-palm
	constexpr float kHeldSafetyWallRadius = 0.72f;   // Conservative static-world mid-palm safety hull while holding props
	constexpr float kShoulderHullRadius = 1.0f;      // HMD -> shoulder safety hull
	constexpr float kTargetSkinClearance = 0.018f;   // ~0.42 mm target clearance (+0.72 mm engine DIST_EPSILON => ~1.1 mm)
	constexpr float kMaxTetherDist = 18.0f;          // ~41.7 cm max visual stretch from live controller

	// Distal phalanx fingertip local +X extents (measured from player_hands VVD mesh)
	static constexpr float kLeftTipExtent[5]  = { 0.995f, 1.035f, 1.037f, 0.882f, 0.875f };
	static constexpr float kRightTipExtent[5] = { 1.057f, 1.065f, 1.549f, 0.938f, 0.857f };

	// Graduated phalanx capsule radii (decreasing by >= 0.04u distally so child joint pivots stay clear of startsolid)
	static constexpr float kThumbPhalanxRadii[3] = { 0.34f, 0.29f, 0.24f };
	static constexpr float kFingerPhalanxRadii[4][3] = {
		{ 0.32f, 0.27f, 0.22f }, // Index
		{ 0.32f, 0.27f, 0.22f }, // Middle
		{ 0.30f, 0.25f, 0.20f }, // Ring
		{ 0.28f, 0.23f, 0.19f }  // Pinky
	};
	static constexpr float kSplayWeights[4]     = { -1.0f, -0.35f, 0.35f, 1.0f };
	static constexpr float kSurfaceSpreadDeg[4] = { 10.5f, 3.0f, 3.5f, 11.5f };
	constexpr float kHeldBlendMinPos = 0.9f;         // Free-air 1-tick lag allowance (~2.1 cm)
	constexpr float kHeldBlendFullPos = 3.2f;        // Full prop coupling deflection (~7.4 cm)
	constexpr float kHeldBlendMinRotDeg = 5.0f;
	constexpr float kHeldBlendFullRotDeg = 16.0f;
	constexpr float kHeldFreeAirMinRotDeg = 18.0f;   // Unblocked free-air rotational lag deadzone during fast 0->140 deg tilts
	constexpr float kHeldFreeAirFullRotDeg = 35.0f;
	constexpr unsigned int kMaskOpenHandSolid =
		CONTENTS_SOLID | CONTENTS_MOVEABLE | CONTENTS_WINDOW | CONTENTS_GRATE;
	constexpr unsigned int kMaskStaticWorldSolid =
		CONTENTS_SOLID | CONTENTS_WINDOW | CONTENTS_GRATE;

	enum class HandSkinPreset : uint8_t
	{
		RightCam   = 0, // v_camera.mdl / v_light.mdl Right Hand
		RightPhone = 1, // v_phone.mdl Right Hand
		RightSuper = 2, // v_superlight.mdl Right Hand
		LeftLight  = 3, // v_light.mdl Left Hand
		LeftSuper  = 4, // v_superlight.mdl Left Hand
		LeftPhone  = 5  // v_phone.mdl Left Hand
	};

	struct DrawModelStateSubset
	{
		void* m_pStudioHdr;
		void* m_pStudioHWData;
		void* m_pRenderable;
		const matrix3x4_t* m_pModelToWorld;
	};

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
				found->IncrementReferenceCount();
				mat = found;
				return mat;
			}
			return nullptr;
		}
	};

	inline float ClampF(float v, float lo, float hi)
	{
		return (v < lo) ? lo : ((v > hi) ? hi : v);
	}

	inline void Mul3x4(const matrix3x4_t& A, const matrix3x4_t& B, matrix3x4_t& out)
	{
		for (int r = 0; r < 3; ++r)
		{
			out[r][0] = A[r][0] * B[0][0] + A[r][1] * B[1][0] + A[r][2] * B[2][0];
			out[r][1] = A[r][0] * B[0][1] + A[r][1] * B[1][1] + A[r][2] * B[2][1];
			out[r][2] = A[r][0] * B[0][2] + A[r][1] * B[1][2] + A[r][2] * B[2][2];
			out[r][3] = A[r][0] * B[0][3] + A[r][1] * B[1][3] + A[r][2] * B[2][3] + A[r][3];
		}
	}

	inline void InvertOrtho3x4(const matrix3x4_t& m, matrix3x4_t& inv)
	{
		inv[0][0] = m[0][0]; inv[0][1] = m[1][0]; inv[0][2] = m[2][0];
		inv[1][0] = m[0][1]; inv[1][1] = m[1][1]; inv[1][2] = m[2][1];
		inv[2][0] = m[0][2]; inv[2][1] = m[1][2]; inv[2][2] = m[2][2];
		inv[0][3] = -(inv[0][0] * m[0][3] + inv[0][1] * m[1][3] + inv[0][2] * m[2][3]);
		inv[1][3] = -(inv[1][0] * m[0][3] + inv[1][1] * m[1][3] + inv[1][2] * m[2][3]);
		inv[2][3] = -(inv[2][0] * m[0][3] + inv[2][1] * m[1][3] + inv[2][2] * m[2][3]);
	}

	inline Vector TransformPoint3x4(const matrix3x4_t& m, float x, float y, float z)
	{
		return Vector(
			m[0][0] * x + m[0][1] * y + m[0][2] * z + m[0][3],
			m[1][0] * x + m[1][1] * y + m[1][2] * z + m[1][3],
			m[2][0] * x + m[2][1] * y + m[2][2] * z + m[2][3]);
	}

	inline Vector NormalizeSafe(const Vector& v, const Vector& fallback)
	{
		float lenSq = v.x * v.x + v.y * v.y + v.z * v.z;
		if (lenSq > 1e-8f)
		{
			float inv = 1.0f / std::sqrt(lenSq);
			return Vector(v.x * inv, v.y * inv, v.z * inv);
		}
		return fallback;
	}

	inline Vector RotateVec3x4(const matrix3x4_t& m, const Vector& v)
	{
		return Vector(
			m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z,
			m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
			m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z);
	}

	inline float Smoothstep01(float edge0, float edge1, float x)
	{
		if (edge1 <= edge0)
			return (x >= edge1) ? 1.0f : 0.0f;
		float t = ClampF((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
		return t * t * (3.0f - 2.0f * t);
	}

	inline void AnglePosToMatrix3x4(const QAngle& ang, const Vector& pos, matrix3x4_t& out)
	{
		Vector fwd, right, up;
		QAngle::AngleVectors(ang, &fwd, &right, &up);
		const Vector left(-right.x, -right.y, -right.z);
		out = matrix3x4_t(fwd, left, up, pos);
	}

	inline void Matrix3x4ToAngles(const matrix3x4_t& m, Vector& outPos, QAngle& outAng)
	{
		outPos = Vector(m[0][3], m[1][3], m[2][3]);
		const float xyDist = std::sqrt(m[0][0] * m[0][0] + m[1][0] * m[1][0]);
		if (xyDist > 1e-6f)
		{
			outAng.y = std::atan2(m[1][0], m[0][0]) * kRad2Deg;
			outAng.x = std::atan2(-m[2][0], xyDist) * kRad2Deg;
			outAng.z = std::atan2(m[2][1], m[2][2]) * kRad2Deg;
		}
		else
		{
			outAng.y = std::atan2(-m[0][1], m[1][1]) * kRad2Deg;
			outAng.x = std::atan2(-m[2][0], xyDist) * kRad2Deg;
			outAng.z = 0.0f;
		}
	}

	struct Quat4
	{
		float w = 1.0f, x = 0.0f, y = 0.0f, z = 0.0f;
	};

	inline Quat4 Mat3ToQuat(const matrix3x4_t& m)
	{
		Quat4 q{};
		const float trace = m[0][0] + m[1][1] + m[2][2];
		if (trace > 0.0f)
		{
			const float s = std::sqrt(trace + 1.0f) * 2.0f;
			q.w = 0.25f * s;
			q.x = (m[2][1] - m[1][2]) / s;
			q.y = (m[0][2] - m[2][0]) / s;
			q.z = (m[1][0] - m[0][1]) / s;
		}
		else if (m[0][0] > m[1][1] && m[0][0] > m[2][2])
		{
			const float s = std::sqrt(std::max(1.0f + m[0][0] - m[1][1] - m[2][2], 1e-8f)) * 2.0f;
			q.w = (m[2][1] - m[1][2]) / s;
			q.x = 0.25f * s;
			q.y = (m[0][1] + m[1][0]) / s;
			q.z = (m[0][2] + m[2][0]) / s;
		}
		else if (m[1][1] > m[2][2])
		{
			const float s = std::sqrt(std::max(1.0f + m[1][1] - m[0][0] - m[2][2], 1e-8f)) * 2.0f;
			q.w = (m[0][2] - m[2][0]) / s;
			q.x = (m[0][1] + m[1][0]) / s;
			q.y = 0.25f * s;
			q.z = (m[1][2] + m[2][1]) / s;
		}
		else
		{
			const float s = std::sqrt(std::max(1.0f + m[2][2] - m[0][0] - m[1][1], 1e-8f)) * 2.0f;
			q.w = (m[1][0] - m[0][1]) / s;
			q.x = (m[0][2] + m[2][0]) / s;
			q.y = (m[1][2] + m[2][1]) / s;
			q.z = 0.25f * s;
		}
		const float norm = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
		if (norm > 1e-8f)
		{
			const float inv = 1.0f / norm;
			q.w *= inv; q.x *= inv; q.y *= inv; q.z *= inv;
		}
		return q;
	}

	inline void QuatToMat3(const Quat4& q, matrix3x4_t& m)
	{
		const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
		const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
		const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
		m[0][0] = 1.0f - 2.0f * (yy + zz);
		m[0][1] = 2.0f * (xy - wz);
		m[0][2] = 2.0f * (xz + wy);
		m[1][0] = 2.0f * (xy + wz);
		m[1][1] = 1.0f - 2.0f * (xx + zz);
		m[1][2] = 2.0f * (yz - wx);
		m[2][0] = 2.0f * (xz - wy);
		m[2][1] = 2.0f * (yz + wx);
		m[2][2] = 1.0f - 2.0f * (xx + yy);
	}

	inline void SlerpOrtho3x4(const matrix3x4_t& a, const matrix3x4_t& b, float t, const Vector& pos, matrix3x4_t& out)
	{
		const float alpha = ClampF(t, 0.0f, 1.0f);
		Quat4 qa = Mat3ToQuat(a);
		Quat4 qb = Mat3ToQuat(b);
		float dot = qa.w * qb.w + qa.x * qb.x + qa.y * qb.y + qa.z * qb.z;
		if (dot < 0.0f)
		{
			dot = -dot;
			qb.w = -qb.w; qb.x = -qb.x; qb.y = -qb.y; qb.z = -qb.z;
		}
		Quat4 qr{};
		if (dot > 0.9995f)
		{
			qr.w = qa.w + (qb.w - qa.w) * alpha;
			qr.x = qa.x + (qb.x - qa.x) * alpha;
			qr.y = qa.y + (qb.y - qa.y) * alpha;
			qr.z = qa.z + (qb.z - qa.z) * alpha;
		}
		else
		{
			const float theta = std::acos(ClampF(dot, -1.0f, 1.0f));
			const float sinTheta = std::sin(theta);
			const float w0 = std::sin((1.0f - alpha) * theta) / sinTheta;
			const float w1 = std::sin(alpha * theta) / sinTheta;
			qr.w = qa.w * w0 + qb.w * w1;
			qr.x = qa.x * w0 + qb.x * w1;
			qr.y = qa.y * w0 + qb.y * w1;
			qr.z = qa.z * w0 + qb.z * w1;
		}
		const float n = std::sqrt(qr.w * qr.w + qr.x * qr.x + qr.y * qr.y + qr.z * qr.z);
		if (n > 1e-8f)
		{
			const float inv = 1.0f / n;
			qr.w *= inv; qr.x *= inv; qr.y *= inv; qr.z *= inv;
		}
		QuatToMat3(qr, out);
		out[0][3] = pos.x;
		out[1][3] = pos.y;
		out[2][3] = pos.z;
	}

	inline float ComputeRotationDeltaDeg(const matrix3x4_t& a, const matrix3x4_t& b)
	{
		float tr = 0.0f;
		for (int c = 0; c < 3; ++c)
			tr += a[0][c] * b[0][c] + a[1][c] * b[1][c] + a[2][c] * b[2][c];
		const float cosHalf = ClampF(0.5f * (tr - 1.0f), -1.0f, 1.0f);
		return std::acos(cosHalf) * kRad2Deg;
	}

	inline bool IsRotationBasisValid(const matrix3x4_t& m)
	{
		const float l0 = m[0][0] * m[0][0] + m[1][0] * m[1][0] + m[2][0] * m[2][0];
		const float l1 = m[0][1] * m[0][1] + m[1][1] * m[1][1] + m[2][1] * m[2][1];
		const float l2 = m[0][2] * m[0][2] + m[1][2] * m[1][2] + m[2][2] * m[2][2];
		return (l0 > 0.5f && l0 < 1.5f && l1 > 0.5f && l1 < 1.5f && l2 > 0.5f && l2 < 1.5f);
	}

	// Lock-free C++17 seqlock cache storing the latest rendered model-to-wrist transform
	// and active open-pose table flag for vm0 (slot 0 = Right Hand mesh) and vm1 (slot 1 = Left Hand mesh).
	struct SeqlockMatrixSlot
	{
		std::atomic<uint32_t> seq{ 0u };
		std::atomic<bool> active{ false };
		std::atomic<float> m[3][4]{
			{ { 0.0f }, { 0.0f }, { 0.0f }, { 0.0f } },
			{ { 0.0f }, { 0.0f }, { 0.0f }, { 0.0f } },
			{ { 0.0f }, { 0.0f }, { 0.0f }, { 0.0f } }
		};
		std::atomic<unsigned long long> tickMs{ 0ULL };
		std::atomic<bool> usePhoneOpenTable{ false };
		std::atomic<uint8_t> skinPreset{ static_cast<uint8_t>(HandSkinPreset::RightCam) };

		void Write(const matrix3x4_t& mat, bool phoneOpen, HandSkinPreset preset, unsigned long long nowMs)
		{
			uint32_t s = seq.load(std::memory_order_relaxed);
			seq.store(s + 1u, std::memory_order_seq_cst);
			std::atomic_thread_fence(std::memory_order_seq_cst);
			for (int r = 0; r < 3; ++r)
			{
				for (int c = 0; c < 4; ++c)
					m[r][c].store(mat.m_matrix[r][c], std::memory_order_relaxed);
			}
			tickMs.store(nowMs, std::memory_order_relaxed);
			usePhoneOpenTable.store(phoneOpen, std::memory_order_relaxed);
			skinPreset.store(static_cast<uint8_t>(preset), std::memory_order_relaxed);
			active.store(true, std::memory_order_relaxed);
			std::atomic_thread_fence(std::memory_order_release);
			seq.store(s + 2u, std::memory_order_release);
		}

		void Clear()
		{
			const uint32_t s = seq.load(std::memory_order_relaxed);
			seq.store(s + 1u, std::memory_order_seq_cst);
			std::atomic_thread_fence(std::memory_order_seq_cst);
			active.store(false, std::memory_order_relaxed);
			tickMs.store(0ULL, std::memory_order_relaxed);
			std::atomic_thread_fence(std::memory_order_release);
			seq.store(s + 2u, std::memory_order_release);
		}

		bool Read(
			matrix3x4_t& outMat,
			bool& outPhoneOpen,
			HandSkinPreset* outPreset,
			unsigned long long nowMs,
			unsigned long long maxAgeMs) const
		{
			for (int attempt = 0; attempt < 8; ++attempt)
			{
				uint32_t s0 = seq.load(std::memory_order_acquire);
				if (s0 == 0u)
					return false;
				if ((s0 & 1u) != 0u)
					continue;
				std::atomic_thread_fence(std::memory_order_acquire);
				const bool isActive = active.load(std::memory_order_relaxed);
				matrix3x4_t tmp{};
				for (int r = 0; r < 3; ++r)
				{
					for (int c = 0; c < 4; ++c)
						tmp.m_matrix[r][c] = m[r][c].load(std::memory_order_relaxed);
				}
				unsigned long long t = tickMs.load(std::memory_order_relaxed);
				bool phoneOpen = usePhoneOpenTable.load(std::memory_order_relaxed);
				uint8_t presetVal = skinPreset.load(std::memory_order_relaxed);
				std::atomic_thread_fence(std::memory_order_seq_cst);
				uint32_t s1 = seq.load(std::memory_order_acquire);
				if (s0 == s1)
				{
					if (!isActive)
						return false;
					if (maxAgeMs == 0ULL || (nowMs >= t && (nowMs - t) <= maxAgeMs))
					{
						outMat = tmp;
						outPhoneOpen = phoneOpen;
						if (outPreset)
							*outPreset = static_cast<HandSkinPreset>(presetVal);
						return true;
					}
					return false;
				}
			}
			return false;
		}
	};

	SeqlockMatrixSlot s_ModelToWristCache[2];

	// Lock-free seqlock storage for resolved visual wrist poses [0 = Physical Right, 1 = Physical Left].
	struct SeqlockVisualWristSlot
	{
		std::atomic<uint32_t> seq{ 0u };
		std::atomic<bool> active{ false };
		std::atomic<float> wrist[3][4]{};
		std::atomic<float> delta[3][4]{};
		std::atomic<float> rawM2W[3][4]{};
		std::atomic<unsigned long long> tickMs{ 0ULL };

		void Write(
			const matrix3x4_t& visualWrist,
			const matrix3x4_t& worldDelta,
			const matrix3x4_t& rawModelToWorld,
			unsigned long long nowMs)
		{
			uint32_t s = seq.load(std::memory_order_relaxed);
			seq.store(s + 1u, std::memory_order_seq_cst);
			std::atomic_thread_fence(std::memory_order_seq_cst);
			for (int r = 0; r < 3; ++r)
			{
				for (int c = 0; c < 4; ++c)
				{
					wrist[r][c].store(visualWrist.m_matrix[r][c], std::memory_order_relaxed);
					delta[r][c].store(worldDelta.m_matrix[r][c], std::memory_order_relaxed);
					rawM2W[r][c].store(rawModelToWorld.m_matrix[r][c], std::memory_order_relaxed);
				}
			}
			tickMs.store(nowMs, std::memory_order_relaxed);
			active.store(true, std::memory_order_relaxed);
			std::atomic_thread_fence(std::memory_order_release);
			seq.store(s + 2u, std::memory_order_release);
		}

		void Clear()
		{
			const uint32_t s = seq.load(std::memory_order_relaxed);
			seq.store(s + 1u, std::memory_order_seq_cst);
			std::atomic_thread_fence(std::memory_order_seq_cst);
			active.store(false, std::memory_order_relaxed);
			tickMs.store(0ULL, std::memory_order_relaxed);
			std::atomic_thread_fence(std::memory_order_release);
			seq.store(s + 2u, std::memory_order_release);
		}

		bool Read(
			matrix3x4_t& outVisualWrist,
			matrix3x4_t* outWorldDelta,
			matrix3x4_t* outRawM2W,
			unsigned long long nowMs,
			unsigned long long maxAgeMs) const
		{
			for (int attempt = 0; attempt < 8; ++attempt)
			{
				const uint32_t s0 = seq.load(std::memory_order_acquire);
				if (s0 == 0u)
					return false;
				if ((s0 & 1u) != 0u)
					continue;
				std::atomic_thread_fence(std::memory_order_acquire);
				const bool isActive = active.load(std::memory_order_relaxed);

				matrix3x4_t tmpW{}, tmpD{}, tmpR{};
				for (int r = 0; r < 3; ++r)
				{
					for (int c = 0; c < 4; ++c)
					{
						tmpW.m_matrix[r][c] = wrist[r][c].load(std::memory_order_relaxed);
						if (outWorldDelta)
							tmpD.m_matrix[r][c] = delta[r][c].load(std::memory_order_relaxed);
						if (outRawM2W)
							tmpR.m_matrix[r][c] = rawM2W[r][c].load(std::memory_order_relaxed);
					}
				}
				const unsigned long long t = tickMs.load(std::memory_order_relaxed);
				std::atomic_thread_fence(std::memory_order_seq_cst);
				const uint32_t s1 = seq.load(std::memory_order_acquire);
				if (s0 == s1)
				{
					if (!isActive)
						return false;
					if (nowMs >= t && (nowMs - t) <= maxAgeMs)
					{
						outVisualWrist = tmpW;
						if (outWorldDelta)
							*outWorldDelta = tmpD;
						if (outRawM2W)
							*outRawM2W = tmpR;
						return true;
					}
					return false;
				}
			}
			return false;
		}
	};

	SeqlockVisualWristSlot s_VisualWristSlots[2];

	struct PhoneScreenQuad
	{
		Vector ul{ 0.0f, 0.0f, 0.0f };
		Vector rightVec{ 0.0f, 0.0f, 0.0f };
		Vector downVec{ 0.0f, 0.0f, 0.0f };
		Vector normal{ 0.0f, 0.0f, 1.0f };
		float widthLenSq = 1.0f;
		float heightLenSq = 1.0f;
	};

	struct SeqlockPhoneScreenSlot
	{
		std::atomic<uint32_t> seq{ 0u };
		std::atomic<bool> active{ false };
		std::atomic<float> ul[3]{ { 0.0f }, { 0.0f }, { 0.0f } };
		std::atomic<float> rightVec[3]{ { 0.0f }, { 0.0f }, { 0.0f } };
		std::atomic<float> downVec[3]{ { 0.0f }, { 0.0f }, { 0.0f } };
		std::atomic<float> normal[3]{ { 0.0f }, { 0.0f }, { 1.0f } };
		std::atomic<float> widthLenSq{ 1.0f };
		std::atomic<float> heightLenSq{ 1.0f };
		std::atomic<unsigned long long> tickMs{ 0ULL };

		void Write(const PhoneScreenQuad& q, unsigned long long nowMs)
		{
			const uint32_t s = seq.load(std::memory_order_relaxed);
			seq.store(s + 1u, std::memory_order_seq_cst);
			std::atomic_thread_fence(std::memory_order_seq_cst);
			ul[0].store(q.ul.x, std::memory_order_relaxed);
			ul[1].store(q.ul.y, std::memory_order_relaxed);
			ul[2].store(q.ul.z, std::memory_order_relaxed);
			rightVec[0].store(q.rightVec.x, std::memory_order_relaxed);
			rightVec[1].store(q.rightVec.y, std::memory_order_relaxed);
			rightVec[2].store(q.rightVec.z, std::memory_order_relaxed);
			downVec[0].store(q.downVec.x, std::memory_order_relaxed);
			downVec[1].store(q.downVec.y, std::memory_order_relaxed);
			downVec[2].store(q.downVec.z, std::memory_order_relaxed);
			normal[0].store(q.normal.x, std::memory_order_relaxed);
			normal[1].store(q.normal.y, std::memory_order_relaxed);
			normal[2].store(q.normal.z, std::memory_order_relaxed);
			widthLenSq.store(q.widthLenSq, std::memory_order_relaxed);
			heightLenSq.store(q.heightLenSq, std::memory_order_relaxed);
			tickMs.store(nowMs, std::memory_order_relaxed);
			active.store(true, std::memory_order_relaxed);
			std::atomic_thread_fence(std::memory_order_release);
			seq.store(s + 2u, std::memory_order_release);
		}

		void Clear()
		{
			const uint32_t s = seq.load(std::memory_order_relaxed);
			seq.store(s + 1u, std::memory_order_seq_cst);
			std::atomic_thread_fence(std::memory_order_seq_cst);
			active.store(false, std::memory_order_relaxed);
			tickMs.store(0ULL, std::memory_order_relaxed);
			std::atomic_thread_fence(std::memory_order_release);
			seq.store(s + 2u, std::memory_order_release);
		}

		bool Read(PhoneScreenQuad& outQ, unsigned long long nowMs, unsigned long long maxAgeMs) const
		{
			for (int attempt = 0; attempt < 8; ++attempt)
			{
				const uint32_t s0 = seq.load(std::memory_order_acquire);
				if (s0 == 0u)
					return false;
				if ((s0 & 1u) != 0u)
					continue;
				std::atomic_thread_fence(std::memory_order_acquire);
				const bool isActive = active.load(std::memory_order_relaxed);
				PhoneScreenQuad tmp{};
				tmp.ul = Vector(ul[0].load(std::memory_order_relaxed), ul[1].load(std::memory_order_relaxed), ul[2].load(std::memory_order_relaxed));
				tmp.rightVec = Vector(rightVec[0].load(std::memory_order_relaxed), rightVec[1].load(std::memory_order_relaxed), rightVec[2].load(std::memory_order_relaxed));
				tmp.downVec = Vector(downVec[0].load(std::memory_order_relaxed), downVec[1].load(std::memory_order_relaxed), downVec[2].load(std::memory_order_relaxed));
				tmp.normal = Vector(normal[0].load(std::memory_order_relaxed), normal[1].load(std::memory_order_relaxed), normal[2].load(std::memory_order_relaxed));
				tmp.widthLenSq = widthLenSq.load(std::memory_order_relaxed);
				tmp.heightLenSq = heightLenSq.load(std::memory_order_relaxed);
				const unsigned long long t = tickMs.load(std::memory_order_relaxed);
				std::atomic_thread_fence(std::memory_order_seq_cst);
				const uint32_t s1 = seq.load(std::memory_order_acquire);
				if (s0 == s1)
				{
					if (isActive && tmp.widthLenSq > 1e-4f && tmp.heightLenSq > 1e-4f &&
					    nowMs >= t && (nowMs - t) <= maxAgeMs)
					{
						outQ = tmp;
						return true;
					}
					return false;
				}
			}
			return false;
		}
	};

	SeqlockPhoneScreenSlot s_PhoneScreenSlot;

	struct PhoneTouchState
	{
		bool armed = true;
		bool pendingRelease = false;
		float lockedU = 0.5f;
		float lockedV = 0.5f;
		unsigned long long pressStartMs = 0ULL;
		unsigned long long lastTapMs = 0ULL;
		bool hasClearSince = false;
		std::chrono::steady_clock::time_point clearSince{};
		bool hasPrevTip = false;
		Vector prevTipPos{ 0.0f, 0.0f, 0.0f };
		std::chrono::steady_clock::time_point prevTipTime{};
	};

	std::mutex s_PhoneTouchMutex;
	PhoneTouchState s_PhoneTouch;

	// Lock-free seqlock holding the server-side physics entity state of the currently grabbed prop.
	struct SeqlockHeldPhysicsSlot
	{
		std::atomic<uint32_t> seq{ 0u };
		std::atomic<bool> active{ false };
		std::atomic<int> heldEntIndex{ -1 };
		std::atomic<bool> holdingLeftHand{ false };
		std::atomic<float> actualEntPos[3]{ { 0.0f }, { 0.0f }, { 0.0f } };
		std::atomic<float> actualEntAng[3]{ { 0.0f }, { 0.0f }, { 0.0f } };
		std::atomic<float> targetEntPos[3]{ { 0.0f }, { 0.0f }, { 0.0f } };
		std::atomic<float> targetEntAng[3]{ { 0.0f }, { 0.0f }, { 0.0f } };
		std::atomic<float> targetWristToWorld[3][4]{
			{ { 0.0f }, { 0.0f }, { 0.0f }, { 0.0f } },
			{ { 0.0f }, { 0.0f }, { 0.0f }, { 0.0f } },
			{ { 0.0f }, { 0.0f }, { 0.0f }, { 0.0f } }
		};
		std::atomic<float> objRadius{ 0.0f };
		std::atomic<bool> isWallBlocked{ false };
		std::atomic<unsigned long long> tickMs{ 0ULL };

		void Publish(const VRHands::HeldHandPhysicsFeedback& fb, unsigned long long nowMs)
		{
			const uint32_t s = seq.load(std::memory_order_relaxed);
			seq.store(s + 1u, std::memory_order_seq_cst);
			std::atomic_thread_fence(std::memory_order_seq_cst);
			heldEntIndex.store(fb.heldEntIndex, std::memory_order_relaxed);
			holdingLeftHand.store(fb.holdingLeftHand, std::memory_order_relaxed);
			actualEntPos[0].store(fb.actualEntPos.x, std::memory_order_relaxed);
			actualEntPos[1].store(fb.actualEntPos.y, std::memory_order_relaxed);
			actualEntPos[2].store(fb.actualEntPos.z, std::memory_order_relaxed);
			actualEntAng[0].store(fb.actualEntAng.x, std::memory_order_relaxed);
			actualEntAng[1].store(fb.actualEntAng.y, std::memory_order_relaxed);
			actualEntAng[2].store(fb.actualEntAng.z, std::memory_order_relaxed);
			targetEntPos[0].store(fb.targetEntPos.x, std::memory_order_relaxed);
			targetEntPos[1].store(fb.targetEntPos.y, std::memory_order_relaxed);
			targetEntPos[2].store(fb.targetEntPos.z, std::memory_order_relaxed);
			targetEntAng[0].store(fb.targetEntAng.x, std::memory_order_relaxed);
			targetEntAng[1].store(fb.targetEntAng.y, std::memory_order_relaxed);
			targetEntAng[2].store(fb.targetEntAng.z, std::memory_order_relaxed);
			for (int r = 0; r < 3; ++r)
			{
				for (int c = 0; c < 4; ++c)
					targetWristToWorld[r][c].store(fb.targetWristToWorld.m_matrix[r][c], std::memory_order_relaxed);
			}
			objRadius.store(fb.objRadius, std::memory_order_relaxed);
			isWallBlocked.store(fb.isWallBlocked, std::memory_order_relaxed);
			tickMs.store(nowMs, std::memory_order_relaxed);
			active.store(true, std::memory_order_relaxed);
			std::atomic_thread_fence(std::memory_order_release);
			seq.store(s + 2u, std::memory_order_release);
		}

		void Clear()
		{
			const uint32_t s = seq.load(std::memory_order_relaxed);
			seq.store(s + 1u, std::memory_order_seq_cst);
			std::atomic_thread_fence(std::memory_order_seq_cst);
			active.store(false, std::memory_order_relaxed);
			heldEntIndex.store(-1, std::memory_order_relaxed);
			tickMs.store(0ULL, std::memory_order_relaxed);
			std::atomic_thread_fence(std::memory_order_release);
			seq.store(s + 2u, std::memory_order_release);
		}

		bool Read(VRHands::HeldHandPhysicsFeedback& outFb, unsigned long long nowMs, unsigned long long maxAgeMs) const
		{
			for (int attempt = 0; attempt < 8; ++attempt)
			{
				const uint32_t s0 = seq.load(std::memory_order_acquire);
				if (s0 == 0u)
					return false;
				if ((s0 & 1u) != 0u)
					continue;
				std::atomic_thread_fence(std::memory_order_acquire);
				const bool isActive = active.load(std::memory_order_relaxed);
				VRHands::HeldHandPhysicsFeedback tmp{};
				tmp.heldEntIndex = heldEntIndex.load(std::memory_order_relaxed);
				tmp.holdingLeftHand = holdingLeftHand.load(std::memory_order_relaxed);
				tmp.actualEntPos = Vector(
					actualEntPos[0].load(std::memory_order_relaxed),
					actualEntPos[1].load(std::memory_order_relaxed),
					actualEntPos[2].load(std::memory_order_relaxed));
				tmp.actualEntAng = QAngle(
					actualEntAng[0].load(std::memory_order_relaxed),
					actualEntAng[1].load(std::memory_order_relaxed),
					actualEntAng[2].load(std::memory_order_relaxed));
				tmp.targetEntPos = Vector(
					targetEntPos[0].load(std::memory_order_relaxed),
					targetEntPos[1].load(std::memory_order_relaxed),
					targetEntPos[2].load(std::memory_order_relaxed));
				tmp.targetEntAng = QAngle(
					targetEntAng[0].load(std::memory_order_relaxed),
					targetEntAng[1].load(std::memory_order_relaxed),
					targetEntAng[2].load(std::memory_order_relaxed));
				for (int r = 0; r < 3; ++r)
				{
					for (int c = 0; c < 4; ++c)
						tmp.targetWristToWorld.m_matrix[r][c] = targetWristToWorld[r][c].load(std::memory_order_relaxed);
				}
				tmp.objRadius = objRadius.load(std::memory_order_relaxed);
				tmp.isWallBlocked = isWallBlocked.load(std::memory_order_relaxed);
				const unsigned long long t = tickMs.load(std::memory_order_relaxed);
				std::atomic_thread_fence(std::memory_order_seq_cst);
				const uint32_t s1 = seq.load(std::memory_order_acquire);
				if (s0 == s1)
				{
					if (isActive && nowMs >= t && (nowMs - t) <= maxAgeMs)
					{
						outFb = tmp;
						return true;
					}
					return false;
				}
			}
			return false;
		}
	};

	SeqlockHeldPhysicsSlot s_HeldPhysicsSlot;

	// Per-joint surface conformation pose for open VR hands [0 = physical Right, 1 = physical Left].
	struct HandSurfaceFingerPose
	{
		float blendAlpha = 0.0f;
		float thumbCurlDeg[3]{ 0.0f, 0.0f, 0.0f };
		float thumbSplayDeg[2]{ 0.0f, 0.0f }; // [0] = CMC, [1] = MCP
		float fingerCurlDeg[4][4]{};          // [Index..Pinky][Meta, MCP, PIP, DIP]
		float fingerSplayDeg[4]{ 0.0f, 0.0f, 0.0f, 0.0f };
	};

	struct SeqlockSurfaceFingerSlot
	{
		std::atomic<uint32_t> seq{ 0u };
		std::atomic<bool> active{ false };
		std::atomic<float> blendAlpha{ 0.0f };
		std::atomic<float> thumbCurlDeg[3]{ { 0.0f }, { 0.0f }, { 0.0f } };
		std::atomic<float> thumbSplayDeg[2]{ { 0.0f }, { 0.0f } };
		std::atomic<float> fingerCurlDeg[4][4]{};
		std::atomic<float> fingerSplayDeg[4]{ { 0.0f }, { 0.0f }, { 0.0f }, { 0.0f } };
		std::atomic<unsigned long long> tickMs{ 0ULL };

		void Write(const HandSurfaceFingerPose& pose, unsigned long long nowMs)
		{
			const uint32_t s = seq.load(std::memory_order_relaxed);
			seq.store(s + 1u, std::memory_order_seq_cst);
			std::atomic_thread_fence(std::memory_order_seq_cst);
			blendAlpha.store(pose.blendAlpha, std::memory_order_relaxed);
			for (int k = 0; k < 3; ++k)
				thumbCurlDeg[k].store(pose.thumbCurlDeg[k], std::memory_order_relaxed);
			for (int k = 0; k < 2; ++k)
				thumbSplayDeg[k].store(pose.thumbSplayDeg[k], std::memory_order_relaxed);
			for (int f = 0; f < 4; ++f)
			{
				for (int k = 0; k < 4; ++k)
					fingerCurlDeg[f][k].store(pose.fingerCurlDeg[f][k], std::memory_order_relaxed);
				fingerSplayDeg[f].store(pose.fingerSplayDeg[f], std::memory_order_relaxed);
			}
			tickMs.store(nowMs, std::memory_order_relaxed);
			active.store(true, std::memory_order_relaxed);
			std::atomic_thread_fence(std::memory_order_release);
			seq.store(s + 2u, std::memory_order_release);
		}

		void Clear()
		{
			const uint32_t s = seq.load(std::memory_order_relaxed);
			seq.store(s + 1u, std::memory_order_seq_cst);
			std::atomic_thread_fence(std::memory_order_seq_cst);
			active.store(false, std::memory_order_relaxed);
			blendAlpha.store(0.0f, std::memory_order_relaxed);
			tickMs.store(0ULL, std::memory_order_relaxed);
			std::atomic_thread_fence(std::memory_order_release);
			seq.store(s + 2u, std::memory_order_release);
		}

		bool Read(HandSurfaceFingerPose& outPose, unsigned long long nowMs, unsigned long long maxAgeMs) const
		{
			for (int attempt = 0; attempt < 8; ++attempt)
			{
				const uint32_t s0 = seq.load(std::memory_order_acquire);
				if (s0 == 0u)
					return false;
				if ((s0 & 1u) != 0u)
					continue;
				std::atomic_thread_fence(std::memory_order_acquire);
				const bool isActive = active.load(std::memory_order_relaxed);
				HandSurfaceFingerPose tmp{};
				tmp.blendAlpha = blendAlpha.load(std::memory_order_relaxed);
				for (int k = 0; k < 3; ++k)
					tmp.thumbCurlDeg[k] = thumbCurlDeg[k].load(std::memory_order_relaxed);
				for (int k = 0; k < 2; ++k)
					tmp.thumbSplayDeg[k] = thumbSplayDeg[k].load(std::memory_order_relaxed);
				for (int f = 0; f < 4; ++f)
				{
					for (int k = 0; k < 4; ++k)
						tmp.fingerCurlDeg[f][k] = fingerCurlDeg[f][k].load(std::memory_order_relaxed);
					tmp.fingerSplayDeg[f] = fingerSplayDeg[f].load(std::memory_order_relaxed);
				}
				const unsigned long long t = tickMs.load(std::memory_order_relaxed);
				std::atomic_thread_fence(std::memory_order_seq_cst);
				const uint32_t s1 = seq.load(std::memory_order_acquire);
				if (s0 == s1)
				{
					if (!isActive)
						return false;
					if (nowMs >= t && (nowMs - t) <= maxAgeMs)
					{
						outPose = tmp;
						return true;
					}
					return false;
				}
			}
			return false;
		}
	};

	SeqlockSurfaceFingerSlot s_SurfaceFingerSlots[2];

	// Per-hand client-main-thread temporal smoothing state for visual hand physics.
	struct VisualHandSmoothingState
	{
		bool initialized = false;
		unsigned long long lastTickMs = 0ULL;
		std::chrono::steady_clock::time_point lastSteadyTime{};
		bool hasSteadyTime = false;
		bool hasPrevVisualPalm = false;
		Vector prevVisualPalm{ 0.0f, 0.0f, 0.0f };
		Vector prevRawPalm{ 0.0f, 0.0f, 0.0f };
		Vector smoothedOffset{ 0.0f, 0.0f, 0.0f };
		float smoothedCouplingAlpha = 0.0f;
	};

	VisualHandSmoothingState s_VisualSmooth[2];

	struct VisualFingerSmoothState
	{
		bool initialized = false;
		HandSurfaceFingerPose pose{};
		float smoothedSpreadAlpha = 0.0f;
	};

	VisualFingerSmoothState s_FingerSmooth[2];

	struct VisualHandTouchHapticState
	{
		bool palmArmed = true;
		bool fingerArmed = true;
		float palmClearSec = 0.0f;
		float fingerClearSec = 0.0f;
		unsigned long long lastImpulseMs = 0ULL;
		unsigned long long suppressUntilMs = 0ULL;
	};

	VisualHandTouchHapticState s_TouchHaptic[2];

	struct FlashlightGripAnimState
	{
		bool initialized = false;
		bool snapPending = false;
		bool hasTime = false;
		float blend = 0.0f; // 0.0 = standard/underhand, 1.0 = overhand (180 deg)
		int lastEyePass = 0;
		std::chrono::steady_clock::time_point lastTime{};
	};

	std::mutex s_FlashlightGripAnimMutex;
	FlashlightGripAnimState s_FlashlightGripAnim;

	float EvaluateFlashlightGripBlend(VR* vr)
	{
		const float target = (vr && vr->m_FlashlightOverhandGrip) ? 1.0f : 0.0f;
		std::lock_guard<std::mutex> lock(s_FlashlightGripAnimMutex);
		const auto now = std::chrono::steady_clock::now();
		const int eyePass = vr ? vr->m_ActiveEyePass : 0;

		if (!s_FlashlightGripAnim.initialized || s_FlashlightGripAnim.snapPending)
		{
			s_FlashlightGripAnim.blend = target;
			s_FlashlightGripAnim.initialized = true;
			s_FlashlightGripAnim.snapPending = false;
			s_FlashlightGripAnim.hasTime = true;
			s_FlashlightGripAnim.lastTime = now;
			s_FlashlightGripAnim.lastEyePass = eyePass;
		}
		else if (eyePass == 2 && s_FlashlightGripAnim.lastEyePass == 1)
		{
			// Keep Eye 2 identical to Eye 1 on the same VR frame (zero stereo mismatch).
			s_FlashlightGripAnim.lastEyePass = 2;
		}
		else
		{
			const float dt = s_FlashlightGripAnim.hasTime
				? ClampF(std::chrono::duration<float>(now - s_FlashlightGripAnim.lastTime).count(), 0.0f, 0.1f)
				: 0.0f;
			s_FlashlightGripAnim.lastTime = now;
			s_FlashlightGripAnim.hasTime = true;
			s_FlashlightGripAnim.lastEyePass = eyePass;

			constexpr float kFlipDurationSec = 0.130f;
			const float maxStep = dt / kFlipDurationSec;
			const float diff = target - s_FlashlightGripAnim.blend;
			if (std::fabs(diff) <= maxStep)
				s_FlashlightGripAnim.blend = target;
			else
				s_FlashlightGripAnim.blend += (diff > 0.0f) ? maxStep : -maxStep;
		}

		const float t = ClampF(s_FlashlightGripAnim.blend, 0.0f, 1.0f);
		return t * t * (3.0f - 2.0f * t);
	}

	void SyncFlashlightGripWhenBare(VR* vr)
	{
		const float target = (vr && vr->m_FlashlightOverhandGrip) ? 1.0f : 0.0f;
		std::lock_guard<std::mutex> lock(s_FlashlightGripAnimMutex);
		s_FlashlightGripAnim.blend = target;
		s_FlashlightGripAnim.initialized = true;
		s_FlashlightGripAnim.snapPending = false;
		s_FlashlightGripAnim.hasTime = false;
		s_FlashlightGripAnim.lastEyePass = 0;
	}

	// Lock-free per-controller finger curl clamp state [0 = physical Right, 1 = physical Left].
	struct FingerCurlLimitSlot
	{
		std::atomic<bool> active{ false };
		std::atomic<float> maxCurl[5]{
			{ 1.0f }, { 1.0f }, { 1.0f }, { 1.0f }, { 1.0f }
		};
	};

	FingerCurlLimitSlot s_CurlLimitSlots[2];

	// Calibrated orthonormal model-space Forearm & Wrist (Hand) poses for bare VR hands:
	// - s_CamRightForearm / s_CamRightHand: v_camera.mdl, v_light.mdl, v_superlight.mdl on vm0
	// - s_PhoneRightForearm / s_CamRightHand: v_phone.mdl on vm0 (zero-twist parent relative to v_phone bind)
	// - s_LightLeftForearm / s_LightLeftHand: v_light.mdl, v_superlight.mdl on vm1
	static const matrix3x4_t s_CamRightForearm(
		 0.415297f,  0.830889f,  0.370340f,  13.040969f,
		 0.382359f, -0.528837f,  0.757716f, -13.045389f,
		 0.825428f, -0.173074f, -0.537322f, -15.618577f
	);

	static const matrix3x4_t s_PhoneRightForearm(
		 0.390161f,  0.919338f, -0.050911f,  13.348797f,
		 0.332921f, -0.089306f,  0.938716f, -12.439932f,
		 0.858451f, -0.383200f, -0.340911f, -16.023053f
	);

	static const matrix3x4_t s_CamRightHand(
		 0.390161f,  0.110232f, -0.914124f,  18.127191f,
		 0.332921f,  0.908747f,  0.251679f,  -8.362570f,
		 0.858451f, -0.402527f,  0.317859f,  -5.509399f
	);

	static const matrix3x4_t s_LightLeftForearm(
		 0.130612f,  0.984364f, -0.118186f,  14.806366f,
		-0.128550f,  0.135015f,  0.982469f,   8.091601f,
		 0.983064f, -0.113129f,  0.144175f, -16.022509f
	);

	static const matrix3x4_t s_PhoneLeftForearm(
		-0.073206f,  0.993631f, -0.085662f,  17.326474f,
		-0.259804f,  0.063926f,  0.963543f,   9.714496f,
		 0.962882f,  0.092792f,  0.253470f, -15.773001f
	);

	static const matrix3x4_t s_LightLeftHand(
		-0.073206f,  0.257670f,  0.963456f,  16.421315f,
		-0.259804f, -0.937620f,  0.231019f,   6.502138f,
		 0.962882f, -0.233398f,  0.135583f,  -3.867408f
	);

	// Arch-preserving open-rest local transforms for v_camera.mdl, v_light.mdl, and v_superlight.mdl:
	// Keeps exact per-bone local translations from the MDL hierarchy, blends palm metacarpals (Finger0..4)
	// with the model's bind arch to eliminate opposing knuckle S-kinks, and aligns phalanges to a natural
	// extended posture without >180-degree antipodal wrap.
	static constexpr float s_OpenCamLeftThumb[3][3][4] = {
		{ // Bip001 L Finger0
			{   0.771190f,   0.554765f,   0.312253f,   0.795398f },
			{   0.240286f,   0.200551f,  -0.949759f,  -0.000595f },
			{  -0.589516f,   0.807474f,   0.021361f,  -0.745692f },
		},
		{ // Bip001 L Finger01 (untwisted open orientation matching v_phone L Finger01)
			{   0.981750f,  -0.188060f,   0.028278f,   2.960720f },
			{   0.181653f,   0.971347f,   0.153252f,   0.000003f },
			{  -0.056288f,  -0.145318f,   0.987782f,  -0.000003f },
		},
		{ // Bip001 L Finger02
			{   0.990548f,  -0.136670f,  -0.011661f,   1.025919f },
			{   0.136954f,   0.990162f,   0.028689f,   0.000001f },
			{   0.007626f,  -0.030015f,   0.999520f,  -0.000003f },
		},
	};

	static constexpr float s_OpenCamLeftFingers[4][4][3][4] = {
		{
			{ // Bip001 L Finger1
				{  0.944078f,  0.322925f,  0.066601f,  0.795393f },
				{ -0.325786f,  0.944697f,  0.037563f, -0.000293f },
				{ -0.050788f, -0.057160f,  0.997072f, -0.372848f },
			},
			{ // Bip001 L Finger11
				{  0.995970f, -0.087693f, -0.018786f,  3.850655f },
				{  0.088379f,  0.995303f,  0.039500f,  0.000000f },
				{  0.015234f, -0.041001f,  0.999043f,  0.000001f },
			},
			{ // Bip001 L Finger12
				{  0.999042f, -0.042493f,  0.010498f,  1.364190f },
				{  0.042498f,  0.999097f, -0.000285f, -0.000002f },
				{ -0.010477f,  0.000731f,  0.999945f, -0.000001f },
			},
			{ // Bip001 L Finger13
				{  0.999349f, -0.035833f,  0.004080f,  0.876814f },
				{  0.035854f,  0.999342f, -0.005394f, -0.000002f },
				{ -0.003884f,  0.005537f,  0.999977f,  0.000001f },
			},
		},
		{
			{ // Bip001 L Finger2
				{  0.956580f,  0.290517f, -0.023534f,  0.795401f },
				{ -0.276815f,  0.930802f,  0.238707f, -0.000001f },
				{  0.091254f, -0.221828f,  0.970806f,  0.000002f },
			},
			{ // Bip001 L Finger21
				{  0.994638f, -0.102120f, -0.016313f,  3.807300f },
				{  0.101256f,  0.993747f, -0.047062f,  0.000002f },
				{  0.021017f,  0.045158f,  0.998759f,  0.000000f },
			},
			{ // Bip001 L Finger22
				{  0.996205f, -0.022386f,  0.084112f,  1.606142f },
				{  0.020980f,  0.999626f,  0.017558f, -0.000008f },
				{ -0.084473f, -0.015727f,  0.996302f, -0.000003f },
			},
			{ // Bip001 L Finger23
				{  0.999346f, -0.036171f,  0.000123f,  1.073047f },
				{  0.036171f,  0.999324f, -0.006526f,  0.000003f },
				{  0.000113f,  0.006526f,  0.999979f,  0.000001f },
			},
		},
		{
			{ // Bip001 L Finger3
				{  0.962184f,  0.224879f, -0.153724f,  0.795402f },
				{ -0.193110f,  0.961134f,  0.197308f,  0.000299f },
				{  0.192120f, -0.160161f,  0.968214f,  0.372846f },
			},
			{ // Bip001 L Finger31
				{  0.997088f, -0.070258f, -0.029647f,  3.722622f },
				{  0.073829f,  0.986726f,  0.144640f,  0.000003f },
				{  0.019092f, -0.146408f,  0.989040f,  0.000000f },
			},
			{ // Bip001 L Finger32
				{  0.999061f, -0.043292f,  0.001634f,  1.460087f },
				{  0.043298f,  0.999055f, -0.003754f,  0.000005f },
				{ -0.001470f,  0.003821f,  0.999992f,  0.000002f },
			},
			{ // Bip001 L Finger33
				{  0.999354f, -0.031641f,  0.017065f,  0.986087f },
				{  0.032596f,  0.997728f, -0.058962f, -0.000010f },
				{ -0.015161f,  0.059480f,  0.998114f, -0.000001f },
			},
		},
		{
			{ // Bip001 L Finger4
				{  0.973849f,  0.181197f, -0.137063f,  0.795405f },
				{ -0.021567f,  0.674281f,  0.738160f,  0.000595f },
				{  0.226171f, -0.715900f,  0.660556f,  0.745696f },
			},
			{ // Bip001 L Finger41
				{  0.965271f,  0.090325f, -0.245139f,  3.316574f },
				{ -0.145973f,  0.964666f, -0.219344f,  0.000003f },
				{  0.216665f,  0.247510f,  0.944349f, -0.000001f },
			},
			{ // Bip001 L Finger42
				{  0.999228f, -0.039108f,  0.003640f,  1.075447f },
				{  0.039107f,  0.999235f,  0.000334f, -0.000003f },
				{ -0.003650f, -0.000191f,  0.999993f,  0.000000f },
			},
			{ // Bip001 L Finger43
				{  0.999206f, -0.033918f,  0.020906f,  0.717947f },
				{  0.033590f,  0.999310f,  0.015824f,  0.000008f },
				{ -0.021428f, -0.015109f,  0.999656f, -0.000003f },
			},
		},
	};

	static constexpr float s_OpenCamRightThumb[3][3][4] = {
		{ // Bip001 R Finger0
			{   0.745354f,   0.597800f,  -0.295098f,   0.795405f },
			{   0.291232f,   0.106205f,   0.950739f,  -0.000600f },
			{   0.599692f,  -0.794579f,  -0.094938f,   0.745694f },
		},
		{ // Bip001 R Finger01
			{   0.982190f,  -0.187546f,  -0.011356f,   2.651910f },
			{   0.187424f,   0.982217f,  -0.011060f,  -0.000001f },
			{   0.013229f,   0.008735f,   0.999874f,   0.000002f },
		},
		{ // Bip001 R Finger02
			{   0.995246f,  -0.097381f,   0.001145f,   1.025906f },
			{   0.097388f,   0.995178f,  -0.011654f,   0.000002f },
			{  -0.000005f,   0.011710f,   0.999931f,   0.000002f },
		},
	};

	static constexpr float s_OpenCamRightFingers[4][4][3][4] = {
		{
			{ // Bip001 R Finger1
				{  0.956200f,  0.263901f, -0.126640f,  0.795398f },
				{ -0.248464f,  0.960480f,  0.125478f, -0.000298f },
				{  0.154749f, -0.088516f,  0.983980f,  0.372849f },
			},
			{ // Bip001 R Finger11
				{  0.991937f, -0.088168f,  0.091038f,  3.850672f },
				{  0.099140f,  0.987314f, -0.124024f,  0.000001f },
				{ -0.078948f,  0.132049f,  0.988094f, -0.000001f },
			},
			{ // Bip001 R Finger12
				{  0.999051f, -0.040572f, -0.015853f,  1.193111f },
				{  0.040437f,  0.999144f, -0.008703f, -0.000004f },
				{  0.016193f,  0.008054f,  0.999836f,  0.000001f },
			},
			{ // Bip001 R Finger13
				{  0.999309f, -0.037169f,  0.000612f,  0.876812f },
				{  0.037172f,  0.999285f, -0.006938f, -0.000007f },
				{ -0.000354f,  0.006956f,  0.999976f,  0.000001f },
			},
		},
		{
			{ // Bip001 R Finger2
				{  0.976248f,  0.215183f, -0.025221f,  0.795395f },
				{ -0.216598f,  0.966650f, -0.136649f,  0.000005f },
				{ -0.005024f,  0.138866f,  0.990298f, -0.000001f },
			},
			{ // Bip001 R Finger21
				{  0.992236f, -0.095341f,  0.079865f,  3.807308f },
				{  0.099137f,  0.994056f, -0.044991f,  0.000001f },
				{ -0.075101f,  0.052559f,  0.995790f,  0.000002f },
			},
			{ // Bip001 R Finger22
				{  0.998226f, -0.035656f, -0.047682f,  1.391960f },
				{  0.035244f,  0.999334f, -0.009452f,  0.000013f },
				{  0.047988f,  0.007755f,  0.998818f,  0.000001f },
			},
			{ // Bip001 R Finger23
				{  0.999338f, -0.036127f,  0.004225f,  0.726546f },
				{  0.036137f,  0.999344f, -0.002252f, -0.000002f },
				{ -0.004140f,  0.002403f,  0.999989f,  0.000001f },
			},
		},
		{
			{ // Bip001 R Finger3
				{  0.986092f,  0.157142f,  0.054117f,  0.795399f },
				{ -0.149363f,  0.980706f, -0.126117f,  0.000300f },
				{ -0.072891f,  0.116280f,  0.990538f, -0.372844f },
			},
			{ // Bip001 R Finger31
				{  0.986943f, -0.052037f,  0.152431f,  3.550786f },
				{  0.079036f,  0.981065f, -0.176816f, -0.000001f },
				{ -0.140344f,  0.186555f,  0.972369f,  0.000000f },
			},
			{ // Bip001 R Finger32
				{  0.999095f, -0.042537f,  0.000000f,  1.460092f },
				{  0.042537f,  0.999095f,  0.000000f, -0.000001f },
				{  0.000000f,  0.000000f,  1.000000f,  0.000004f },
			},
			{ // Bip001 R Finger33
				{  0.999420f, -0.034031f, -0.001018f,  0.986076f },
				{  0.034037f,  0.998012f,  0.053041f,  0.000001f },
				{ -0.000789f, -0.053045f,  0.998592f, -0.000001f },
			},
		},
		{
			{ // Bip001 R Finger4
				{  0.987628f,  0.034860f,  0.152893f,  0.795403f },
				{  0.004850f,  0.967722f, -0.251972f,  0.000596f },
				{ -0.156741f,  0.249596f,  0.955580f, -0.745691f },
			},
			{ // Bip001 R Finger41
				{  0.964973f,  0.074803f,  0.251461f,  3.369550f },
				{ -0.031641f,  0.984677f, -0.171492f,  0.000001f },
				{ -0.260436f,  0.157529f,  0.952553f,  0.000005f },
			},
			{ // Bip001 R Finger42
				{  0.999241f, -0.038964f,  0.000000f,  0.964716f },
				{  0.038964f,  0.999241f,  0.000000f, -0.000005f },
				{  0.000000f,  0.000000f,  1.000000f, -0.000002f },
			},
			{ // Bip001 R Finger43
				{  0.998920f, -0.032892f, -0.032806f,  0.662460f },
				{  0.032930f,  0.999457f,  0.000629f,  0.000000f },
				{  0.032768f, -0.001709f,  0.999462f,  0.000000f },
			},
		},
	};

	// Native open-rest local transforms for v_phone.mdl (relaxed open hand at curl = 0.0).
	static constexpr float s_OpenPhoneRightThumb[3][3][4] = {
		{ // Bip001 R Finger0
			{   0.706251f,   0.604233f,  -0.368935f,   0.795413f },
			{   0.363868f,   0.137220f,   0.921288f,  -0.000595f },
			{   0.607297f,  -0.784904f,  -0.122949f,   0.745692f },
		},
		{ // Bip001 R Finger01
			{   0.983823f,  -0.174108f,   0.042166f,   2.651912f },
			{   0.173948f,   0.984727f,   0.007455f,   0.000000f },
			{  -0.042820f,  -0.000000f,   0.999083f,  -0.000005f },
		},
		{ // Bip001 R Finger02
			{   0.996154f,  -0.087621f,   0.000000f,   1.025909f },
			{   0.087621f,   0.996154f,   0.000000f,  -0.000001f },
			{  -0.000000f,   0.000000f,   1.000000f,  -0.000000f },
		},
	};

	static constexpr float s_OpenPhoneRightFingers[4][4][3][4] = {
		{
			{ // Bip001 R Finger1
				{  0.989995f,  0.109722f, -0.088717f,  0.795403f },
				{ -0.109355f,  0.993962f,  0.008999f, -0.000304f },
				{  0.089168f,  0.000793f,  0.996016f,  0.372847f },
			},
			{ // Bip001 R Finger11
				{  0.998191f, -0.053291f,  0.027821f,  3.850656f },
				{  0.054616f,  0.997291f, -0.049271f,  0.000006f },
				{ -0.025120f,  0.050701f,  0.998398f,  0.000000f },
			},
			{ // Bip001 R Finger12
				{  0.998943f, -0.045959f,  0.000000f,  1.193108f },
				{  0.045959f,  0.998943f,  0.000000f,  0.000001f },
				{  0.000000f,  0.000000f,  1.000000f,  0.000000f },
			},
			{ // Bip001 R Finger13
				{  0.999320f, -0.036863f,  0.000000f,  0.876813f },
				{  0.036863f,  0.999320f,  0.000000f,  0.000000f },
				{  0.000000f,  0.000000f,  1.000000f,  0.000000f },
			},
		},
		{
			{ // Bip001 R Finger2
				{  0.995849f,  0.079692f,  0.043976f,  0.795409f },
				{ -0.068980f,  0.975994f, -0.206587f,  0.000001f },
				{ -0.059383f,  0.202696f,  0.977439f, -0.000001f },
			},
			{ // Bip001 R Finger21
				{  0.998238f, -0.059342f,  0.000000f,  3.807313f },
				{  0.059342f,  0.998238f,  0.000000f,  0.000003f },
				{  0.000000f,  0.000000f,  1.000000f,  0.000000f },
			},
			{ // Bip001 R Finger22
				{  0.998014f, -0.035805f, -0.051824f,  1.391960f },
				{  0.035227f,  0.999307f, -0.012022f, -0.000004f },
				{  0.052219f,  0.010173f,  0.998584f,  0.000000f },
			},
			{ // Bip001 R Finger23
				{  0.999375f, -0.035342f,  0.000000f,  0.726544f },
				{  0.035342f,  0.999375f,  0.000000f,  0.000002f },
				{  0.000000f,  0.000000f,  1.000000f,  0.000000f },
			},
		},
		{
			{ // Bip001 R Finger3
				{  0.989049f,  0.004398f,  0.147521f,  0.795403f },
				{  0.015074f,  0.991318f, -0.130620f,  0.000300f },
				{ -0.146814f,  0.131413f,  0.980396f, -0.372845f },
			},
			{ // Bip001 R Finger31
				{  0.999725f, -0.013574f,  0.019121f,  3.550782f },
				{  0.017268f,  0.977835f, -0.208665f, -0.000002f },
				{ -0.015865f,  0.208938f,  0.977800f,  0.000000f },
			},
			{ // Bip001 R Finger32
				{  0.999065f, -0.043228f,  0.000000f,  1.460090f },
				{  0.043228f,  0.999065f,  0.000000f, -0.000001f },
				{  0.000000f,  0.000000f,  1.000000f,  0.000000f },
			},
			{ // Bip001 R Finger33
				{  0.999401f, -0.034591f, -0.001019f,  0.986084f },
				{  0.034596f,  0.997993f,  0.053041f,  0.000004f },
				{ -0.000818f, -0.053044f,  0.998592f,  0.000000f },
			},
		},
		{
			{ // Bip001 R Finger4
				{  0.963609f, -0.085556f,  0.253254f,  0.795407f },
				{  0.147790f,  0.959947f, -0.238031f,  0.000592f },
				{ -0.222746f,  0.266798f,  0.937658f, -0.745695f },
			},
			{ // Bip001 R Finger41
				{  0.987144f,  0.093066f,  0.129948f,  3.369553f },
				{ -0.061926f,  0.972193f, -0.225843f, -0.000004f },
				{ -0.147353f,  0.214892f,  0.965458f,  0.000000f },
			},
			{ // Bip001 R Finger42
				{  0.999170f, -0.040728f,  0.000000f,  0.964713f },
				{  0.040728f,  0.999170f,  0.000000f,  0.000000f },
				{  0.000000f,  0.000000f,  1.000000f,  0.000000f },
			},
			{ // Bip001 R Finger43
				{  0.998800f, -0.036368f, -0.032806f,  0.662461f },
				{  0.036409f,  0.999337f,  0.000629f, -0.000001f },
				{  0.032762f, -0.001823f,  0.999462f,  0.000001f },
			},
		},
	};

	static constexpr float s_OpenPhoneLeftThumb[3][3][4] = {
		{ // Bip001 L Finger0
			{ +0.713930f, +0.542880f, +0.442250f, +0.795408f },
			{ +0.334800f, +0.290061f, -0.896534f, -0.000595f },
			{ -0.614990f, +0.788128f, +0.025327f, -0.745693f },
		},
		{ // Bip001 L Finger01
			{ +0.981750f, -0.188060f, +0.028278f, +2.960726f },
			{ +0.181653f, +0.971347f, +0.153252f, -0.000004f },
			{ -0.056288f, -0.145318f, +0.987782f, +0.000000f },
		},
		{ // Bip001 L Finger02
			{ +0.996154f, -0.087620f, -0.000000f, +1.025908f },
			{ +0.087620f, +0.996154f, +0.000000f, +0.000000f },
			{ +0.000000f, -0.000000f, +1.000000f, +0.000002f },
		},
	};

	static constexpr float s_OpenPhoneLeftFingers[4][4][3][4] = {
		{
			{ // Bip001 L Finger1
				{ +0.984612f, +0.093069f, +0.147910f, +0.795404f },
				{ -0.098028f, +0.994828f, +0.026586f, -0.000298f },
				{ -0.144671f, -0.040676f, +0.988643f, -0.372847f },
			},
			{ // Bip001 L Finger11
				{ +0.994034f, -0.050455f, -0.096702f, +3.850657f },
				{ +0.053411f, +0.998173f, +0.028234f, -0.000004f },
				{ +0.095101f, -0.033230f, +0.994913f, +0.000000f },
			},
			{ // Bip001 L Finger12
				{ +0.998971f, -0.045363f, -0.000000f, +1.364185f },
				{ +0.045363f, +0.998971f, -0.000000f, +0.000000f },
				{ +0.000000f, +0.000000f, +1.000000f, -0.000000f },
			},
			{ // Bip001 L Finger13
				{ +0.999328f, -0.036644f, -0.000000f, +0.876810f },
				{ +0.036644f, +0.999328f, -0.000000f, +0.000004f },
				{ +0.000000f, +0.000000f, +1.000000f, +0.000000f },
			},
		},
		{
			{ // Bip001 L Finger2
				{ +0.998691f, +0.049635f, +0.012389f, +0.795412f },
				{ -0.051116f, +0.978022f, +0.202140f, +0.000000f },
				{ -0.002083f, -0.202508f, +0.979278f, -0.000000f },
			},
			{ // Bip001 L Finger21
				{ +0.995326f, -0.060010f, -0.075657f, +3.807312f },
				{ +0.058767f, +0.998099f, -0.018564f, +0.000000f },
				{ +0.076627f, +0.014031f, +0.996961f, -0.000001f },
			},
			{ // Bip001 L Finger22
				{ +0.996123f, -0.035767f, +0.080371f, +1.606144f },
				{ +0.034013f, +0.999155f, +0.023086f, +0.000008f },
				{ -0.081129f, -0.020263f, +0.996498f, +0.000000f },
			},
			{ // Bip001 L Finger23
				{ +0.999391f, -0.034899f, +0.000000f, +1.073053f },
				{ +0.034899f, +0.999391f, -0.000000f, -0.000002f },
				{ -0.000000f, +0.000000f, +1.000000f, +0.000000f },
			},
		},
		{
			{ // Bip001 L Finger3
				{ +0.994392f, -0.015209f, -0.104655f, +0.795406f },
				{ +0.029039f, +0.990834f, +0.131929f, +0.000290f },
				{ +0.101689f, -0.134229f, +0.985719f, +0.372847f },
			},
			{ // Bip001 L Finger31
				{ +0.998447f, -0.010592f, -0.054701f, +3.722612f },
				{ +0.021537f, +0.978824f, +0.203565f, +0.000004f },
				{ +0.051387f, -0.204427f, +0.977532f, +0.000000f },
			},
			{ // Bip001 L Finger32
				{ +0.999048f, -0.043619f, +0.000000f, +1.460093f },
				{ +0.043619f, +0.999048f, +0.000000f, +0.000000f },
				{ -0.000000f, -0.000000f, +1.000000f, +0.000000f },
			},
			{ // Bip001 L Finger33
				{ +0.999391f, -0.034872f, +0.001018f, +0.986082f },
				{ +0.034877f, +0.997983f, -0.053041f, -0.000004f },
				{ +0.000833f, +0.053044f, +0.998592f, -0.000000f },
			},
		},
		{
			{ // Bip001 L Finger4
				{ +0.972073f, -0.039234f, -0.231374f, +0.795410f },
				{ +0.196475f, +0.675238f, +0.710951f, +0.000595f },
				{ +0.128339f, -0.736556f, +0.664089f, +0.745694f },
			},
			{ // Bip001 L Finger41
				{ +0.976837f, +0.035437f, -0.211032f, +3.316568f },
				{ -0.061743f, +0.990924f, -0.119401f, -0.000002f },
				{ +0.204886f, +0.129665f, +0.970159f, +0.000004f },
			},
			{ // Bip001 L Finger42
				{ +0.999194f, -0.040132f, +0.000000f, +1.075457f },
				{ +0.040132f, +0.999194f, +0.000000f, -0.000004f },
				{ -0.000000f, -0.000000f, +1.000000f, -0.000001f },
			},
			{ // Bip001 L Finger43
				{ +0.998791f, -0.036614f, +0.032806f, +0.717955f },
				{ +0.036654f, +0.999328f, -0.000629f, -0.000002f },
				{ -0.032761f, +0.001830f, +0.999462f, +0.000000f },
			},
		},
	};

	static constexpr float s_RadialPhoneLeftThumb[3][4] = {
		{ +0.3784f, -0.0515f, 1.0200f, 1.0200f }, // Bip001 L Finger0
		{ +0.0764f, -0.0085f, 1.1000f, 1.1300f }, // Bip001 L Finger01
		{ -0.0389f, +0.0367f, 1.1500f, 1.1800f }, // Bip001 L Finger02
	};

	static constexpr float s_RadialPhoneLeftFingers[4][4][4] = {
		{
			{ +0.1347f, +0.4761f, 1.0400f, 1.0400f }, // Bip001 L Finger1
			{ +0.1205f, +0.0282f, 1.1300f, 1.1200f }, // Bip001 L Finger11
			{ +0.0509f, +0.0071f, 1.1700f, 1.1700f }, // Bip001 L Finger12
			{ -0.0465f, +0.0192f, 1.1700f, 1.1700f }, // Bip001 L Finger13
		},
		{
			{ +0.2445f, +0.6944f, 1.0400f, 1.0400f }, // Bip001 L Finger2
			{ +0.0392f, +0.0005f, 1.1300f, 1.1200f }, // Bip001 L Finger21
			{ +0.0154f, +0.0070f, 1.1700f, 1.1700f }, // Bip001 L Finger22
			{ +0.0035f, +0.0177f, 1.1700f, 1.1700f }, // Bip001 L Finger23
		},
		{
			{ -0.0697f, +0.2762f, 1.0400f, 1.0400f }, // Bip001 L Finger3
			{ -0.0091f, -0.0459f, 1.1600f, 1.1400f }, // Bip001 L Finger31
			{ -0.0050f, -0.0078f, 1.2200f, 1.2200f }, // Bip001 L Finger32
			{ +0.0028f, -0.0346f, 1.2200f, 1.2200f }, // Bip001 L Finger33
		},
		{
			{ -0.0101f, +0.2569f, 1.0400f, 1.0400f }, // Bip001 L Finger4
			{ +0.0141f, -0.1393f, 1.1600f, 1.1400f }, // Bip001 L Finger41
			{ -0.0287f, -0.0133f, 1.2200f, 1.2200f }, // Bip001 L Finger42
			{ -0.0067f, +0.0091f, 1.2200f, 1.2200f }, // Bip001 L Finger43
		},
	};

	static constexpr float s_RadialPhoneRightThumb[3][4] = {
		{ +0.1833f, +0.1855f, 1.0600f, 1.0600f }, // Bip001 R Finger0
		{ +0.1042f, -0.0205f, 1.1000f, 1.1300f }, // Bip001 R Finger01
		{ -0.0128f, +0.0207f, 1.1500f, 1.1800f }, // Bip001 R Finger02
	};

	static constexpr float s_RadialPhoneRightFingers[4][4][4] = {
		{
			{ +0.0211f, -0.2736f, 1.0400f, 1.0400f }, // Bip001 R Finger1
			{ +0.0640f, -0.0076f, 1.1300f, 1.1200f }, // Bip001 R Finger11
			{ +0.0130f, +0.0032f, 1.1700f, 1.1700f }, // Bip001 R Finger12
			{ -0.0613f, -0.0183f, 1.1700f, 1.1700f }, // Bip001 R Finger13
		},
		{
			{ +0.1626f, -0.0411f, 1.0400f, 1.0400f }, // Bip001 R Finger2
			{ +0.0308f, +0.0080f, 1.1300f, 1.1200f }, // Bip001 R Finger21
			{ +0.0347f, -0.0065f, 1.1700f, 1.1700f }, // Bip001 R Finger22
			{ -0.0128f, -0.0108f, 1.1700f, 1.1700f }, // Bip001 R Finger23
		},
		{
			{ +0.3256f, -0.5156f, 1.0400f, 1.0400f }, // Bip001 R Finger3
			{ -0.0257f, +0.0184f, 1.1600f, 1.1400f }, // Bip001 R Finger31
			{ -0.0056f, -0.0154f, 1.2200f, 1.2200f }, // Bip001 R Finger32
			{ -0.0431f, +0.0121f, 1.2200f, 1.2200f }, // Bip001 R Finger33
		},
		{
			{ +0.1166f, -0.2648f, 1.0400f, 1.0400f }, // Bip001 R Finger4
			{ +0.0468f, +0.1333f, 1.1600f, 1.1400f }, // Bip001 R Finger41
			{ +0.0090f, +0.0095f, 1.2200f, 1.2200f }, // Bip001 R Finger42
			{ -0.0124f, +0.0226f, 1.2200f, 1.2200f }, // Bip001 R Finger43
		},
	};

	// Per-bone cross-sectional fullness parameters [cy, cz, sy, sz] measured from VVD shaft centroids.
	// Applied strictly post-kinematically (activeBones[b] = W_kin[b] * S_local[b]) so child joint centers
	// never translate and skin never stretches along local +X.
	static constexpr float s_RadialLeftThumb[3][4] = {
		{  0.4161f, -0.0487f, 1.0200f, 1.0200f }, // Bip001 L Finger0
		{ -0.0219f,  0.1096f, 1.1000f, 1.1300f }, // Bip001 L Finger01
		{ -0.1957f,  0.1091f, 1.1500f, 1.1800f }, // Bip001 L Finger02
	};

	static constexpr float s_RadialLeftFingers[4][4][4] = {
		{
			{ -0.0143f,  0.3681f, 1.0400f, 1.0400f }, // Bip001 L Finger1
			{  0.0198f, -0.1583f, 1.1300f, 1.1200f }, // Bip001 L Finger11
			{ -0.0820f, -0.1247f, 1.1700f, 1.1700f }, // Bip001 L Finger12
			{ -0.1747f, -0.1081f, 1.1700f, 1.1700f }, // Bip001 L Finger13
		},
		{
			{  0.2678f,  0.6544f, 1.0400f, 1.0400f }, // Bip001 L Finger2
			{ -0.0852f, -0.1490f, 1.1300f, 1.1200f }, // Bip001 L Finger21
			{ -0.1283f, -0.1301f, 1.1700f, 1.1700f }, // Bip001 L Finger22
			{ -0.0999f, -0.1024f, 1.1700f, 1.1700f }, // Bip001 L Finger23
		},
		{
			{ -0.0833f,  0.1187f, 1.0400f, 1.0400f }, // Bip001 L Finger3
			{ -0.1045f, -0.2198f, 1.1600f, 1.1400f }, // Bip001 L Finger31
			{ -0.1267f, -0.1664f, 1.2200f, 1.2200f }, // Bip001 L Finger32
			{ -0.1400f, -0.1739f, 1.2200f, 1.2200f }, // Bip001 L Finger33
		},
		{
			{  0.1071f,  0.1215f, 1.0400f, 1.0400f }, // Bip001 L Finger4
			{ -0.1359f, -0.1702f, 1.1600f, 1.1400f }, // Bip001 L Finger41
			{ -0.1620f, -0.1574f, 1.2200f, 1.2200f }, // Bip001 L Finger42
			{ -0.1019f, -0.1758f, 1.2200f, 1.2200f }, // Bip001 L Finger43
		},
	};

	static constexpr float s_RadialRightThumb[3][4] = {
		{  0.3501f,  0.4426f, 1.0600f, 1.0600f }, // Bip001 R Finger0
		{  0.1339f, -0.0102f, 1.1000f, 1.1300f }, // Bip001 R Finger01
		{  0.0339f,  0.0468f, 1.1500f, 1.1800f }, // Bip001 R Finger02
	};

	static constexpr float s_RadialRightFingers[4][4][4] = {
		{
			{ -0.0907f, -0.4737f, 1.0400f, 1.0400f }, // Bip001 R Finger1
			{  0.0858f, -0.0105f, 1.1300f, 1.1200f }, // Bip001 R Finger11
			{  0.0690f, -0.0408f, 1.1700f, 1.1700f }, // Bip001 R Finger12
			{ -0.0357f, -0.0546f, 1.1700f, 1.1700f }, // Bip001 R Finger13
		},
		{
			{  0.1618f, -0.8531f, 1.0400f, 1.0400f }, // Bip001 R Finger2
			{  0.0256f, -0.0116f, 1.1300f, 1.1200f }, // Bip001 R Finger21
			{  0.0753f, -0.0458f, 1.1700f, 1.1700f }, // Bip001 R Finger22
			{ -0.0364f, -0.0458f, 1.1700f, 1.1700f }, // Bip001 R Finger23
		},
		{
			{ -0.1533f, -0.4051f, 1.0400f, 1.0400f }, // Bip001 R Finger3
			{ -0.0146f, -0.0071f, 1.1600f, 1.1400f }, // Bip001 R Finger31
			{ -0.0121f, -0.0535f, 1.2200f, 1.2200f }, // Bip001 R Finger32
			{ -0.0714f, -0.0238f, 1.2200f, 1.2200f }, // Bip001 R Finger33
		},
		{
			{  0.2222f, -0.2470f, 1.0400f, 1.0400f }, // Bip001 R Finger4
			{  0.0132f, -0.0271f, 1.1600f, 1.1400f }, // Bip001 R Finger41
			{  0.0059f, -0.0509f, 1.2200f, 1.2200f }, // Bip001 R Finger42
			{ -0.0320f, -0.0206f, 1.2200f, 1.2200f }, // Bip001 R Finger43
		},
	};

	inline void ApplyRadialFullnessToBone(const matrix3x4_t& wKin, const float radialParams[4], float flexSin, matrix3x4_t& outSkinBone)
	{
		const float cy = radialParams[0];
		const float cz = radialParams[1];
		// Subtle curl-dependent palmar pad volume compensation counteracts LBS knuckle pinching under flexion
		const float sy = radialParams[2] * (1.0f + 0.055f * flexSin);
		const float sz = radialParams[3];
		const float ty = cy * (1.0f - sy);
		const float tz = cz * (1.0f - sz);

		for (int r = 0; r < 3; ++r)
		{
			outSkinBone[r][0] = wKin[r][0];
			outSkinBone[r][1] = wKin[r][1] * sy;
			outSkinBone[r][2] = wKin[r][2] * sz;
			outSkinBone[r][3] = wKin[r][1] * ty + wKin[r][2] * tz + wKin[r][3];
		}
	}

	inline void BuildJointDeltaRotation(float curlDeg, float splayDeg, matrix3x4_t& outR)
	{
		const float czRad = curlDeg * kDeg2Rad;
		const float cyRad = splayDeg * kDeg2Rad;
		const float cz = std::cos(czRad), sz = std::sin(czRad);
		const float cy = std::cos(cyRad), sy = std::sin(cyRad);

		outR[0][0] =  cz * cy; outR[0][1] = -sz;  outR[0][2] =  cz * sy; outR[0][3] = 0.0f;
		outR[1][0] =  sz * cy; outR[1][1] =  cz;  outR[1][2] =  sz * sy; outR[1][3] = 0.0f;
		outR[2][0] = -sy;      outR[2][1] = 0.0f; outR[2][2] =  cy;      outR[2][3] = 0.0f;
	}

	inline void CollapseBoneToPoint(matrix3x4_t* activeBones, int numBones, int boneIdx, float wx, float wy, float wz)
	{
		if (boneIdx >= 0 && boneIdx < numBones)
		{
			std::memset(&activeBones[boneIdx], 0, sizeof(matrix3x4_t));
			activeBones[boneIdx][0][3] = wx;
			activeBones[boneIdx][1][3] = wy;
			activeBones[boneIdx][2][3] = wz;
		}
	}

	// Natural resting flexion added to bare hands at the open end-pose (curl = 0),
	// tapering linearly to 0 extra flexion at full grip (curl = 1) so closed-fist
	// and held-prop grip poses stay unchanged.
	static constexpr float kBareThumbRestDeg[3]  = { 0.0f, 5.0f, 5.0f };
	static constexpr float kBareFingerRestDeg[4] = { 0.0f, 5.0f, 5.0f, 5.0f };
	static constexpr float kDorsalBioMinDeg[4]   = { 0.0f, -17.0f, -3.5f, -5.5f };

	inline void EvalBareThumbCurlDeg(float c01, float outDeg[3], bool isLeftHandMesh = false)
	{
		const float c = ClampF(c01, 0.0f, 1.0f);
		if (isLeftHandMesh)
		{
			outDeg[0] = kBareThumbRestDeg[0] + c * (26.0f - kBareThumbRestDeg[0]);
			outDeg[1] = kBareThumbRestDeg[1] + c * (14.0f - kBareThumbRestDeg[1]);
			outDeg[2] = kBareThumbRestDeg[2] + c * (48.0f - kBareThumbRestDeg[2]);
		}
		else
		{
			outDeg[0] = kBareThumbRestDeg[0] + c * (12.0f - kBareThumbRestDeg[0]);
			outDeg[1] = kBareThumbRestDeg[1] + c * (38.0f - kBareThumbRestDeg[1]);
			outDeg[2] = kBareThumbRestDeg[2] + c * (42.0f - kBareThumbRestDeg[2]);
		}
	}

	inline void EvalBareFingerCurlDeg(int fingerSlot, float c01, float outDeg[4])
	{
		const float c = ClampF(c01, 0.0f, 1.0f);
		const float fullMeta = (fingerSlot >= 2) ? 6.0f : 2.0f;
		outDeg[0] = kBareFingerRestDeg[0] + c * (fullMeta - kBareFingerRestDeg[0]);
		outDeg[1] = kBareFingerRestDeg[1] + c * (65.0f - kBareFingerRestDeg[1]);
		outDeg[2] = kBareFingerRestDeg[2] + c * (70.0f - kBareFingerRestDeg[2]);
		outDeg[3] = kBareFingerRestDeg[3] + c * (50.0f - kBareFingerRestDeg[3]);
	}

	void AnimateHandRig(
		int numBones,
		int wristBone,
		const int thumbBones[3],
		const int fingerBones[4][4],
		const matrix3x4_t* backupBones,
		matrix3x4_t* activeBones,
		const float* rawCurls,
		const float* splays,
		const float* maxCurlClamp,
		bool isLeftHandMesh,
		bool isBareHand,
		bool usePhoneOpenTable,
		const HandSurfaceFingerPose* surfacePose = nullptr)
	{
		if (wristBone < 0 || wristBone >= numBones)
			return;

		float curls[5];
		for (int i = 0; i < 5; ++i)
		{
			float hi = (isBareHand && maxCurlClamp) ? ClampF(maxCurlClamp[i], 0.0f, 1.0f) : 1.0f;
			curls[i] = ClampF(rawCurls[i], 0.0f, hi);
		}

		const auto& radialThumb = usePhoneOpenTable
			? (isLeftHandMesh ? s_RadialPhoneLeftThumb   : s_RadialPhoneRightThumb)
			: (isLeftHandMesh ? s_RadialLeftThumb        : s_RadialRightThumb);
		const auto& radialFingers = usePhoneOpenTable
			? (isLeftHandMesh ? s_RadialPhoneLeftFingers : s_RadialPhoneRightFingers)
			: (isLeftHandMesh ? s_RadialLeftFingers      : s_RadialRightFingers);

		// When the Flashlight is equipped in the Left Hand, keep all joints locked on the handle
		// while still applying cross-sectional radial fullness so finger proportions remain consistent.
		if (isLeftHandMesh && !isBareHand)
		{
			for (int k = 0; k < 3; ++k)
			{
				int b = thumbBones[k];
				if (b >= 0 && b < numBones)
					ApplyRadialFullnessToBone(backupBones[b], radialThumb[k], 0.25f, activeBones[b]);
			}
			for (int f = 0; f < 4; ++f)
			{
				for (int k = 0; k < 4; ++k)
				{
					int b = fingerBones[f][k];
					if (b >= 0 && b < numBones)
						ApplyRadialFullnessToBone(backupBones[b], radialFingers[f][k], 0.45f, activeBones[b]);
				}
			}
			return;
		}

		// When the 3D Phone is actively held in the Right Hand, preserve its native phone-cradling pose
		// so touchscreen ray intersections remain exact, while applying radial fullness.
		if (!isLeftHandMesh && !isBareHand && usePhoneOpenTable)
		{
			for (int k = 0; k < 3; ++k)
			{
				int b = thumbBones[k];
				if (b >= 0 && b < numBones)
					ApplyRadialFullnessToBone(backupBones[b], radialThumb[k], 0.20f, activeBones[b]);
			}
			for (int f = 0; f < 4; ++f)
			{
				for (int k = 0; k < 4; ++k)
				{
					int b = fingerBones[f][k];
					if (b >= 0 && b < numBones)
						ApplyRadialFullnessToBone(backupBones[b], radialFingers[f][k], 0.35f, activeBones[b]);
				}
			}
			return;
		}

		const matrix3x4_t& W_wrist_orig = backupBones[wristBone];

		auto rotateChain = [&](const int* chain, int count, const float (*openPose)[3][4],
		                       const float (*radialTable)[4], const float* curlDegPerJoint, const float* splayDegPerJoint,
		                       float mcpPivotShiftX = 0.0f)
		{
			matrix3x4_t W_parent_orig = W_wrist_orig;
			matrix3x4_t W_parent_kin  = W_wrist_orig;
			for (int k = 0; k < count; ++k)
			{
				int b = chain[k];
				if (b < 0 || b >= numBones)
					break;

				const matrix3x4_t& W_b_orig = backupBones[b];
				matrix3x4_t T_local_base{};
				if (isBareHand && openPose)
				{
					std::memcpy(&T_local_base, openPose[k], sizeof(matrix3x4_t));
				}
				else
				{
					matrix3x4_t W_parent_orig_inv{};
					InvertOrtho3x4(W_parent_orig, W_parent_orig_inv);
					Mul3x4(W_parent_orig_inv, W_b_orig, T_local_base);
				}

				matrix3x4_t R_delta{};
				const float splayDeg = splayDegPerJoint ? splayDegPerJoint[k] : 0.0f;
				BuildJointDeltaRotation(curlDegPerJoint[k], splayDeg, R_delta);
				if (k == 1 && mcpPivotShiftX != 0.0f)
				{
					R_delta[0][3] =  mcpPivotShiftX * (1.0f - R_delta[0][0]);
					R_delta[1][3] = -mcpPivotShiftX * R_delta[1][0];
					R_delta[2][3] = -mcpPivotShiftX * R_delta[2][0];
				}

				matrix3x4_t T_local_kin{}, W_b_kin{};
				Mul3x4(T_local_base, R_delta, T_local_kin);
				Mul3x4(W_parent_kin, T_local_kin, W_b_kin);

				float flexSin = (k >= 1 && curlDegPerJoint[k] > 0.0f)
					? std::sin(std::min(curlDegPerJoint[k], 80.0f) * kDeg2Rad)
					: 0.0f;
				ApplyRadialFullnessToBone(W_b_kin, radialTable[k], flexSin, activeBones[b]);

				W_parent_orig = W_b_orig;
				W_parent_kin  = W_b_kin;
			}
		};

		const auto& openThumb = isLeftHandMesh
			? (usePhoneOpenTable ? s_OpenPhoneLeftThumb   : s_OpenCamLeftThumb)
			: (usePhoneOpenTable ? s_OpenPhoneRightThumb  : s_OpenCamRightThumb);
		const auto& openFingers = isLeftHandMesh
			? (usePhoneOpenTable ? s_OpenPhoneLeftFingers : s_OpenCamLeftFingers)
			: (usePhoneOpenTable ? s_OpenPhoneRightFingers : s_OpenCamRightFingers);

		const bool applySurface = (isBareHand && surfacePose && surfacePose->blendAlpha > 0.002f);
		const float surfAlpha = applySurface ? ClampF(surfacePose->blendAlpha, 0.0f, 1.0f) : 0.0f;

		// 1. Thumb (CMC, MCP, IP)
		const float thumbCurl = curls[0];
		const float thumbFactor = -4.0f + thumbCurl * 3.0f;
		float thumbDeg[3]{};
		if (isBareHand)
		{
			EvalBareThumbCurlDeg(thumbCurl, thumbDeg, isLeftHandMesh);
		}
		else
		{
			thumbDeg[0] = thumbFactor * 2.0f;
			thumbDeg[1] = thumbFactor * 4.0f;
			thumbDeg[2] = thumbFactor * 4.0f;
		}
		const float baseThumbSplay = isBareHand ? ((isLeftHandMesh ? -1.0f : 1.0f) * splays[0] * 10.0f) : 0.0f;
		float thumbSplays[3] = { 0.0f, baseThumbSplay, 0.0f };
		if (applySurface)
		{
			for (int k = 0; k < 3; ++k)
				thumbDeg[k] += (surfacePose->thumbCurlDeg[k] - thumbDeg[k]) * surfAlpha;
			thumbSplays[0] = surfacePose->thumbSplayDeg[0] * surfAlpha;
			thumbSplays[1] = baseThumbSplay + (surfacePose->thumbSplayDeg[1] - baseThumbSplay) * surfAlpha;
		}
		rotateChain(thumbBones, 3, openThumb, radialThumb, thumbDeg, thumbSplays,
		            (isLeftHandMesh && isBareHand) ? -0.45f : 0.0f);

		// 2. Index, Middle, Ring, Pinky (Metacarpal, MCP, PIP, DIP)
		for (int f = 0; f < 4; ++f)
		{
			if (!isBareHand)
			{
				// Camera equipped in Right Hand: keep Middle, Ring, Pinky wrapped around camera body
				if (f >= 1)
				{
					for (int k = 0; k < 4; ++k)
					{
						int b = fingerBones[f][k];
						if (b >= 0 && b < numBones)
							ApplyRadialFullnessToBone(backupBones[b], radialFingers[f][k], 0.50f, activeBones[b]);
					}
					continue;
				}

				// Index finger shutter motion (-40 deg hovering -> 0 deg pressing shutter)
				const float c = curls[1];
				const float indexFactor = -2.0f + c * 2.0f;
				const float jointDeg[4] = {
					0.0f,
					indexFactor * 8.0f,
					indexFactor * 7.0f,
					indexFactor * 5.0f
				};
				rotateChain(fingerBones[0], 4, nullptr, radialFingers[0], jointDeg, nullptr);
				continue;
			}

			const float c = curls[f + 1];
			// Subtle metacarpal arch cupping on Ring/Pinky and natural 3-joint flexion ratios
			float jointDeg[4]{};
			EvalBareFingerCurlDeg(f, c, jointDeg);
			const float splaySign = isLeftHandMesh ? 1.0f : -1.0f;
			float splayDeg = splaySign * (splays[f] * 10.0f + (0.5f - c) * kSplayWeights[f] * 4.0f);
			if (applySurface)
			{
				for (int k = 0; k < 4; ++k)
					jointDeg[k] += (surfacePose->fingerCurlDeg[f][k] - jointDeg[k]) * surfAlpha;
				splayDeg += (surfacePose->fingerSplayDeg[f] - splayDeg) * surfAlpha;
			}
			const float fingerSplays[4] = { 0.0f, splayDeg, 0.0f, 0.0f };
			rotateChain(fingerBones[f], 4, openFingers[f], radialFingers[f], jointDeg, fingerSplays);
		}
	}
}

namespace VRHands
{
	const char* SelectBareHandModel(Game* game, int* outModelIndex)
	{
		static const char* s_RightHandFallbacks[] = {
			"models/weapons/v_phone.mdl",
			"models/weapons/v_camera.mdl",
			"models/weapons/v_light.mdl",
			"models/weapons/v_superlight.mdl"
		};
		if (game && game->m_ModelInfo)
		{
			for (const char* candidateMdl : s_RightHandFallbacks)
			{
				int idx = game->m_ModelInfo->GetModelIndex(candidateMdl);
				if (idx != -1)
				{
					if (outModelIndex)
						*outModelIndex = idx;
					return candidateMdl;
				}
			}
		}
		if (outModelIndex)
			*outModelIndex = -1;
		return "models/weapons/v_light.mdl";
	}

	struct FingerInputTrackState
	{
		vr::VRActionHandle_t actionHandle = vr::k_ulInvalidActionHandle;
		float minObserved[5] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
		float steadyBaseline[5] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
		float winMin[5] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
		float winMax[5] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
		int steadyFrames[5] = { 0, 0, 0, 0, 0 };
		int lowRestFrames[5] = { 0, 0, 0, 0, 0 };
		unsigned long long lastLogMs = 0ULL;
		float lastLoggedRaw[5] = { -1.0f, -1.0f, -1.0f, -1.0f, -1.0f };
		float lastLoggedEffLo[5] = { -1.0f, -1.0f, -1.0f, -1.0f, -1.0f };
	};

	std::mutex s_FingerInputMutex;
	FingerInputTrackState s_FingerTracks[4]{};
	std::atomic<bool> s_FingerDebugLogEnabled{ false };
	int s_FingerDiagLines = 0;
	bool s_FingerDiagHeaderWritten = false;

	int AcquireFingerTrackSlot(vr::VRActionHandle_t skelAction)
	{
		for (int i = 0; i < 4; ++i)
		{
			if (s_FingerTracks[i].actionHandle == skelAction)
				return i;
			if (s_FingerTracks[i].actionHandle == vr::k_ulInvalidActionHandle)
			{
				s_FingerTracks[i].actionHandle = skelAction;
				return i;
			}
		}
		return 0;
	}

	void LogFingerDiagIfUseful(
		int slotIdx,
		uint32_t devIdx,
		int trackLvl,
		const char* srcLabel,
		bool triggerPressed,
		bool gripPressed,
		bool hasDev,
		const float rawDev[5],
		bool hasAnim,
		const float rawAnim[5],
		const float effLo[5],
		const float outCurls[5],
		unsigned long long nowMs)
	{
		if (!s_FingerDebugLogEnabled.load(std::memory_order_acquire) || s_FingerDiagLines >= 64)
			return;
		FingerInputTrackState& st = s_FingerTracks[slotIdx];
		bool changed = (st.lastLogMs == 0ULL);
		if (!changed)
		{
			if ((nowMs - st.lastLogMs) < 2500ULL)
				return;
			for (int i = 0; i < 5; ++i)
			{
				if (std::fabs(rawDev[i] - st.lastLoggedRaw[i]) >= 0.05f ||
					std::fabs(effLo[i] - st.lastLoggedEffLo[i]) >= 0.02f)
				{
					changed = true;
					break;
				}
			}
		}
		if (!changed)
			return;

		const std::string vrDir = VR::ResolveGameRelativePath("VR");
		const DWORD vrAttr = GetFileAttributesA(vrDir.c_str());
		if (vrAttr == INVALID_FILE_ATTRIBUTES || (vrAttr & FILE_ATTRIBUTE_DIRECTORY) == 0)
			return;

		const std::string logPath = VR::ResolveGameRelativePath("VR\\finger_debug.txt");
		std::ofstream out(logPath, s_FingerDiagHeaderWritten ? std::ios::app : std::ios::trunc);
		if (!out.is_open())
			return;

		if (!s_FingerDiagHeaderWritten)
		{
			out << "# slot devIdx trackLvl src trig grip | dev[T I M R P] | anim[T I M R P] | effLo[T I M R P] | out[T I M R P]\n";
			s_FingerDiagHeaderWritten = true;
		}

		char lineBuf[512]{};
		std::snprintf(
			lineBuf,
			sizeof(lineBuf),
			"s%d dev=%u lvl=%d src=%s t=%d g=%d | dev=[%.3f %.3f %.3f %.3f %.3f] | anim=[%.3f %.3f %.3f %.3f %.3f] | lo=[%.3f %.3f %.3f %.3f %.3f] | out=[%.3f %.3f %.3f %.3f %.3f]\n",
			slotIdx,
			devIdx,
			trackLvl,
			srcLabel,
			triggerPressed ? 1 : 0,
			gripPressed ? 1 : 0,
			hasDev ? rawDev[0] : -1.0f, hasDev ? rawDev[1] : -1.0f, hasDev ? rawDev[2] : -1.0f, hasDev ? rawDev[3] : -1.0f, hasDev ? rawDev[4] : -1.0f,
			hasAnim ? rawAnim[0] : -1.0f, hasAnim ? rawAnim[1] : -1.0f, hasAnim ? rawAnim[2] : -1.0f, hasAnim ? rawAnim[3] : -1.0f, hasAnim ? rawAnim[4] : -1.0f,
			effLo[0], effLo[1], effLo[2], effLo[3], effLo[4],
			outCurls[0], outCurls[1], outCurls[2], outCurls[3], outCurls[4]);
		out << lineBuf;

		st.lastLogMs = nowMs;
		for (int i = 0; i < 5; ++i)
		{
			st.lastLoggedRaw[i] = rawDev[i];
			st.lastLoggedEffLo[i] = effLo[i];
		}
		++s_FingerDiagLines;
	}

	void SetFingerDebugLogEnabled(bool enabled)
	{
		s_FingerDebugLogEnabled.store(enabled, std::memory_order_release);
	}

	void UpdateFingerInput(
		vr::IVRInput* input,
		vr::VRActionHandle_t skelAction,
		bool triggerPressed,
		bool gripPressed,
		bool hasCtrlState,
		const vr::VRControllerState_t& ctrlState,
		float (&curls)[5],
		float (&splays)[4],
		bool* outHasPartialOrFullSkeletal)
	{
		float targetCurls[5] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
		float targetSplays[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		bool gotSkeletal = false;

		vr::EVRSkeletalTrackingLevel trackLevel = vr::VRSkeletalTracking_Estimated;
		bool hasDev = false;
		bool hasAnim = false;
		float rawDev[5] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
		float rawAnim[5] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
		float effLo[5] = { 0.08f, 0.16f, 0.16f, 0.16f, 0.16f };
		uint32_t devIdx = 0xFFFFFFFFu;
		const char* srcLabel = "none";

		if (input && skelAction != vr::k_ulInvalidActionHandle)
		{
			input->GetSkeletalTrackingLevel(skelAction, &trackLevel);

			vr::InputSkeletalActionData_t skelData{};
			if (input->GetSkeletalActionData(skelAction, &skelData, sizeof(skelData)) == vr::VRInputError_None && skelData.bActive)
			{
				vr::InputOriginInfo_t originInfo{};
				if (input->GetOriginTrackedDeviceInfo(skelData.activeOrigin, &originInfo, sizeof(originInfo)) == vr::VRInputError_None)
					devIdx = originInfo.trackedDeviceIndex;

				vr::VRSkeletalSummaryData_t summaryDev{};
				vr::VRSkeletalSummaryData_t summaryAnim{};
				hasDev = (input->GetSkeletalSummaryData(skelAction, vr::VRSummaryType_FromDevice, &summaryDev) == vr::VRInputError_None);
				hasAnim = (input->GetSkeletalSummaryData(skelAction, vr::VRSummaryType_FromAnimation, &summaryAnim) == vr::VRInputError_None);

				auto summaryFinite = [](const vr::VRSkeletalSummaryData_t& s) -> bool
				{
					for (int i = 0; i < 5; ++i)
					{
						if (!std::isfinite(s.flFingerCurl[i]))
							return false;
					}
					return true;
				};

				if (hasDev && !summaryFinite(summaryDev))
					hasDev = false;
				if (hasAnim && !summaryFinite(summaryAnim))
					hasAnim = false;

				if (hasDev)
				{
					for (int i = 0; i < 5; ++i)
						rawDev[i] = ClampF(summaryDev.flFingerCurl[i], 0.0f, 1.0f);
				}
				if (hasAnim)
				{
					for (int i = 0; i < 5; ++i)
						rawAnim[i] = ClampF(summaryAnim.flFingerCurl[i], 0.0f, 1.0f);
				}

				if (hasDev || hasAnim)
				{
					const vr::VRSkeletalSummaryData_t& activeSummary = hasDev ? summaryDev : summaryAnim;
					srcLabel = hasDev ? "dev" : "anim";
					const unsigned long long nowMs = GetTickCount64();

					std::lock_guard<std::mutex> lock(s_FingerInputMutex);
					const int slotIdx = AcquireFingerTrackSlot(skelAction);
					FingerInputTrackState& st = s_FingerTracks[slotIdx];

					for (int i = 0; i < 5; ++i)
					{
						const float raw = ClampF(activeSummary.flFingerCurl[i], 0.0f, 1.0f);
						const float defaultLo = (i == 0)
							? 0.08f
							: ((trackLevel >= vr::VRSkeletalTracking_Partial) ? 0.16f : 0.06f);
						const float hi = (i == 0)
							? 0.94f
							: ((trackLevel >= vr::VRSkeletalTracking_Partial) ? 0.92f : 0.95f);

						if (trackLevel >= vr::VRSkeletalTracking_Partial && i >= 1)
						{
							if (raw <= defaultLo)
							{
								st.steadyFrames[i] = 0;
								st.winMin[i] = 1.0f;
								st.winMax[i] = 0.0f;
								if (st.lowRestFrames[i] < 15)
									++st.lowRestFrames[i];
								if (st.lowRestFrames[i] >= 15)
								{
									// Sustained open rest below deadzone: lock permanently to default threshold.
									st.minObserved[i] = std::min(st.minObserved[i], raw);
									st.steadyBaseline[i] = 0.0f;
								}
							}
							else
							{
								st.lowRestFrames[i] = 0;
								if (st.minObserved[i] <= defaultLo)
								{
									st.steadyBaseline[i] = 0.0f;
									st.steadyFrames[i] = 0;
								}
								else
								{
									st.minObserved[i] = std::min(st.minObserved[i], raw);
									if (st.steadyBaseline[i] > st.minObserved[i] + 0.02f)
										st.steadyBaseline[i] = ClampF(st.minObserved[i] + 0.02f, defaultLo, 0.38f);

									if (!gripPressed && !triggerPressed && raw <= 0.38f)
									{
										st.winMin[i] = std::min(st.winMin[i], raw);
										st.winMax[i] = std::max(st.winMax[i], raw);
										++st.steadyFrames[i];
										if ((st.winMax[i] - st.winMin[i]) > 0.045f)
										{
											st.steadyFrames[i] = 1;
											st.winMin[i] = raw;
											st.winMax[i] = raw;
										}
										else if (st.steadyFrames[i] >= 45)
										{
											st.steadyBaseline[i] = ClampF(st.winMin[i] + 0.02f, defaultLo, 0.38f);
										}
									}
									else
									{
										st.steadyFrames[i] = 0;
										st.winMin[i] = 1.0f;
										st.winMax[i] = 0.0f;
									}
								}
							}
							effLo[i] = std::max(defaultLo, st.steadyBaseline[i]);
						}
						else
						{
							effLo[i] = defaultLo;
						}

						targetCurls[i] = (raw <= effLo[i])
							? 0.0f
							: ClampF((raw - effLo[i]) / std::max(hi - effLo[i], 0.15f), 0.0f, 1.0f);
					}
					for (int i = 0; i < 4; ++i)
					{
						float s = activeSummary.flFingerSplay[i];
						targetSplays[i] = std::isfinite(s) ? ClampF(s, -1.0f, 1.0f) : 0.0f;
					}
					gotSkeletal = true;
					LogFingerDiagIfUseful(
						slotIdx, devIdx, static_cast<int>(trackLevel), srcLabel,
						triggerPressed, gripPressed,
						hasDev, rawDev, hasAnim, rawAnim, effLo, targetCurls, nowMs);
				}
			}
		}

		if (!gotSkeletal && hasCtrlState)
		{
			const uint64_t thumbMask =
				vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Touchpad) |
				vr::ButtonMaskFromId(vr::k_EButton_IndexController_JoyStick) |
				vr::ButtonMaskFromId(vr::k_EButton_IndexController_A) |
				vr::ButtonMaskFromId(vr::k_EButton_IndexController_B) |
				vr::ButtonMaskFromId(vr::k_EButton_ApplicationMenu);
			if ((ctrlState.ulButtonPressed & thumbMask) != 0)
				targetCurls[0] = 0.85f;
			else if ((ctrlState.ulButtonTouched & thumbMask) != 0)
				targetCurls[0] = 0.50f;

			// Smooth analog trigger pull without snapping Index to 0.40 on light capacitive proximity
			const float rawTrig = ctrlState.rAxis[1].x;
			float trigAxis = std::isfinite(rawTrig) ? ClampF(rawTrig, 0.0f, 1.0f) : 0.0f;
			float trigVal = (trigAxis <= 0.04f) ? 0.0f : ((trigAxis - 0.04f) / 0.96f);
			if (triggerPressed)
				trigVal = std::max(trigVal, 0.90f);
			targetCurls[1] = trigVal;

			const float rawGrip = ctrlState.rAxis[2].x;
			float gripAxis = std::isfinite(rawGrip) ? ClampF(rawGrip, 0.0f, 1.0f) : 0.0f;
			float gripVal = (gripAxis <= 0.05f) ? 0.0f : ((gripAxis - 0.05f) / 0.95f);
			if (gripPressed)
				gripVal = std::max(gripVal, 0.90f);
			targetCurls[2] = gripVal;
			targetCurls[3] = gripVal;
			targetCurls[4] = gripVal;

			if (input && skelAction != vr::k_ulInvalidActionHandle)
			{
				std::lock_guard<std::mutex> lock(s_FingerInputMutex);
				const int slotIdx = AcquireFingerTrackSlot(skelAction);
				const float ctrlRaw[5] = {
					targetCurls[0], trigAxis, gripAxis, gripAxis, gripAxis
				};
				LogFingerDiagIfUseful(
					slotIdx, devIdx, static_cast<int>(trackLevel), "ctrl",
					triggerPressed, gripPressed,
					true, ctrlRaw, false, rawAnim, effLo, targetCurls, GetTickCount64());
			}
		}

		constexpr float kSmooth = 0.35f;
		for (int i = 0; i < 5; ++i)
		{
			curls[i] += (targetCurls[i] - curls[i]) * kSmooth;
			if (curls[i] < 0.002f && targetCurls[i] == 0.0f)
				curls[i] = 0.0f;
		}
		for (int i = 0; i < 4; ++i)
			splays[i] += (targetSplays[i] - splays[i]) * kSmooth;
		if (outHasPartialOrFullSkeletal)
			*outHasPartialOrFullSkeletal = (gotSkeletal && trackLevel >= vr::VRSkeletalTracking_Partial);
	}

	void SetFingerCurlLimits(bool physicalLeftHand, const float maxCurls[5])
	{
		if (!maxCurls)
		{
			ClearFingerCurlLimits(physicalLeftHand);
			return;
		}
		FingerCurlLimitSlot& slot = s_CurlLimitSlots[physicalLeftHand ? 1 : 0];
		for (int i = 0; i < 5; ++i)
			slot.maxCurl[i].store(ClampF(maxCurls[i], 0.0f, 1.0f), std::memory_order_relaxed);
		slot.active.store(true, std::memory_order_release);
	}

	void ClearFingerCurlLimits(bool physicalLeftHand)
	{
		FingerCurlLimitSlot& slot = s_CurlLimitSlots[physicalLeftHand ? 1 : 0];
		slot.active.store(false, std::memory_order_release);
		for (int i = 0; i < 5; ++i)
			slot.maxCurl[i].store(1.0f, std::memory_order_relaxed);
	}

	bool GetFingerCurlLimits(bool physicalLeftHand, float outMaxCurls[5])
	{
		const FingerCurlLimitSlot& slot = s_CurlLimitSlots[physicalLeftHand ? 1 : 0];
		if (!slot.active.load(std::memory_order_acquire))
		{
			if (outMaxCurls)
			{
				for (int i = 0; i < 5; ++i)
					outMaxCurls[i] = 1.0f;
			}
			return false;
		}
		if (outMaxCurls)
		{
			for (int i = 0; i < 5; ++i)
				outMaxCurls[i] = slot.maxCurl[i].load(std::memory_order_relaxed);
		}
		return true;
	}

	namespace
	{
		class CHandWorldPropTraceFilter final : public CTraceFilter
		{
		public:
			CHandWorldPropTraceFilter(
				IHandleEntity* localPlayer,
				IHandleEntity* skip1,
				IHandleEntity* skip2,
				IHandleEntity* skip3,
				bool checkNPCs,
				bool includePhysicsProps = false)
				: CTraceFilter(localPlayer, 0)
				, m_LocalPass(localPlayer)
				, m_Skip1(skip1)
				, m_Skip2(skip2)
				, m_Skip3(skip3)
				, m_CheckNPCs(checkNPCs)
				, m_IncludePhysicsProps(includePhysicsProps)
			{
			}

			bool ShouldHitEntity(IHandleEntity* pServerEntity, int contentsMask) override
			{
				(void)contentsMask;
				if (!pServerEntity)
					return true;

				if (pServerEntity == m_LocalPass ||
					(m_Skip1 && pServerEntity == m_Skip1) ||
					(m_Skip2 && pServerEntity == m_Skip2) ||
					(m_Skip3 && pServerEntity == m_Skip3))
				{
					return false;
				}
				if (!m_CheckNPCs)
					return true;

				void* const* entVt = *reinterpret_cast<void* const* const*>(pServerEntity);
				if (!entVt)
					return true;

				// IClientUnknown::GetBaseEntity() is slot 7 (+0x1C); CStaticProp returns nullptr and stays solid.
				typedef void* (__thiscall *tNoArgPtrFn)(void* thisptr);
				auto fnGetBaseEntity = reinterpret_cast<tNoArgPtrFn>(entVt[7]);
				void* baseEnt = fnGetBaseEntity ? fnGetBaseEntity(pServerEntity) : nullptr;
				if (!baseEnt)
					return true;

				if (baseEnt == m_LocalPass ||
					(m_Skip1 && baseEnt == m_Skip1) ||
					(m_Skip2 && baseEnt == m_Skip2) ||
					(m_Skip3 && baseEnt == m_Skip3))
				{
					return false;
				}

				void* const* baseVt = *reinterpret_cast<void* const* const*>(baseEnt);
				if (!baseVt)
					return true;

				// Verified client.dll slots on C_BaseEntity: IsPlayer = 149 (+0x254), IsNPC = 152 (+0x260) (both ret 0).
				typedef bool (__thiscall *tEntityFlagFn)(void* thisptr);
				auto fnIsPlayer = reinterpret_cast<tEntityFlagFn>(baseVt[149]);
				auto fnIsNPC    = reinterpret_cast<tEntityFlagFn>(baseVt[152]);
				if ((fnIsPlayer && fnIsPlayer(baseEnt)) || (fnIsNPC && fnIsNPC(baseEnt)))
					return false;

				// Query IClientUnknown::GetClientNetworkable() (slot 4, +0x10) -> IClientNetworkable::GetClientClass() (slot 2, +0x08)
				// -> ClientClass::m_pNetworkName (+0x08), plus C_BaseEntity::m_MoveType (+0x354).
				const char* netName = nullptr;
				auto fnGetClientNetworkable = reinterpret_cast<tNoArgPtrFn>(entVt[4]);
				void* clientNet = fnGetClientNetworkable ? fnGetClientNetworkable(pServerEntity) : nullptr;
				if (clientNet)
				{
					void* const* netVt = *reinterpret_cast<void* const* const*>(clientNet);
					if (netVt && netVt[2])
					{
						auto fnGetClientClass = reinterpret_cast<tNoArgPtrFn>(netVt[2]);
						void* clientClass = fnGetClientClass(clientNet);
						if (clientClass)
							netName = *reinterpret_cast<const char* const*>(reinterpret_cast<uintptr_t>(clientClass) + 0x08);
					}
				}

				if (netName && netName[0] != '\0')
				{
					// World, doors, brush fixtures, and static/dynamic architectural props stay solid.
					if (std::strcmp(netName, "CWorld") == 0 ||
						std::strstr(netName, "Door") != nullptr ||
						std::strncmp(netName, "CFunc", 5) == 0 ||
						std::strcmp(netName, "CDynamicProp") == 0 ||
						std::strcmp(netName, "CStatueProp") == 0 ||
						std::strcmp(netName, "CBreakableSurface") == 0 ||
						std::strcmp(netName, "CBaseButton") == 0 ||
						std::strcmp(netName, "CINFRAButton") == 0 ||
						std::strcmp(netName, "CMomentaryRotButton") == 0 ||
						std::strcmp(netName, "CPropFloorButton") == 0 ||
						std::strcmp(netName, "CINFRACrane") == 0 ||
						std::strcmp(netName, "CINFRABoat") == 0)
					{
						return true;
					}

					// Always skip collectibles, player tools, viewmodels, weapons, and ragdolls.
					if (std::strcmp(netName, "CINFRADocument") == 0 ||
						std::strcmp(netName, "CINFRA_Document") == 0 ||
						std::strcmp(netName, "CINFRAGeocache") == 0 ||
						std::strcmp(netName, "CINFRA_Geocache") == 0 ||
						std::strcmp(netName, "CINFRACamera") == 0 ||
						std::strcmp(netName, "C_INFRACamera") == 0 ||
						std::strcmp(netName, "CINFRAPhone") == 0 ||
						std::strcmp(netName, "CINFRAHands") == 0 ||
						std::strcmp(netName, "C_INFRAHands") == 0 ||
						std::strcmp(netName, "CINFRA_ViewModel") == 0 ||
						std::strcmp(netName, "CPredictedViewModel") == 0 ||
						std::strcmp(netName, "CBaseViewModel") == 0 ||
						std::strcmp(netName, "CRagdollProp") == 0 ||
						std::strcmp(netName, "CRagdollPropAttached") == 0 ||
						std::strcmp(netName, "CPortalRagdoll") == 0 ||
						std::strcmp(netName, "C_ClientRagdoll") == 0 ||
						std::strcmp(netName, "CClientRagdoll") == 0 ||
						std::strncmp(netName, "CWeapon", 7) == 0 ||
						std::strncmp(netName, "C_Weapon", 8) == 0 ||
						std::strncmp(netName, "CBaseCombatWeapon", 17) == 0 ||
						std::strncmp(netName, "CItem", 5) == 0)
					{
						return false;
					}

					// Skip movable/pickupable physics props for palm sweeps (so players can freely push/grab them),
					// while optionally allowing free fingers to conform to stationary props (e.g. desk mice).
					if (std::strcmp(netName, "CPhysicsProp") == 0 ||
						std::strcmp(netName, "CPhysicsPropMultiplayer") == 0 ||
						std::strcmp(netName, "CPhysBox") == 0 ||
						std::strcmp(netName, "CPhysBoxMultiplayer") == 0 ||
						std::strcmp(netName, "CBreakableProp") == 0 ||
						std::strcmp(netName, "CPropWeightedCube") == 0 ||
						std::strcmp(netName, "CPropCombineBall") == 0 ||
						std::strcmp(netName, "CPropEnergyBall") == 0 ||
						std::strcmp(netName, "CPropPaintBomb") == 0)
					{
						return m_IncludePhysicsProps;
					}
				}

				// Skip movable/non-networked physics debris & props via m_MoveType (+0x354):
				// MOVETYPE_FLYGRAVITY = 5, MOVETYPE_VPHYSICS = 6.
				// Client-only non-networked debris (!netName) is ALWAYS skipped so cosmetic shards
				// never block bare hands; other physics props are skipped when !m_IncludePhysicsProps.
				const uint8_t moveType = *reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(baseEnt) + 0x354);
				if (moveType == 6u || moveType == 5u)
				{
					if (!netName || netName[0] == '\0' || !m_IncludePhysicsProps)
						return false;
				}

				return true;
			}

		private:
			IHandleEntity* m_LocalPass;
			IHandleEntity* m_Skip1;
			IHandleEntity* m_Skip2;
			IHandleEntity* m_Skip3;
			bool m_CheckNPCs;
			bool m_IncludePhysicsProps;
		};

		inline float VecLen3(const Vector& v)
		{
			return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
		}

		Vector ClampVectorMaxLength(const Vector& v, float maxLen)
		{
			const float lenSq = v.x * v.x + v.y * v.y + v.z * v.z;
			if (lenSq > maxLen * maxLen && lenSq > 1e-8f)
			{
				const float scale = maxLen / std::sqrt(lenSq);
				return Vector(v.x * scale, v.y * scale, v.z * scale);
			}
			return v;
		}

		Vector GetPalmCenterInWristLocal(HandSkinPreset preset)
		{
			// Mid-palm interior slab center (>= 18 mm inside all VVD skin surfaces):
			switch (preset)
			{
			case HandSkinPreset::LeftPhone:
				return Vector(3.25f,  0.02f, -0.40f);
			case HandSkinPreset::LeftLight:
			case HandSkinPreset::LeftSuper:
				return Vector(3.30f, -0.42f, -0.42f);
			case HandSkinPreset::RightPhone:
				return Vector(3.25f,  0.02f,  0.20f);
			case HandSkinPreset::RightCam:
			case HandSkinPreset::RightSuper:
			default:
				return Vector(3.25f, -0.30f,  0.20f);
			}
		}

		bool ComputeRawControllerWristPose(
			VR* vr,
			bool physicalLeftHand,
			const Vector& eyePosition,
			unsigned long long nowMs,
			matrix3x4_t& outModelToWorld,
			matrix3x4_t& outWristToWorld,
			bool& outUsePhoneOpen,
			bool& outIsLeftMesh,
			Vector& outFwd,
			Vector& outLeft,
			Vector& outUp,
			HandSkinPreset* outSkinPreset = nullptr)
		{
			if (!vr || !vr->m_IsVREnabled)
				return false;

			const bool useVm0 = vr->m_LeftHanded ? physicalLeftHand : !physicalLeftHand;
			outIsLeftMesh = !useVm0;

			const Vector ctrlPos = physicalLeftHand
				? vr->GetLeftControllerAbsPos(eyePosition)
				: vr->GetRightControllerAbsPos(eyePosition);

			// Read the calibrated orthonormal 3D controller basis directly from VR to avoid QAngle::VectorAngles
			// +/-90 deg pitch Euler gimbal singularities when tilting hands >70..140 deg backward.
			const Vector rawBasisFwd = physicalLeftHand ? vr->m_LeftControllerForward : vr->m_RightControllerForward;
			const Vector rawBasisRight = physicalLeftHand ? vr->m_LeftControllerRight : vr->m_RightControllerRight;
			const Vector rawBasisUp = physicalLeftHand ? vr->m_LeftControllerUp : vr->m_RightControllerUp;

			Vector right;
			if (DotProduct(rawBasisFwd, rawBasisFwd) > 0.5f &&
				DotProduct(rawBasisRight, rawBasisRight) > 0.5f &&
				DotProduct(rawBasisUp, rawBasisUp) > 0.5f)
			{
				outFwd = NormalizeSafe(rawBasisFwd, Vector(1.0f, 0.0f, 0.0f));
				Vector orthoRight = rawBasisRight - outFwd * DotProduct(rawBasisRight, outFwd);
				right = NormalizeSafe(orthoRight, Vector(0.0f, -1.0f, 0.0f));
				outLeft = Vector(-right.x, -right.y, -right.z);
				outUp = NormalizeSafe(CrossProduct(outFwd, outLeft), rawBasisUp);
				if (DotProduct(outUp, rawBasisUp) < 0.0f)
					outUp = Vector(-outUp.x, -outUp.y, -outUp.z);
			}
			else
			{
				const QAngle ctrlAng = physicalLeftHand
					? vr->GetLeftControllerAbsAngle()
					: vr->GetRightControllerAbsAngle();
				QAngle::AngleVectors(ctrlAng, &outFwd, &right, &outUp);
				outLeft = Vector(-right.x, -right.y, -right.z);
			}

			Vector vmOrigin = useVm0
				? (ctrlPos - (outFwd * 15.5f) - (right * 1.9f) + (outUp * 2.1f))
				: (ctrlPos - (outFwd * 16.7f) + (right * 7.6f) + (outUp * 5.0f));

			if (useVm0 && vr->m_PhoneEquipped)
			{
				VRHands::ApplyEquippedPhoneTilt(vmOrigin, outFwd, right, outUp);
				outLeft = Vector(-right.x, -right.y, -right.z);
			}
			else if (useVm0 && !vr->m_RightHandEmpty)
			{
				const float camShiftUnits = 0.01f * vr->m_VRScale; // 1.0 cm up (+outUp) and 1.0 cm toward viewer (-outFwd)
				vmOrigin = vmOrigin + (outUp * camShiftUnits) - (outFwd * camShiftUnits);
			}

			outModelToWorld = matrix3x4_t(outFwd, outLeft, outUp, vmOrigin);

			matrix3x4_t modelToWrist = useVm0 ? s_CamRightHand : s_LightLeftHand;
			outUsePhoneOpen = false;
			HandSkinPreset preset = useVm0 ? HandSkinPreset::RightCam : HandSkinPreset::LeftLight;
			// Non-expiring (maxAgeMs = 0) while in game so frustum-culling the viewmodel when tilted behind HMD
			// never reverts modelToWrist or usePhoneOpenTable mid-hold; cleared on ResetVisualHandPhysics().
			s_ModelToWristCache[useVm0 ? 0 : 1].Read(modelToWrist, outUsePhoneOpen, &preset, nowMs, 0ULL);
			if (outSkinPreset)
				*outSkinPreset = preset;

			Mul3x4(outModelToWorld, modelToWrist, outWristToWorld);
			return true;
		}

		Vector ComputeSafeElbowAnchor(const Vector& safeShoulder, const Vector& palmPos, const Vector& wristDistal)
		{
			const Vector armVec = palmPos - safeShoulder;
			const float armLen = VecLen3(armVec);
			if (armLen <= 1e-4f)
				return safeShoulder;

			const Vector armDir = armVec * (1.0f / armLen);
			// Only follow wristDistal when pointing outward along the arm; when wrist tilts >90 deg backward
			// toward the player, clamp align to 0 so elbowAnchor never shoots forward past the hand into walls/tables.
			const float align = ClampF(DotProduct(wristDistal, armDir), 0.0f, 1.0f);
			const Vector foreDir = NormalizeSafe(
				armDir * (1.0f - 0.45f * align) + wristDistal * (0.45f * align),
				armDir);
			const float backDist = std::min(11.5f, armLen * 0.65f);
			Vector elbow = palmPos - foreDir * backDist;

			// Guarantee elbow stays strictly behind palmPos along armDir:
			const float projFromPalm = DotProduct(elbow - palmPos, armDir);
			if (projFromPalm > 0.0f)
				elbow = elbow - armDir * projFromPalm;
			return elbow;
		}

		void ComputeCoupledHeldWrist(
			const VRHands::HeldHandPhysicsFeedback& fb,
			const matrix3x4_t& rawWrist,
			matrix3x4_t& outCoupledTargetWrist,
			float& outAlpha)
		{
			matrix3x4_t W_obj_target{}, W_obj_actual{}, W_obj_target_inv{}, T_wrist_in_obj{}, W_wrist_at_obj{};
			AnglePosToMatrix3x4(fb.targetEntAng, fb.targetEntPos, W_obj_target);
			AnglePosToMatrix3x4(fb.actualEntAng, fb.actualEntPos, W_obj_actual);

			const matrix3x4_t& baseTargetWrist = IsRotationBasisValid(fb.targetWristToWorld)
				? fb.targetWristToWorld
				: rawWrist;
			InvertOrtho3x4(W_obj_target, W_obj_target_inv);
			Mul3x4(W_obj_target_inv, baseTargetWrist, T_wrist_in_obj);
			Mul3x4(W_obj_actual, T_wrist_in_obj, W_wrist_at_obj);

			const float posDeflect = VecLen3(fb.actualEntPos - fb.targetEntPos);
			const float rotDeflectDeg = ComputeRotationDeltaDeg(W_obj_actual, W_obj_target);
			const float alphaPos = Smoothstep01(kHeldBlendMinPos, kHeldBlendFullPos, posDeflect);
			const bool contactActive = fb.isWallBlocked || (posDeflect > kHeldBlendMinPos);
			const float alphaRot = contactActive
				? Smoothstep01(kHeldBlendMinRotDeg, kHeldBlendFullRotDeg, rotDeflectDeg)
				: Smoothstep01(kHeldFreeAirMinRotDeg, kHeldFreeAirFullRotDeg, rotDeflectDeg);
			outAlpha = fb.isWallBlocked
				? std::max(0.85f, std::max(alphaPos, alphaRot))
				: std::max(alphaPos, alphaRot);

			if (outAlpha <= 0.001f)
			{
				outCoupledTargetWrist = rawWrist;
				outAlpha = 0.0f;
				return;
			}

			const Vector rawPos(rawWrist[0][3], rawWrist[1][3], rawWrist[2][3]);
			const Vector atObjPos(W_wrist_at_obj[0][3], W_wrist_at_obj[1][3], W_wrist_at_obj[2][3]);
			const Vector delta = ClampVectorMaxLength((atObjPos - rawPos) * outAlpha, kMaxTetherDist);
			const Vector desiredPos = rawPos + delta;
			SlerpOrtho3x4(rawWrist, W_wrist_at_obj, outAlpha, desiredPos, outCoupledTargetWrist);
		}

		inline float ExtractExactTracePlaneDist(const CGameTrace& tr, const Vector& unitNormal, float boxHalfExtent)
		{
			const float bevel = boxHalfExtent * (std::fabs(unitNormal.x) + std::fabs(unitNormal.y) + std::fabs(unitNormal.z));
			const float endDist = DotProduct(tr.endpos, unitNormal) - bevel;
			const float rawPlaneLenSq = DotProduct(tr.plane.normal, tr.plane.normal);
			if (rawPlaneLenSq > 0.5f)
			{
				const float diff = endDist - tr.plane.dist;
				// Strip Source Engine BSP CM_ClipBoxToBrush DIST_EPSILON (0.03125u = 0.72 mm) when plane.dist is populated.
				if (diff > 1e-4f && diff < 0.06f)
					return tr.plane.dist;
			}
			return endDist;
		}

		inline void RunZeroedBoxTrace(
			IEngineTrace* engineTrace,
			CTraceFilter& filter,
			const Vector& start,
			const Vector& end,
			float r,
			unsigned int mask,
			CGameTrace& tr)
		{
			std::memset(&tr, 0, sizeof(tr));
			tr.fraction = 1.0f;
			if (!engineTrace)
				return;
			Ray_t ray;
			std::memset(&ray, 0, sizeof(ray));
			if (r > 0.0f)
				ray.Init(start, end, Vector(-r, -r, -r), Vector(r, r, r));
			else
				ray.Init(start, end);
			engineTrace->TraceRay(ray, mask, &filter, &tr);
		}

		Vector SweepHandPalmHullWithSlide(
			IEngineTrace* engineTrace,
			CHandWorldPropTraceFilter& filter,
			const Vector& headAnchor,
			const Vector& safeShoulder,
			const Vector& elbowAnchor,
			const Vector& desiredPalm,
			float hullRadius,
			unsigned int traceMask)
		{
			if (!engineTrace)
				return desiredPalm;

			auto runBoxTrace = [&](const Vector& start, const Vector& end, float r, unsigned int mask, CGameTrace& tr)
			{
				RunZeroedBoxTrace(engineTrace, filter, start, end, r, mask, tr);
			};

			auto resolveHitSlide = [&](const Vector& hitPos, const Vector& rawNormal) -> Vector
			{
				const float n1Len = VecLen3(rawNormal);
				if (n1Len <= 0.5f)
					return hitPos;
				const Vector n1 = rawNormal * (1.0f / n1Len);
				const Vector rem = desiredPalm - hitPos;
				const float vn = DotProduct(rem, n1);
				if (vn >= 0.0f)
					return hitPos;

				const Vector slideRem = rem - n1 * vn;
				if (VecLen3(slideRem) <= 0.05f)
					return hitPos;

				Vector slideStart = hitPos + n1 * 0.015f;
				CGameTrace trSlide;
				runBoxTrace(slideStart, hitPos + slideRem, hullRadius, traceMask, trSlide);
				if (trSlide.startsolid)
				{
					slideStart = hitPos;
					runBoxTrace(slideStart, hitPos + slideRem, hullRadius, traceMask, trSlide);
				}
				if (trSlide.startsolid)
					return hitPos;
				if (trSlide.fraction >= 1.0f)
					return trSlide.endpos;

				const Vector hit2 = trSlide.endpos;
				const float n2Len = VecLen3(trSlide.plane.normal);
				if (n2Len > 0.5f)
				{
					const Vector n2 = trSlide.plane.normal * (1.0f / n2Len);
					Vector crease = CrossProduct(n1, n2);
					const float cLen = VecLen3(crease);
					if (cLen > 0.1f)
					{
						crease = crease * (1.0f / cLen);
						const float proj = DotProduct((desiredPalm - hit2), crease);
						if (std::fabs(proj) > 0.05f)
						{
							const Vector creaseEnd = hit2 + crease * proj;
							Vector creaseStart = hit2 + (n1 + n2) * 0.015f;
							CGameTrace trCrease;
							runBoxTrace(creaseStart, creaseEnd, hullRadius, traceMask, trCrease);
							if (trCrease.startsolid)
								runBoxTrace(hit2, creaseEnd, hullRadius, traceMask, trCrease);
							if (!trCrease.startsolid)
								return trCrease.endpos;
						}
					}
				}
				return hit2;
			};

			CGameTrace trElbow;
			runBoxTrace(safeShoulder, elbowAnchor, hullRadius, traceMask, trElbow);
			if (!trElbow.startsolid)
			{
				if (trElbow.fraction < 1.0f)
					return resolveHitSlide(trElbow.endpos, trElbow.plane.normal);

				CGameTrace trPalm;
				runBoxTrace(elbowAnchor, desiredPalm, hullRadius, traceMask, trPalm);
				if (!trPalm.startsolid)
				{
					if (trPalm.fraction >= 1.0f)
						return desiredPalm;
					return resolveHitSlide(trPalm.endpos, trPalm.plane.normal);
				}
			}

			CGameTrace trHeadHull;
			runBoxTrace(headAnchor, desiredPalm, hullRadius, traceMask, trHeadHull);
			if (!trHeadHull.startsolid)
			{
				if (trHeadHull.fraction >= 1.0f)
					return desiredPalm;
				return resolveHitSlide(trHeadHull.endpos, trHeadHull.plane.normal);
			}

			CGameTrace trLine;
			runBoxTrace(headAnchor, desiredPalm, 0.0f, traceMask, trLine);
			if (!trLine.startsolid && trLine.fraction < 1.0f)
				return trLine.endpos;

			return desiredPalm;
		}

		struct PalmContactResult
		{
			Vector resolvedPalm{ 0.0f, 0.0f, 0.0f };
			Vector hardSafePalm{ 0.0f, 0.0f, 0.0f };
			bool hadPalmContact = false;
			bool hadFingerPushback = false;
			Vector contactNormal{ 0.0f, 0.0f, 0.0f };
			float palmarSurfaceDist = 999.0f;
			Vector palmarSurfaceNormal{ 0.0f, 0.0f, 0.0f };
		};

		inline bool TestPhalanxSegment(
			IEngineTrace* engineTrace,
			CHandWorldPropTraceFilter& filter,
			const Vector& P,
			const Vector& Q,
			float r,
			unsigned int mask,
			const Vector* fallbackNormal = nullptr,
			Vector* outHitNormal = nullptr)
		{
			const Vector d = Q - P;
			const float segLen = VecLen3(d);
			Vector startPt = P;
			if (segLen > 1e-4f)
			{
				const float skipFrac = std::min(0.28f, 0.26f / segLen);
				startPt = P + d * skipFrac;
			}
			CGameTrace tr;
			RunZeroedBoxTrace(engineTrace, filter, startPt, Q, r, mask, tr);
			if (tr.DidHit())
			{
				if (outHitNormal)
				{
					if (VecLen3(tr.plane.normal) > 0.5f)
						*outHitNormal = NormalizeSafe(tr.plane.normal, Vector(0.0f, 0.0f, 1.0f));
					else if (fallbackNormal && VecLen3(*fallbackNormal) > 0.5f)
						*outHitNormal = *fallbackNormal;
					else
						*outHitNormal = NormalizeSafe(P - Q, Vector(0.0f, 0.0f, 1.0f));
				}
				return true;
			}
			return false;
		}

		inline void EvalFingerWorldChainFromWrist(
			const matrix3x4_t& wrist,
			const float openFinger[4][3][4],
			float tipExtent,
			const float deg[4],
			float splayDeg,
			matrix3x4_t outW[4],
			Vector outPts[5])
		{
			matrix3x4_t W_parent = wrist;
			for (int k = 0; k < 4; ++k)
			{
				matrix3x4_t T_base{}, R_delta{}, T_kin{};
				std::memcpy(&T_base, openFinger[k], sizeof(matrix3x4_t));
				BuildJointDeltaRotation(deg[k], (k == 1) ? splayDeg : 0.0f, R_delta);
				Mul3x4(T_base, R_delta, T_kin);
				Mul3x4(W_parent, T_kin, outW[k]);
				outPts[k] = Vector(outW[k][0][3], outW[k][1][3], outW[k][2][3]);
				W_parent = outW[k];
			}
			outPts[4] = TransformPoint3x4(outW[3], tipExtent, 0.0f, 0.0f);
		}

		inline void EvalMirroredPhoneLeftFingerFromWrist(
			const matrix3x4_t& wrist,
			int fingerSlot,
			const float deg[4],
			float leftSplayDeg,
			Vector outPts[5])
		{
			const matrix3x4_t ident(
				1.0f, 0.0f, 0.0f, 0.0f,
				0.0f, 1.0f, 0.0f, 0.0f,
				0.0f, 0.0f, 1.0f, 0.0f);
			matrix3x4_t wLocal[4]{};
			Vector localPts[5]{};
			// Bare Left Hand on vm1 renders s_OpenPhoneRightFingers animated with right-hand splay (-leftSplayDeg)
			// and reflected across Z = 0 in wrist-local space (matches DrawModelExecute exactly).
			EvalFingerWorldChainFromWrist(ident, s_OpenPhoneRightFingers[fingerSlot], kRightTipExtent[fingerSlot + 1], deg, -leftSplayDeg, wLocal, localPts);
			for (int i = 0; i < 5; ++i)
			{
				outPts[i] = TransformPoint3x4(wrist, localPts[i].x, localPts[i].y, -localPts[i].z);
			}
		}

		inline void EvalThumbWorldChainFromWrist(
			const matrix3x4_t& wrist,
			const float openThumb[3][3][4],
			float tipExtent,
			float thumbPalmarSign,
			float baseThumbSplay,
			const float cArr[3],
			const float pArr[2],
			matrix3x4_t outW[3],
			Vector outPts[4])
		{
			matrix3x4_t W_parent = wrist;
			const float splays[3] = {
				thumbPalmarSign * pArr[0],
				baseThumbSplay + thumbPalmarSign * pArr[1],
				0.0f
			};
			for (int k = 0; k < 3; ++k)
			{
				matrix3x4_t T_base{}, R_delta{}, T_kin{};
				std::memcpy(&T_base, openThumb[k], sizeof(matrix3x4_t));
				BuildJointDeltaRotation(cArr[k], splays[k], R_delta);
				Mul3x4(T_base, R_delta, T_kin);
				Mul3x4(W_parent, T_kin, outW[k]);
				outPts[k] = Vector(outW[k][0][3], outW[k][1][3], outW[k][2][3]);
				W_parent = outW[k];
			}
			outPts[3] = TransformPoint3x4(outW[2], tipExtent, 0.0f, 0.0f);
		}

		bool PushPalmForLimitLockedFingers(
			IEngineTrace* engineTrace,
			CHandWorldPropTraceFilter& fingerFilter,
			const Vector& rawPalmPos,
			const matrix3x4_t& rawWrist,
			HandSkinPreset preset,
			bool usePhoneOpen,
			bool hadPalmContact,
			const Vector& palmContactNormal,
			const float curls5[5],
			const float splays4[4],
			unsigned int traceMask,
			Vector& ioPalmPos)
		{
			const bool isLeftMesh = (preset == HandSkinPreset::LeftLight || preset == HandSkinPreset::LeftSuper || preset == HandSkinPreset::LeftPhone);
			const auto& openThumb = isLeftMesh
				? (usePhoneOpen ? s_OpenPhoneLeftThumb  : s_OpenCamLeftThumb)
				: (usePhoneOpen ? s_OpenPhoneRightThumb : s_OpenCamRightThumb);
			const auto& openFingers = isLeftMesh
				? (usePhoneOpen ? s_OpenPhoneLeftFingers  : s_OpenCamLeftFingers)
				: (usePhoneOpen ? s_OpenPhoneRightFingers : s_OpenCamRightFingers);
			const float (&tipExtents)[5] = isLeftMesh ? kLeftTipExtent : kRightTipExtent;

			const Vector rawWristPos(rawWrist[0][3], rawWrist[1][3], rawWrist[2][3]);
			const Vector wristDistal = NormalizeSafe(
				Vector(rawWrist[0][0], rawWrist[1][0], rawWrist[2][0]), Vector(1.0f, 0.0f, 0.0f));
			const Vector initShift = ioPalmPos - rawPalmPos;
			const Vector initWristPos = rawWristPos + initShift;
			const Vector broadCenter = initWristPos + wristDistal * 4.5f;

			Ray_t broadRay;
			std::memset(&broadRay, 0, sizeof(broadRay));
			broadRay.Init(broadCenter, broadCenter, Vector(-5.5f, -5.5f, -5.5f), Vector(5.5f, 5.5f, 5.5f));
			CGameTrace trBroad;
			std::memset(&trBroad, 0, sizeof(trBroad));
			trBroad.fraction = 1.0f;
			engineTrace->TraceRay(broadRay, traceMask, &fingerFilter, &trBroad);
			if (!trBroad.DidHit() && !hadPalmContact)
				return false;

			const Vector preFingerPalm = ioPalmPos;
			const Vector* fallbackN = hadPalmContact ? &palmContactNormal : nullptr;
			const float baseSplaySign = isLeftMesh ? 1.0f : -1.0f;
			const float thumbPalmarSign = isLeftMesh ? 1.0f : -1.0f;
			const float cThumb = ClampF(curls5[0], 0.0f, 1.0f);
			const float baseThumbSplay = (isLeftMesh ? -1.0f : 1.0f) * splays4[0] * 10.0f;

			for (int pass = 0; pass < 2; ++pass)
			{
				const Vector passStart = ioPalmPos;
				const Vector palmShift = passStart - rawPalmPos;
				matrix3x4_t candWrist = rawWrist;
				candWrist[0][3] += palmShift.x;
				candWrist[1][3] += palmShift.y;
				candWrist[2][3] += palmShift.z;

				struct FingerConstraint
				{
					Vector n;
					float reqPalmDist;
				};
				FingerConstraint fConstraints[8]{};
				int fConstraintCount = 0;

				auto addFConstraint = [&](const Vector& n, float reqDist)
				{
					for (int i = 0; i < fConstraintCount; ++i)
					{
						if (DotProduct(fConstraints[i].n, n) > 0.96f)
						{
							if (reqDist > fConstraints[i].reqPalmDist)
							{
								fConstraints[i].n = n;
								fConstraints[i].reqPalmDist = reqDist;
							}
							return;
						}
					}
					if (fConstraintCount < 8)
					{
						fConstraints[fConstraintCount].n = n;
						fConstraints[fConstraintCount].reqPalmDist = reqDist;
						++fConstraintCount;
					}
				};

				auto accumulateChainPushback = [&](const Vector* chainPts, const float* chainRadii, int numSegs)
				{
					for (int s = 0; s < numSegs; ++s)
					{
						CGameTrace tr;
						RunZeroedBoxTrace(engineTrace, fingerFilter, chainPts[s], chainPts[s + 1], chainRadii[s], traceMask, tr);
						if (tr.startsolid)
							break;
						if (tr.fraction < 1.0f && VecLen3(tr.plane.normal) > 0.5f)
						{
							const Vector n = NormalizeSafe(tr.plane.normal, Vector(0.0f, 0.0f, 1.0f));
							const float dSurf = ExtractExactTracePlaneDist(tr, n, chainRadii[s]);
							float maxPen = (dSurf + chainRadii[s] + kTargetSkinClearance) - DotProduct(chainPts[s + 1], n);
							for (int j = s + 2; j <= numSegs; ++j)
							{
								CGameTrace trPt;
								RunZeroedBoxTrace(engineTrace, fingerFilter, chainPts[j], chainPts[j], chainRadii[j - 1], traceMask, trPt);
								if (trPt.startsolid)
								{
									const float candPen = (dSurf + chainRadii[j - 1] + kTargetSkinClearance) - DotProduct(chainPts[j], n);
									if (candPen > maxPen)
										maxPen = candPen;
								}
							}
							if (maxPen > 1e-4f)
							{
								maxPen = std::min(maxPen, 4.2f);
								addFConstraint(n, DotProduct(passStart, n) + maxPen);
							}
							break;
						}
					}
				};

				// 4 Fingers (Index..Pinky)
				for (int f = 0; f < 4; ++f)
				{
					const float cUser = ClampF(curls5[f + 1], 0.0f, 1.0f);
					const float baseSplay = baseSplaySign * (splays4[f] * 10.0f + (0.5f - cUser) * kSplayWeights[f] * 4.0f);
					float userDeg[4]{};
					EvalBareFingerCurlDeg(f, cUser, userDeg);

					matrix3x4_t uW[4]{};
					Vector uPts[5]{};
					EvalFingerWorldChainFromWrist(candWrist, openFingers[f], tipExtents[f + 1], userDeg, baseSplay, uW, uPts);

					int firstHitSeg = -1;
					Vector firstHitN(0.0f, 0.0f, 1.0f);
					for (int seg = 0; seg < 3; ++seg)
					{
						if (TestPhalanxSegment(engineTrace, fingerFilter, uPts[seg + 1], uPts[seg + 2], kFingerPhalanxRadii[f][seg], traceMask, fallbackN, &firstHitN))
						{
							firstHitSeg = seg;
							break;
						}
					}
					if (firstHitSeg < 0)
						continue;

					float limitDeg[4]{ userDeg[0], userDeg[1], userDeg[2], userDeg[3] };
					if (cUser < 0.70f)
					{
						const matrix3x4_t& W_hit = uW[firstHitSeg + 1];
						const Vector tCurl(W_hit[0][1], W_hit[1][1], W_hit[2][1]);
						if (DotProduct(tCurl, firstHitN) < 0.15f)
						{
							for (int k = 0; k < 4; ++k)
								limitDeg[k] = kDorsalBioMinDeg[k];
						}
						else
						{
							EvalBareFingerCurlDeg(f, 1.0f, limitDeg);
						}
					}

					matrix3x4_t lW[4]{};
					Vector lPts[5]{};
					EvalFingerWorldChainFromWrist(candWrist, openFingers[f], tipExtents[f + 1], limitDeg, baseSplay, lW, lPts);

					const Vector chainPts[5] = { passStart, lPts[1], lPts[2], lPts[3], lPts[4] };
					const float chainRadii[4] = { 0.34f, kFingerPhalanxRadii[f][0], kFingerPhalanxRadii[f][1], kFingerPhalanxRadii[f][2] };
					accumulateChainPushback(chainPts, chainRadii, 4);
				}

				// Thumb
				{
					float tCurl[3]{};
					EvalBareThumbCurlDeg(cThumb, tCurl, isLeftMesh);
					constexpr float kZeroPitch[2] = { 0.0f, 0.0f };
					matrix3x4_t uTW[3]{};
					Vector uTPts[4]{};
					EvalThumbWorldChainFromWrist(candWrist, openThumb, tipExtents[0], thumbPalmarSign, baseThumbSplay, tCurl, kZeroPitch, uTW, uTPts);

					int firstThumbSeg = -1;
					Vector firstThumbHitN(0.0f, 0.0f, 1.0f);
					for (int seg = 0; seg < 3; ++seg)
					{
						if (TestPhalanxSegment(engineTrace, fingerFilter, uTPts[seg], uTPts[seg + 1], kThumbPhalanxRadii[seg], traceMask, fallbackN, &firstThumbHitN))
						{
							firstThumbSeg = seg;
							break;
						}
					}
					if (firstThumbSeg >= 0)
					{
						float limitPitch[2] = { 0.0f, 0.0f };
						if (cThumb < 0.70f)
						{
							const matrix3x4_t& W_tHit = uTW[std::min(firstThumbSeg, 1)];
							const Vector palmarDir(
								W_tHit[0][2] * (-thumbPalmarSign),
								W_tHit[1][2] * (-thumbPalmarSign),
								W_tHit[2][2] * (-thumbPalmarSign));
							if (DotProduct(palmarDir, firstThumbHitN) < 0.15f)
							{
								limitPitch[0] = -13.0f;
								limitPitch[1] = -9.0f;
							}
							else
							{
								limitPitch[0] = 24.0f;
								limitPitch[1] = 26.0f;
							}
						}

						matrix3x4_t lTW[3]{};
						Vector lTPts[4]{};
						EvalThumbWorldChainFromWrist(candWrist, openThumb, tipExtents[0], thumbPalmarSign, baseThumbSplay, tCurl, limitPitch, lTW, lTPts);

						const Vector chainPts[4] = { passStart, lTPts[1], lTPts[2], lTPts[3] };
						const float chainRadii[3] = { kThumbPhalanxRadii[0], kThumbPhalanxRadii[1], kThumbPhalanxRadii[2] };
						accumulateChainPushback(chainPts, chainRadii, 3);
					}
				}

				if (fConstraintCount == 0)
					break;

				for (int iter = 0; iter < 4; ++iter)
				{
					bool anyMoved = false;
					for (int i = 0; i < fConstraintCount; ++i)
					{
						const float pen = fConstraints[i].reqPalmDist - DotProduct(ioPalmPos, fConstraints[i].n);
						if (pen > 1e-4f)
						{
							ioPalmPos = ioPalmPos + fConstraints[i].n * pen;
							anyMoved = true;
						}
					}
					if (!anyMoved)
						break;
				}

				if (VecLen3(ioPalmPos - passStart) <= 1e-4f)
					break;
			}

			ioPalmPos = preFingerPalm + ClampVectorMaxLength(ioPalmPos - preFingerPalm, 4.5f);
			if (VecLen3(ioPalmPos - preFingerPalm) > 1e-3f)
			{
				CGameTrace trFVerify;
				RunZeroedBoxTrace(engineTrace, fingerFilter, preFingerPalm, ioPalmPos, 0.24f, traceMask, trFVerify);
				if (!trFVerify.startsolid && trFVerify.fraction < 1.0f)
					ioPalmPos = trFVerify.endpos;
			}

			return (VecLen3(ioPalmPos - preFingerPalm) > 0.015f);
		}

		// Two-layer Layer-1 palm solver for open VR hands:
		// 1) Continuous stateful core sweep (prevVisualPalm -> rawPalmPos, r = 0.44u) with 2-plane wall/crease slide
		//    and shoulder/head recovery fallback (no synthetic elbow hook catching desk edges).
		// 2) Broadphase + 8 local anatomical palm-patch probes (palm pad ONLY, zero finger vertices) so resting
		//    on narrow railings or curved desk mice places the inner palm skin flush without phantom pushback.
		// 2B) Finger-limit anti-penetration pushback so bare fingers at their bend limits cannot sink into objects.
		// 3) Palmar proximity query against fingerFilter for Layer-2 finger surface draping.
		PalmContactResult ResolveOpenHandPalmContact(
			IEngineTrace* engineTrace,
			CHandWorldPropTraceFilter& palmFilter,
			CHandWorldPropTraceFilter& fingerFilter,
			const Vector& headAnchor,
			const Vector& safeShoulder,
			const Vector& rawPalmPos,
			const matrix3x4_t& rawWrist,
			HandSkinPreset preset,
			const float curls5[5],
			const float splays4[4],
			bool isBareHand,
			bool usePhoneOpen,
			bool hasPrevVisualPalm,
			const Vector& prevVisualPalm,
			unsigned int traceMask)
		{
			PalmContactResult out{};
			out.resolvedPalm = rawPalmPos;
			out.hardSafePalm = rawPalmPos;
			if (!engineTrace)
				return out;

			// Step 1: Continuous stateful core sweep & 2-plane slide
			auto sweepAndSlideCore = [&](const Vector& startPt, Vector& outPalm, bool& outHit, Vector& outN) -> bool
			{
				CGameTrace tr;
				RunZeroedBoxTrace(engineTrace, palmFilter, startPt, rawPalmPos, kPalmCoreRadius, traceMask, tr);
				if (tr.startsolid)
					return false;
				if (tr.fraction >= 1.0f)
				{
					outPalm = rawPalmPos;
					outHit = false;
					outN = Vector(0.0f, 0.0f, 0.0f);
					return true;
				}

				const Vector hit1 = tr.endpos;
				if (VecLen3(tr.plane.normal) <= 0.5f)
				{
					outPalm = hit1;
					outHit = false;
					outN = Vector(0.0f, 0.0f, 0.0f);
					return true;
				}

				const Vector n1 = NormalizeSafe(tr.plane.normal, Vector(0.0f, 0.0f, 1.0f));
				outHit = true;
				outN = n1;

				const Vector rem = rawPalmPos - hit1;
				const float vn = DotProduct(rem, n1);
				if (vn >= 0.0f)
				{
					outPalm = hit1;
					return true;
				}

				const Vector slideRem = rem - n1 * vn;
				if (VecLen3(slideRem) <= 0.04f)
				{
					outPalm = hit1;
					return true;
				}

				CGameTrace trSlide;
				RunZeroedBoxTrace(engineTrace, palmFilter, hit1 + n1 * 0.015f, hit1 + slideRem, kPalmCoreRadius, traceMask, trSlide);
				if (trSlide.startsolid)
					RunZeroedBoxTrace(engineTrace, palmFilter, hit1, hit1 + slideRem, kPalmCoreRadius, traceMask, trSlide);

				if (trSlide.startsolid)
				{
					outPalm = hit1;
					return true;
				}
				if (trSlide.fraction >= 1.0f)
				{
					outPalm = trSlide.endpos;
					return true;
				}

				const Vector hit2 = trSlide.endpos;
				if (VecLen3(trSlide.plane.normal) > 0.5f)
				{
					const Vector n2 = NormalizeSafe(trSlide.plane.normal, Vector(0.0f, 0.0f, 1.0f));
					Vector crease = CrossProduct(n1, n2);
					const float cLen = VecLen3(crease);
					if (cLen > 0.1f)
					{
						crease = crease * (1.0f / cLen);
						const float proj = DotProduct(rawPalmPos - hit2, crease);
						if (std::fabs(proj) > 0.04f)
						{
							const Vector creaseEnd = hit2 + crease * proj;
							CGameTrace trCrease;
							RunZeroedBoxTrace(engineTrace, palmFilter, hit2 + (n1 + n2) * 0.015f, creaseEnd, kPalmCoreRadius, traceMask, trCrease);
							if (trCrease.startsolid)
								RunZeroedBoxTrace(engineTrace, palmFilter, hit2, creaseEnd, kPalmCoreRadius, traceMask, trCrease);
							if (!trCrease.startsolid)
							{
								outPalm = trCrease.endpos;
								return true;
							}
						}
					}
				}
				outPalm = hit2;
				return true;
			};

			Vector palmPos = rawPalmPos;
			bool coreOk = false;
			if (hasPrevVisualPalm && VecLen3(rawPalmPos - prevVisualPalm) <= 14.0f)
				coreOk = sweepAndSlideCore(prevVisualPalm, palmPos, out.hadPalmContact, out.contactNormal);
			if (!coreOk)
				coreOk = sweepAndSlideCore(safeShoulder, palmPos, out.hadPalmContact, out.contactNormal);
			if (!coreOk)
				coreOk = sweepAndSlideCore(headAnchor, palmPos, out.hadPalmContact, out.contactNormal);
			if (!coreOk)
			{
				CGameTrace trLine;
				RunZeroedBoxTrace(engineTrace, palmFilter, headAnchor, rawPalmPos, 0.0f, traceMask, trLine);
				if (!trLine.startsolid && trLine.fraction < 1.0f)
				{
					palmPos = trLine.endpos;
					if (VecLen3(trLine.plane.normal) > 0.5f)
					{
						out.hadPalmContact = true;
						out.contactNormal = NormalizeSafe(trLine.plane.normal, Vector(0.0f, 0.0f, 1.0f));
					}
				}
			}

			out.hardSafePalm = palmPos;

			// Exact preset inner-palmar & dorsal skin thicknesses from palmLocal
			float palmSkinY = 0.76f;
			float dorsalSkinY = 0.75f;
			switch (preset)
			{
			case HandSkinPreset::RightPhone:
			case HandSkinPreset::LeftPhone:
				palmSkinY = 0.73f;
				dorsalSkinY = 0.75f;
				break;
			case HandSkinPreset::LeftLight:
			case HandSkinPreset::LeftSuper:
				palmSkinY = 0.90f;
				dorsalSkinY = 0.78f;
				break;
			case HandSkinPreset::RightCam:
			case HandSkinPreset::RightSuper:
			default:
				palmSkinY = 0.76f;
				dorsalSkinY = 0.75f;
				break;
			}

			const float meanCurl = 0.25f * (curls5[1] + curls5[2] + curls5[3] + curls5[4]);
			const float fistFactor = Smoothstep01(0.65f, 0.90f, meanCurl);
			const float effPalmY = palmSkinY + 0.42f * fistFactor;
			const float distalX  = 1.25f + 0.55f * fistFactor;
			const float sideZ    = 1.25f;
			const bool isLeftMesh = (preset == HandSkinPreset::LeftLight || preset == HandSkinPreset::LeftSuper || preset == HandSkinPreset::LeftPhone);
			const float thumbSign = isLeftMesh ? -1.0f : 1.0f;

			// Step 2: Stationary broadphase + local anatomical palm-patch refinement
			Ray_t broadRay;
			std::memset(&broadRay, 0, sizeof(broadRay));
			broadRay.Init(palmPos, palmPos, Vector(-2.4f, -2.4f, -2.4f), Vector(2.4f, 2.4f, 2.4f));
			CGameTrace trBroad;
			std::memset(&trBroad, 0, sizeof(trBroad));
			trBroad.fraction = 1.0f;
			engineTrace->TraceRay(broadRay, traceMask, &palmFilter, &trBroad);

			if (trBroad.DidHit() || out.hadPalmContact)
			{
				struct LocalConstraint
				{
					Vector n;
					float reqPalmDist;
				};
				LocalConstraint constraints[6]{};
				int constraintCount = 0;

				auto addConstraint = [&](const Vector& n, float reqDist)
				{
					for (int i = 0; i < constraintCount; ++i)
					{
						if (DotProduct(constraints[i].n, n) > 0.985f)
						{
							if (reqDist > constraints[i].reqPalmDist)
							{
								constraints[i].n = n;
								constraints[i].reqPalmDist = reqDist;
							}
							return;
						}
					}
					if (constraintCount < 6)
					{
						constraints[constraintCount].n = n;
						constraints[constraintCount].reqPalmDist = reqDist;
						++constraintCount;
					}
				};

				constexpr float kProbeRadius = 0.10f;
				for (int pass = 0; pass < 2; ++pass)
				{
					const Vector passStartPalm = palmPos;
					const int prevCount = constraintCount;

					struct LocalProbeSpec
					{
						Vector localA;
						Vector localB;
						bool hasAnchor;
					};
					const LocalProbeSpec specs[8] = {
						{ Vector(0.0f, 0.0f, 0.0f),              Vector(0.0f,  effPalmY, 0.0f),            false }, // Center palmar
						{ Vector(-0.85f, 0.0f, 0.0f),            Vector(-0.85f, effPalmY * 0.95f, 0.0f),   true  }, // Heel palmar
						{ Vector( 0.95f, 0.0f,  0.60f * thumbSign), Vector( 0.95f, effPalmY * 0.95f,  0.60f * thumbSign), true }, // Radial-knuckle palmar
						{ Vector( 0.95f, 0.0f, -0.60f * thumbSign), Vector( 0.95f, effPalmY * 0.95f, -0.60f * thumbSign), true }, // Ulnar-knuckle palmar
						{ Vector(0.0f, 0.0f, 0.0f),              Vector(0.0f, -dorsalSkinY, 0.0f),         false }, // Center dorsal
						{ Vector(0.0f, 0.0f, 0.0f),              Vector(distalX, 0.0f, 0.0f),              false }, // Distal knuckle ridge
						{ Vector(0.0f, 0.0f, 0.0f),              Vector(0.0f, 0.0f,  sideZ * thumbSign),   false }, // Radial palm edge
						{ Vector(0.0f, 0.0f, 0.0f),              Vector(0.0f, 0.0f, -sideZ * thumbSign),   false }  // Ulnar palm edge
					};

					for (int si = 0; si < 8; ++si)
					{
						Vector segStart = passStartPalm;
						Vector segEnd = passStartPalm + RotateVec3x4(rawWrist, specs[si].localB);

						if (specs[si].hasAnchor)
						{
							const Vector anchorWorld = passStartPalm + RotateVec3x4(rawWrist, specs[si].localA);
							CGameTrace trA;
							RunZeroedBoxTrace(engineTrace, palmFilter, passStartPalm, anchorWorld, kProbeRadius, traceMask, trA);
							if (trA.startsolid)
								continue;
							if (trA.fraction < 1.0f)
							{
								if (VecLen3(trA.plane.normal) > 0.5f)
								{
									const Vector n = NormalizeSafe(trA.plane.normal, Vector(0.0f, 0.0f, 1.0f));
									const float dSurf = ExtractExactTracePlaneDist(trA, n, kProbeRadius);
									const float pen = (dSurf + kTargetSkinClearance) - DotProduct(anchorWorld, n);
									if (pen > 1e-4f)
									{
										const float maxPush = std::max(VecLen3(anchorWorld - passStartPalm), 0.15f);
										addConstraint(n, DotProduct(passStartPalm, n) + std::min(pen, maxPush));
									}
								}
								continue;
							}
							segStart = anchorWorld;
						}

						CGameTrace trB;
						RunZeroedBoxTrace(engineTrace, palmFilter, segStart, segEnd, kProbeRadius, traceMask, trB);
						if (!trB.startsolid && trB.fraction < 1.0f && VecLen3(trB.plane.normal) > 0.5f)
						{
							const Vector n = NormalizeSafe(trB.plane.normal, Vector(0.0f, 0.0f, 1.0f));
							const float dSurf = ExtractExactTracePlaneDist(trB, n, kProbeRadius);
							const float pen = (dSurf + kTargetSkinClearance) - DotProduct(segEnd, n);
							if (pen > 1e-4f)
							{
								const float maxPush = std::max(VecLen3(segEnd - segStart), 0.15f);
								addConstraint(n, DotProduct(passStartPalm, n) + std::min(pen, maxPush));
							}
						}
					}

					if (constraintCount == 0)
						break;

					for (int iter = 0; iter < 4; ++iter)
					{
						bool anyMoved = false;
						for (int i = 0; i < constraintCount; ++i)
						{
							const float pen = constraints[i].reqPalmDist - DotProduct(palmPos, constraints[i].n);
							if (pen > 1e-4f)
							{
								palmPos = palmPos + constraints[i].n * pen;
								anyMoved = true;
							}
						}
						if (!anyMoved)
							break;
					}

					if (constraintCount == prevCount && VecLen3(palmPos - passStartPalm) <= 1e-4f)
						break;
				}

				const Vector step2Delta = palmPos - out.hardSafePalm;
				const float step2Len = VecLen3(step2Delta);
				if (step2Len > 1.6f)
					palmPos = out.hardSafePalm + step2Delta * (1.6f / step2Len);

				if (VecLen3(palmPos - out.hardSafePalm) > 1e-3f)
				{
					CGameTrace trVerify;
					RunZeroedBoxTrace(engineTrace, palmFilter, out.hardSafePalm, palmPos, 0.24f, traceMask, trVerify);
					if (!trVerify.startsolid && trVerify.fraction < 1.0f)
						palmPos = trVerify.endpos;
				}

				const Vector netStep2 = palmPos - out.hardSafePalm;
				if (VecLen3(netStep2) > 0.02f && constraintCount > 0)
				{
					int bestIdx = 0;
					float bestProj = -1e30f;
					for (int i = 0; i < constraintCount; ++i)
					{
						const float proj = DotProduct(netStep2, constraints[i].n);
						if (proj > bestProj)
						{
							bestProj = proj;
							bestIdx = i;
						}
					}
					if (bestProj > 0.005f)
					{
						out.hadPalmContact = true;
						out.contactNormal = constraints[bestIdx].n;
					}
				}
			}

			// Step 2B: Finger-limit anti-penetration wrist pushback so bare fingers stop on surfaces
			if (isBareHand)
			{
				out.hadFingerPushback = PushPalmForLimitLockedFingers(
					engineTrace,
					fingerFilter,
					rawPalmPos,
					rawWrist,
					preset,
					usePhoneOpen,
					out.hadPalmContact,
					out.contactNormal,
					curls5,
					splays4,
					traceMask,
					palmPos);
			}

			out.resolvedPalm = palmPos;

			// Step 3: Palmar surface proximity query (used by Layer-2 finger draping)
			const Vector palmOutwardNormal = NormalizeSafe(
				Vector(rawWrist[0][1], rawWrist[1][1], rawWrist[2][1]), Vector(0.0f, 0.0f, 1.0f));
			const Vector proxOffsets[3] = {
				Vector(0.0f, 0.0f, 0.0f),
				RotateVec3x4(rawWrist, Vector(-0.80f, 0.0f, 0.0f)),
				RotateVec3x4(rawWrist, Vector( 0.90f, 0.0f, 0.0f))
			};
			constexpr float kProxRadius = 0.16f;
			const float raySpan = effPalmY + 1.85f;
			for (int pi = 0; pi < 3; ++pi)
			{
				const Vector a = palmPos + proxOffsets[pi];
				const Vector b = a + palmOutwardNormal * raySpan;
				CGameTrace trProx;
				RunZeroedBoxTrace(engineTrace, fingerFilter, a, b, kProxRadius, traceMask, trProx);
				if (!trProx.startsolid && trProx.fraction < 1.0f && VecLen3(trProx.plane.normal) > 0.5f)
				{
					const Vector n = NormalizeSafe(trProx.plane.normal, Vector(0.0f, 0.0f, 1.0f));
					const float denom = DotProduct(palmOutwardNormal, n);
					if (denom < -0.20f)
					{
						const float dSurf = ExtractExactTracePlaneDist(trProx, n, kProxRadius);
						const float hitAlongPalm = ClampF((dSurf - DotProduct(a, n)) / denom, 0.0f, raySpan);
						const float surfDist = std::max(0.0f, hitAlongPalm - effPalmY);
						if (surfDist < out.palmarSurfaceDist)
						{
							out.palmarSurfaceDist = surfDist;
							out.palmarSurfaceNormal = n;
						}
					}
				}
			}

			return out;
		}

		// Layer 2: Two-stage per-finger / per-phalanx surface conformation solver.
		// Handles flat-surface uncurling + gentle finger splaying as well as curved downward
		// draping onto railings and desk items (e.g. Chapter 1 intro computer mouse).
		// Returns true iff at least one finger or thumb phalanx actually contacted surface geometry.
		bool SolveHandFingerSurfaceConformation(
			int handIdx,
			bool physicalLeft,
			bool isLeftMesh,
			bool usePhoneOpen,
			bool isHoldingThisHand,
			VR* vr,
			Game* game,
			CHandWorldPropTraceFilter& fingerFilter,
			const matrix3x4_t& visualWrist,
			const Vector& rawPalmPos,
			const Vector& palmLocal,
			const float liveCurls[5],
			const PalmContactResult& palmRes,
			float dt,
			unsigned long long nowMs)
		{
			const bool isGrabbingThisHand = isHoldingThisHand || (vr->m_IsHoldingObject && vr->m_HoldingWithLeftHand == physicalLeft);
			const bool isBareHand = isLeftMesh ? (!vr->m_FlashlightEquipped) : vr->m_RightHandEmpty;
			if (!vr->m_ShowHands || !game || !game->m_EngineTrace || isGrabbingThisHand || !isBareHand)
			{
				s_SurfaceFingerSlots[handIdx].Clear();
				s_FingerSmooth[handIdx] = VisualFingerSmoothState{};
				return false;
			}

			const Vector wristPos(visualWrist[0][3], visualWrist[1][3], visualWrist[2][3]);
			const Vector wristDistal = NormalizeSafe(
				Vector(visualWrist[0][0], visualWrist[1][0], visualWrist[2][0]), Vector(1.0f, 0.0f, 0.0f));

			// Fast broadphase AABB around posed fingers (0 per-finger traces in open air)
			const Vector broadCenter = wristPos + wristDistal * 4.5f;
			Ray_t broadRay;
			std::memset(&broadRay, 0, sizeof(broadRay));
			broadRay.Init(broadCenter, broadCenter, Vector(-5.5f, -5.5f, -5.5f), Vector(5.5f, 5.5f, 5.5f));
			CGameTrace trBroad;
			std::memset(&trBroad, 0, sizeof(trBroad));
			trBroad.fraction = 1.0f;
			game->m_EngineTrace->TraceRay(broadRay, kMaskOpenHandSolid, &fingerFilter, &trBroad);

			VisualFingerSmoothState& fSm = s_FingerSmooth[handIdx];
			if (!trBroad.DidHit() && !palmRes.hadPalmContact && palmRes.palmarSurfaceDist > 1.80f)
			{
				if (!fSm.initialized || fSm.pose.blendAlpha < 0.005f)
				{
					s_SurfaceFingerSlots[handIdx].Clear();
					fSm.initialized = false;
					fSm.pose.blendAlpha = 0.0f;
					fSm.smoothedSpreadAlpha = 0.0f;
					return false;
				}
				const float decay = std::exp(-dt * 18.0f);
				fSm.pose.blendAlpha *= decay;
				fSm.smoothedSpreadAlpha *= decay;
				if (fSm.pose.blendAlpha < 0.005f)
				{
					s_SurfaceFingerSlots[handIdx].Clear();
					fSm.initialized = false;
					fSm.pose.blendAlpha = 0.0f;
					fSm.smoothedSpreadAlpha = 0.0f;
				}
				else
				{
					s_SurfaceFingerSlots[handIdx].Write(fSm.pose, nowMs);
				}
				return false;
			}

			const auto& openThumb = isLeftMesh
				? (usePhoneOpen ? s_OpenPhoneLeftThumb  : s_OpenCamLeftThumb)
				: (usePhoneOpen ? s_OpenPhoneRightThumb : s_OpenCamRightThumb);
			const auto& openFingers = isLeftMesh
				? (usePhoneOpen ? s_OpenPhoneLeftFingers  : s_OpenCamLeftFingers)
				: (usePhoneOpen ? s_OpenPhoneRightFingers : s_OpenCamRightFingers);
			const float (&tipExtents)[5] = isLeftMesh ? kLeftTipExtent : kRightTipExtent;
			const auto& liveSplays = physicalLeft ? vr->m_LeftFingerSplay : vr->m_RightFingerSplay;

			const float drapeWeight = 1.0f - Smoothstep01(0.35f, 1.80f, palmRes.palmarSurfaceDist);

			const Vector* fallbackPalmN = palmRes.hadPalmContact ? &palmRes.contactNormal : nullptr;
			auto testPhalanxSegment = [&](const Vector& P, const Vector& Q, float r, Vector* outHitNormal = nullptr) -> bool
			{
				return TestPhalanxSegment(game->m_EngineTrace, fingerFilter, P, Q, r, kMaskOpenHandSolid, fallbackPalmN, outHitNormal);
			};

			auto testSweepChord = [&](const Vector& a, const Vector& b, float r) -> float
			{
				CGameTrace tr;
				RunZeroedBoxTrace(game->m_EngineTrace, fingerFilter, a, b, r, kMaskOpenHandSolid, tr);
				if (!tr.DidHit())
					return 2.0f;
				return tr.startsolid ? 0.0f : tr.fraction;
			};

			auto evalBiomechanicalCurl = [](int fingerSlot, float cBio, float outDeg[4])
			{
				if (cBio >= 0.0f)
				{
					EvalBareFingerCurlDeg(fingerSlot, cBio, outDeg);
				}
				else
				{
					const float t = ClampF(-cBio / 0.50f, 0.0f, 1.0f);
					for (int k = 0; k < 4; ++k)
						outDeg[k] = kBareFingerRestDeg[k] + t * (kDorsalBioMinDeg[k] - kBareFingerRestDeg[k]);
				}
			};

			auto evalFingerChain = [&](
				int fingerSlot,
				const float deg[4],
				float splayDeg,
				matrix3x4_t outW[4],
				Vector outPts[5])
			{
				EvalFingerWorldChainFromWrist(visualWrist, openFingers[fingerSlot], tipExtents[fingerSlot + 1], deg, splayDeg, outW, outPts);
			};

			HandSurfaceFingerPose targetPose{};
			float dorsalPushDeg[4]{ 0.0f, 0.0f, 0.0f, 0.0f };
			float palmarDrapeDeg[4]{ 0.0f, 0.0f, 0.0f, 0.0f };
			bool anyFingerDeviated = false;
			bool anyFingerSurfaceTouch = false;

			const float prevSpreadAlpha = fSm.initialized ? fSm.smoothedSpreadAlpha : 0.0f;
			const float baseSplaySign = isLeftMesh ? 1.0f : -1.0f;
			const float spreadSign = isLeftMesh ? -1.0f : 1.0f;

			static constexpr float kMinJointDeg[4]  = {  -5.0f, -18.0f,  -5.0f,  -7.0f };
			static constexpr float kMaxJointDeg[4]  = {  12.0f,  80.0f,  92.0f,  68.0f };
			static constexpr float kDrapeMaxDeg[4]  = {  12.0f,  58.0f,  68.0f,  48.0f };

			// 4 Fingers (Index, Middle, Ring, Pinky)
			for (int f = 0; f < 4; ++f)
			{
				const float cUser = ClampF(liveCurls[f + 1], 0.0f, 1.0f);
				const float baseSplay = baseSplaySign * (liveSplays[f] * 10.0f + (0.5f - cUser) * kSplayWeights[f] * 4.0f);
				const float workSplay = baseSplay + spreadSign * kSplayWeights[f] * kSurfaceSpreadDeg[f] * prevSpreadAlpha;

				float userDeg[4]{};
				evalBiomechanicalCurl(f, cUser, userDeg);

				float theta[4]{ userDeg[0], userDeg[1], userDeg[2], userDeg[3] };
				matrix3x4_t W_chain[4]{};
				Vector pts[5]{};
				evalFingerChain(f, theta, workSplay, W_chain, pts);

				// Stage 2A: Coupled whole-finger biomechanical uncurl / dorsal fold
				int firstHitSeg = -1;
				Vector firstHitN(0.0f, 0.0f, 1.0f);
				for (int seg = 0; seg < 3; ++seg)
				{
					if (testPhalanxSegment(pts[seg + 1], pts[seg + 2], kFingerPhalanxRadii[f][seg], &firstHitN))
					{
						firstHitSeg = seg;
						break;
					}
				}

				auto allSegsClearAtBio = [&](float cBio) -> bool
				{
					float d[4]{};
					evalBiomechanicalCurl(f, cBio, d);
					matrix3x4_t wTmp[4]{};
					Vector pTmp[5]{};
					evalFingerChain(f, d, workSplay, wTmp, pTmp);
					for (int seg = 0; seg < 3; ++seg)
					{
						if (testPhalanxSegment(pTmp[seg + 1], pTmp[seg + 2], kFingerPhalanxRadii[f][seg]))
							return false;
					}
					return true;
				};

				if (firstHitSeg >= 0)
				{
					anyFingerSurfaceTouch = true;
					const matrix3x4_t& W_hit = W_chain[firstHitSeg + 1];
					const Vector tCurl(W_hit[0][1], W_hit[1][1], W_hit[2][1]);
					if (DotProduct(tCurl, firstHitN) < 0.15f)
					{
						// Palmar/front obstacle (table, wall, mouse arch, railing): uncurl dorsally
						float lo = -0.50f;
						float hi = cUser;
						if (!allSegsClearAtBio(lo))
						{
							evalBiomechanicalCurl(f, lo, theta);
						}
						else
						{
							for (int it = 0; it < 6; ++it)
							{
								const float mid = 0.5f * (lo + hi);
								if (allSegsClearAtBio(mid))
									lo = mid;
								else
									hi = mid;
							}
							evalBiomechanicalCurl(f, lo, theta);
						}
					}
					else
					{
						// Dorsal obstacle behind back of hand: curl inward
						float lo = cUser;
						float hi = 1.0f;
						if (!allSegsClearAtBio(hi))
						{
							evalBiomechanicalCurl(f, hi, theta);
						}
						else
						{
							for (int it = 0; it < 6; ++it)
							{
								const float mid = 0.5f * (lo + hi);
								if (allSegsClearAtBio(mid))
									hi = mid;
								else
									lo = mid;
							}
							evalBiomechanicalCurl(f, hi, theta);
						}
					}
				}

				// Stage 2B: Per-phalanx local refinement & palmar draping (MCP -> PIP -> DIP)
				matrix3x4_t W_parent = visualWrist;
				{
					matrix3x4_t T_meta{}, R_meta{}, T_kin{};
					std::memcpy(&T_meta, openFingers[f][0], sizeof(matrix3x4_t));
					BuildJointDeltaRotation(theta[0], 0.0f, R_meta);
					Mul3x4(T_meta, R_meta, T_kin);
					Mul3x4(W_parent, T_kin, W_parent);
				}

				bool fingerHadDrapeHit = false;
				for (int k = 1; k <= 3; ++k)
				{
					const float r = kFingerPhalanxRadii[f][k - 1];
					const float splayK = (k == 1) ? workSplay : 0.0f;
					matrix3x4_t T_base{};
					std::memcpy(&T_base, openFingers[f][k], sizeof(matrix3x4_t));

					auto evalSingleJoint = [&](float candDeg, matrix3x4_t& outWk, Vector& outP, Vector& outQ)
					{
						matrix3x4_t R_delta{}, T_kin{};
						BuildJointDeltaRotation(candDeg, splayK, R_delta);
						Mul3x4(T_base, R_delta, T_kin);
						Mul3x4(W_parent, T_kin, outWk);
						outP = Vector(outWk[0][3], outWk[1][3], outWk[2][3]);
						if (k < 3)
						{
							outQ = TransformPoint3x4(
								outWk,
								openFingers[f][k + 1][0][3],
								openFingers[f][k + 1][1][3],
								openFingers[f][k + 1][2][3]);
						}
						else
						{
							outQ = TransformPoint3x4(outWk, tipExtents[f + 1], 0.0f, 0.0f);
						}
					};

					matrix3x4_t W_k{};
					Vector P{}, Q{}, hitN{};
					evalSingleJoint(theta[k], W_k, P, Q);

					if (testPhalanxSegment(P, Q, r, &hitN))
					{
						anyFingerSurfaceTouch = true;
						const Vector tCurl(W_k[0][1], W_k[1][1], W_k[2][1]);
						const float dirSign = (DotProduct(tCurl, hitN) < 0.15f) ? -1.0f : 1.0f;
						const float limitDeg = (dirSign < 0.0f) ? kMinJointDeg[k] : kMaxJointDeg[k];

						float prevBad = theta[k];
						float goodCand = theta[k];
						bool foundClear = false;
						for (int stp = 1; stp <= 5; ++stp)
						{
							float cand = theta[k] + dirSign * (12.0f * static_cast<float>(stp));
							const bool hitLimit = (dirSign < 0.0f) ? (cand <= limitDeg) : (cand >= limitDeg);
							cand = ClampF(cand, kMinJointDeg[k], kMaxJointDeg[k]);

							matrix3x4_t wTmp{};
							Vector pTmp{}, qTmp{};
							evalSingleJoint(cand, wTmp, pTmp, qTmp);
							if (!testPhalanxSegment(pTmp, qTmp, r))
							{
								goodCand = cand;
								foundClear = true;
								break;
							}
							prevBad = cand;
							if (hitLimit)
								break;
						}

						if (foundClear)
						{
							float bad = prevBad;
							float good = goodCand;
							for (int it = 0; it < 3; ++it)
							{
								const float mid = 0.5f * (bad + good);
								matrix3x4_t wTmp{};
								Vector pTmp{}, qTmp{};
								evalSingleJoint(mid, wTmp, pTmp, qTmp);
								if (testPhalanxSegment(pTmp, qTmp, r))
									bad = mid;
								else
									good = mid;
							}
							theta[k] = good;
						}
						else
						{
							theta[k] = limitDeg;
						}
					}
					else if (drapeWeight > 0.05f && cUser < 0.85f && theta[k] < kDrapeMaxDeg[k])
					{
						const float drapeLimit = theta[k] + (kDrapeMaxDeg[k] - theta[k]) * drapeWeight;
						if (drapeLimit > theta[k] + 0.5f)
						{
							const int steps = std::max(1, static_cast<int>(std::ceil((drapeLimit - theta[k]) / 15.0f)));
							const float stepSize = (drapeLimit - theta[k]) / static_cast<float>(steps);
							float thetaA = theta[k];
							matrix3x4_t wA{};
							Vector pA{}, qA{};
							evalSingleJoint(thetaA, wA, pA, qA);
							Vector mA = (pA + qA) * 0.5f;

							bool hitDuringDrape = false;
							for (int stp = 1; stp <= steps; ++stp)
							{
								const float thetaB = (stp == steps) ? drapeLimit : (theta[k] + stepSize * static_cast<float>(stp));
								matrix3x4_t wB{};
								Vector pB{}, qB{};
								evalSingleJoint(thetaB, wB, pB, qB);
								const Vector mB = (pB + qB) * 0.5f;

								const float fQ = testSweepChord(qA, qB, r);
								const float fM = testSweepChord(mA, mB, r);
								const float earliest = std::min(fQ, fM);
								if (earliest <= 1.0f)
								{
									theta[k] = thetaA + (thetaB - thetaA) * (earliest * 0.96f);
									hitDuringDrape = true;
									fingerHadDrapeHit = true;
									anyFingerSurfaceTouch = true;
									break;
								}
								thetaA = thetaB;
								qA = qB;
								mA = mB;
							}

							if (!hitDuringDrape && k > 1 && fingerHadDrapeHit)
								theta[k] = std::min(drapeLimit, theta[k] + 12.0f * drapeWeight);
						}
					}

					matrix3x4_t wFinal{};
					Vector pFinal{}, qFinal{};
					evalSingleJoint(theta[k], wFinal, pFinal, qFinal);
					W_parent = wFinal;
				}

				for (int k = 0; k < 4; ++k)
					targetPose.fingerCurlDeg[f][k] = theta[k];

				float dPush = 0.0f;
				float pDrape = 0.0f;
				for (int k = 1; k <= 3; ++k)
				{
					dPush += std::max(0.0f, userDeg[k] - theta[k]);
					pDrape += std::max(0.0f, theta[k] - userDeg[k]);
				}
				dorsalPushDeg[f] = dPush;
				palmarDrapeDeg[f] = pDrape;
				if (dPush > 1.0f || pDrape > 1.0f)
					anyFingerDeviated = true;
			}

			// Surface finger splay ("bei einer Flaeche, drueckt die Hand auseinander")
			const float avgDorsalPush = 0.25f * (dorsalPushDeg[0] + dorsalPushDeg[1] + dorsalPushDeg[2] + dorsalPushDeg[3]);
			const float avgPalmarDrape = 0.25f * (palmarDrapeDeg[0] + palmarDrapeDeg[1] + palmarDrapeDeg[2] + palmarDrapeDeg[3]);
			const Vector finalPalmPos = TransformPoint3x4(visualWrist, palmLocal.x, palmLocal.y, palmLocal.z);
			const float mean4Curl = 0.25f * (liveCurls[1] + liveCurls[2] + liveCurls[3] + liveCurls[4]);
			const float openHandMask = 1.0f - Smoothstep01(0.55f, 0.78f, mean4Curl);
			const float flatSurfaceMask = 1.0f - Smoothstep01(8.0f, 22.0f, avgPalmarDrape);
			const float palmPressIn = palmRes.hadPalmContact
				? std::max(0.0f, DotProduct(finalPalmPos - rawPalmPos, palmRes.contactNormal))
				: 0.0f;
			const float spreadDrive = std::max(
				ClampF(avgDorsalPush / 30.0f, 0.0f, 1.0f) * 0.70f,
				Smoothstep01(0.10f, 1.50f, palmPressIn) * ((palmRes.palmarSurfaceDist < 0.80f) ? 1.0f : 0.0f));
			const float spreadAlpha = ClampF(spreadDrive * openHandMask * flatSurfaceMask, 0.0f, 1.0f);

			for (int f = 0; f < 4; ++f)
			{
				const float cUser = ClampF(liveCurls[f + 1], 0.0f, 1.0f);
				const float baseSplay = baseSplaySign * (liveSplays[f] * 10.0f + (0.5f - cUser) * kSplayWeights[f] * 4.0f);
				targetPose.fingerSplayDeg[f] = baseSplay + spreadSign * kSplayWeights[f] * kSurfaceSpreadDeg[f] * spreadAlpha;
			}

			// Thumb (CMC k=0, MCP k=1, IP k=2)
			const float thumbPalmarSign = isLeftMesh ? 1.0f : -1.0f;
			const float cThumb = ClampF(liveCurls[0], 0.0f, 1.0f);
			const float baseThumbSplay = (isLeftMesh ? -1.0f : 1.0f) * liveSplays[0] * 10.0f;
			float tCurl[3]{};
			EvalBareThumbCurlDeg(cThumb, tCurl, isLeftMesh);
			float tPitch[2] = { 0.0f, 0.0f };

			auto evalThumbChain = [&](const float cArr[3], const float pArr[2], matrix3x4_t outW[3], Vector outPts[4])
			{
				EvalThumbWorldChainFromWrist(visualWrist, openThumb, tipExtents[0], thumbPalmarSign, baseThumbSplay, cArr, pArr, outW, outPts);
			};

			// Thumb CMC (k = 0)
			{
				constexpr float minCmcPitch = -13.0f;
				constexpr float maxCmcPitch = 24.0f;
				matrix3x4_t wT[3]{};
				Vector pT[4]{};
				evalThumbChain(tCurl, tPitch, wT, pT);
				Vector hitN{};
				if (testPhalanxSegment(pT[0], pT[1], kThumbPhalanxRadii[0], &hitN))
				{
					anyFingerSurfaceTouch = true;
					const Vector palmarDir(
						wT[0][0][2] * (-thumbPalmarSign),
						wT[0][1][2] * (-thumbPalmarSign),
						wT[0][2][2] * (-thumbPalmarSign));
					const float dirSign = (DotProduct(palmarDir, hitN) < 0.15f) ? -1.0f : 1.0f;
					const float limitDeg = (dirSign < 0.0f) ? minCmcPitch : maxCmcPitch;
					float prevBad = tPitch[0];
					float goodCand = tPitch[0];
					bool foundClear = false;
					for (int stp = 1; stp <= 5; ++stp)
					{
						float cand = tPitch[0] + dirSign * (10.0f * static_cast<float>(stp));
						const bool hitLim = (dirSign < 0.0f) ? (cand <= limitDeg) : (cand >= limitDeg);
						cand = ClampF(cand, minCmcPitch, maxCmcPitch);
						const float tmpP[2] = { cand, tPitch[1] };
						evalThumbChain(tCurl, tmpP, wT, pT);
						if (!testPhalanxSegment(pT[0], pT[1], kThumbPhalanxRadii[0]))
						{
							goodCand = cand;
							foundClear = true;
							break;
						}
						prevBad = cand;
						if (hitLim)
							break;
					}
					if (foundClear)
					{
						float bad = prevBad, good = goodCand;
						for (int it = 0; it < 3; ++it)
						{
							const float mid = 0.5f * (bad + good);
							const float tmpP[2] = { mid, tPitch[1] };
							evalThumbChain(tCurl, tmpP, wT, pT);
							if (testPhalanxSegment(pT[0], pT[1], kThumbPhalanxRadii[0]))
								bad = mid;
							else
								good = mid;
						}
						tPitch[0] = good;
					}
					else
					{
						tPitch[0] = limitDeg;
					}
					anyFingerDeviated = true;
				}
				else if (drapeWeight > 0.05f && cThumb < 0.80f && tPitch[0] < maxCmcPitch)
				{
					const float drapeLim = tPitch[0] + (maxCmcPitch - tPitch[0]) * drapeWeight;
					if (drapeLim > tPitch[0] + 0.5f)
					{
						const int steps = std::max(1, static_cast<int>(std::ceil((drapeLim - tPitch[0]) / 12.0f)));
						const float stepSize = (drapeLim - tPitch[0]) / static_cast<float>(steps);
						float pA = tPitch[0];
						Vector qA = pT[1];
						Vector mA = (pT[0] + pT[1]) * 0.5f;
						for (int stp = 1; stp <= steps; ++stp)
						{
							const float pB = (stp == steps) ? drapeLim : (tPitch[0] + stepSize * static_cast<float>(stp));
							const float tmpP[2] = { pB, tPitch[1] };
							evalThumbChain(tCurl, tmpP, wT, pT);
							const Vector qB = pT[1];
							const Vector mB = (pT[0] + pT[1]) * 0.5f;
							const float earliest = std::min(testSweepChord(qA, qB, kThumbPhalanxRadii[0]), testSweepChord(mA, mB, kThumbPhalanxRadii[0]));
							if (earliest <= 1.0f)
							{
								tPitch[0] = pA + (pB - pA) * (earliest * 0.96f);
								anyFingerDeviated = true;
								anyFingerSurfaceTouch = true;
								break;
							}
							pA = pB;
							qA = qB;
							mA = mB;
						}
					}
				}
			}

			// Flat-surface thumb radial abduction (clamp if blocked by adjacent geometry)
			if (spreadAlpha > 0.01f)
			{
				matrix3x4_t wBase[3]{};
				Vector pBase[4]{};
				evalThumbChain(tCurl, tPitch, wBase, pBase);
				const bool baseBlocked =
					testPhalanxSegment(pBase[0], pBase[1], kThumbPhalanxRadii[0]) ||
					testPhalanxSegment(pBase[1], pBase[2], kThumbPhalanxRadii[1]) ||
					testPhalanxSegment(pBase[2], pBase[3], kThumbPhalanxRadii[2]);
				if (!baseBlocked)
				{
					const float cmcAbductDeg = isLeftMesh ? 6.0f : 12.0f;
					for (float scale : { 1.0f, 0.5f })
					{
						const float candC[3] = {
							tCurl[0] - cmcAbductDeg * spreadAlpha * scale,
							tCurl[1] - 8.0f * spreadAlpha * scale,
							tCurl[2]
						};
						matrix3x4_t wCand[3]{};
						Vector pCand[4]{};
						evalThumbChain(candC, tPitch, wCand, pCand);
						if (!testPhalanxSegment(pCand[0], pCand[1], kThumbPhalanxRadii[0]) &&
							!testPhalanxSegment(pCand[1], pCand[2], kThumbPhalanxRadii[1]) &&
							!testPhalanxSegment(pCand[2], pCand[3], kThumbPhalanxRadii[2]))
						{
							tCurl[0] = candC[0];
							tCurl[1] = candC[1];
							break;
						}
					}
				}
			}

			// Thumb MCP + distal phalanx (k = 1)
			{
				matrix3x4_t wT[3]{};
				Vector pT[4]{};
				evalThumbChain(tCurl, tPitch, wT, pT);
				Vector hitN{};
				const bool hitSeg1 = testPhalanxSegment(pT[1], pT[2], kThumbPhalanxRadii[1], &hitN);
				const bool hitSeg2 = !hitSeg1 && testPhalanxSegment(pT[2], pT[3], kThumbPhalanxRadii[2], &hitN);
				auto thumbDistalHit = [&](const float cArr[3], const float pArr[2], Vector* outN = nullptr) -> bool
				{
					matrix3x4_t wTmp[3]{};
					Vector pTmp[4]{};
					evalThumbChain(cArr, pArr, wTmp, pTmp);
					if (testPhalanxSegment(pTmp[1], pTmp[2], kThumbPhalanxRadii[1], outN))
						return true;
					return testPhalanxSegment(pTmp[2], pTmp[3], kThumbPhalanxRadii[2], outN);
				};

				if (hitSeg1 || hitSeg2)
				{
					anyFingerSurfaceTouch = true;
					const Vector palmarDir(
						wT[1][0][2] * (-thumbPalmarSign),
						wT[1][1][2] * (-thumbPalmarSign),
						wT[1][2][2] * (-thumbPalmarSign));
					const float dirSign = (DotProduct(palmarDir, hitN) < 0.15f) ? -1.0f : 1.0f;
					const float limitDeg = (dirSign < 0.0f) ? -9.0f : 26.0f;
					float prevBad = tPitch[1];
					float goodCand = tPitch[1];
					bool foundClear = false;
					for (int stp = 1; stp <= 5; ++stp)
					{
						float cand = tPitch[1] + dirSign * (10.0f * static_cast<float>(stp));
						const bool hitLim = (dirSign < 0.0f) ? (cand <= limitDeg) : (cand >= limitDeg);
						cand = ClampF(cand, -9.0f, 26.0f);
						const float tmpP[2] = { tPitch[0], cand };
						if (!thumbDistalHit(tCurl, tmpP))
						{
							goodCand = cand;
							foundClear = true;
							break;
						}
						prevBad = cand;
						if (hitLim)
							break;
					}
					if (foundClear)
					{
						float bad = prevBad, good = goodCand;
						for (int it = 0; it < 3; ++it)
						{
							const float mid = 0.5f * (bad + good);
							const float tmpP[2] = { tPitch[0], mid };
							if (thumbDistalHit(tCurl, tmpP))
								bad = mid;
							else
								good = mid;
						}
						tPitch[1] = good;
					}
					else
					{
						tPitch[1] = limitDeg;
						// If thumb still clips medially into a curved object side (e.g. desk mouse), abduct radially outward
						Vector postHitN{};
						if (thumbDistalHit(tCurl, tPitch, &postHitN))
						{
							evalThumbChain(tCurl, tPitch, wT, pT);
							const Vector medDir(wT[1][0][1], wT[1][1][1], wT[1][2][1]);
							if (DotProduct(medDir, postHitN) < 0.15f)
							{
								const float origC1 = tCurl[1], origC2 = tCurl[2];
								for (int stp = 1; stp <= 6; ++stp)
								{
									const float frac = static_cast<float>(stp) / 6.0f;
									const float candC[3] = {
										tCurl[0],
										origC1 + (-9.0f - origC1) * frac,
										origC2 + (-5.0f - origC2) * frac
									};
									if (!thumbDistalHit(candC, tPitch))
									{
										tCurl[1] = candC[1];
										tCurl[2] = candC[2];
										break;
									}
								}
							}
						}
					}
					anyFingerDeviated = true;
				}
				else if (drapeWeight > 0.05f && cThumb < 0.80f && tPitch[1] < 26.0f)
				{
					const float drapeLim = tPitch[1] + (26.0f - tPitch[1]) * drapeWeight;
					if (drapeLim > tPitch[1] + 0.5f)
					{
						const int steps = std::max(1, static_cast<int>(std::ceil((drapeLim - tPitch[1]) / 12.0f)));
						const float stepSize = (drapeLim - tPitch[1]) / static_cast<float>(steps);
						float pA = tPitch[1];
						Vector p2A = pT[2], p3A = pT[3], mA = (pT[2] + pT[3]) * 0.5f;
						for (int stp = 1; stp <= steps; ++stp)
						{
							const float pB = (stp == steps) ? drapeLim : (tPitch[1] + stepSize * static_cast<float>(stp));
							const float tmpP[2] = { tPitch[0], pB };
							evalThumbChain(tCurl, tmpP, wT, pT);
							const Vector p2B = pT[2], p3B = pT[3], mB = (pT[2] + pT[3]) * 0.5f;
							const float f3 = testSweepChord(p3A, p3B, kThumbPhalanxRadii[2]);
							const float f2 = testSweepChord(p2A, p2B, kThumbPhalanxRadii[1]);
							const float fM = testSweepChord(mA, mB, kThumbPhalanxRadii[2]);
							const float earliest = std::min(f3, std::min(f2, fM));
							if (earliest <= 1.0f)
							{
								tPitch[1] = pA + (pB - pA) * (earliest * 0.96f);
								anyFingerDeviated = true;
								anyFingerSurfaceTouch = true;
								break;
							}
							pA = pB;
							p2A = p2B;
							p3A = p3B;
							mA = mB;
						}
					}
				}
			}

			for (int k = 0; k < 3; ++k)
				targetPose.thumbCurlDeg[k] = tCurl[k];
			targetPose.thumbSplayDeg[0] = thumbPalmarSign * tPitch[0];
			targetPose.thumbSplayDeg[1] = baseThumbSplay + thumbPalmarSign * tPitch[1];

			const bool anyActive = palmRes.hadPalmContact || palmRes.hadFingerPushback || (drapeWeight > 0.05f) || (spreadAlpha > 0.02f) || anyFingerDeviated;

			if (!fSm.initialized)
			{
				fSm.initialized = true;
				fSm.pose = targetPose;
				fSm.pose.blendAlpha = 0.0f;
				fSm.smoothedSpreadAlpha = spreadAlpha;
			}
			else
			{
				auto smoothJoint = [&](float cur, float tgt, bool dorsalEscape) -> float
				{
					const float diff = tgt - cur;
					if (std::fabs(diff) < 0.55f)
						return cur;
					const float rate = dorsalEscape ? 38.0f : 22.0f;
					return cur + diff * (1.0f - std::exp(-dt * rate));
				};

				for (int f = 0; f < 4; ++f)
				{
					for (int k = 0; k < 4; ++k)
					{
						const bool dorsal = (targetPose.fingerCurlDeg[f][k] < fSm.pose.fingerCurlDeg[f][k]);
						fSm.pose.fingerCurlDeg[f][k] = smoothJoint(fSm.pose.fingerCurlDeg[f][k], targetPose.fingerCurlDeg[f][k], dorsal);
					}
					fSm.pose.fingerSplayDeg[f] = smoothJoint(fSm.pose.fingerSplayDeg[f], targetPose.fingerSplayDeg[f], false);
				}
				for (int k = 0; k < 3; ++k)
				{
					const bool dorsal = (targetPose.thumbCurlDeg[k] < fSm.pose.thumbCurlDeg[k]);
					fSm.pose.thumbCurlDeg[k] = smoothJoint(fSm.pose.thumbCurlDeg[k], targetPose.thumbCurlDeg[k], dorsal);
				}
				for (int k = 0; k < 2; ++k)
					fSm.pose.thumbSplayDeg[k] = smoothJoint(fSm.pose.thumbSplayDeg[k], targetPose.thumbSplayDeg[k], false);

				fSm.smoothedSpreadAlpha += (spreadAlpha - fSm.smoothedSpreadAlpha) * (1.0f - std::exp(-dt * 20.0f));
			}

			const float targetAlpha = anyActive ? 1.0f : 0.0f;
			const float alphaRate = anyActive ? 32.0f : 18.0f;
			fSm.pose.blendAlpha += (targetAlpha - fSm.pose.blendAlpha) * (1.0f - std::exp(-dt * alphaRate));

			if (fSm.pose.blendAlpha < 0.005f)
			{
				s_SurfaceFingerSlots[handIdx].Clear();
				if (!anyActive)
					fSm.initialized = false;
			}
			else
			{
				s_SurfaceFingerSlots[handIdx].Write(fSm.pose, nowMs);
			}
			return anyFingerSurfaceTouch;
		}
	}

	void PublishHeldHandPhysicsFeedback(const HeldHandPhysicsFeedback& fb)
	{
		s_HeldPhysicsSlot.Publish(fb, GetTickCount64());
	}

	void ClearHeldHandPhysicsFeedback()
	{
		s_HeldPhysicsSlot.Clear();
	}

	bool GetHeldHandPhysicsFeedback(HeldHandPhysicsFeedback& outFb, unsigned long long maxAgeMs)
	{
		return s_HeldPhysicsSlot.Read(outFb, GetTickCount64(), maxAgeMs);
	}

	void ResetVisualHandPhysics()
	{
		const unsigned long long nowMs = GetTickCount64();
		for (int i = 0; i < 2; ++i)
		{
			s_VisualSmooth[i] = VisualHandSmoothingState{};
			s_FingerSmooth[i] = VisualFingerSmoothState{};
			s_TouchHaptic[i] = VisualHandTouchHapticState{};
			s_TouchHaptic[i].suppressUntilMs = nowMs + 250ULL;
			s_VisualWristSlots[i].Clear();
			s_SurfaceFingerSlots[i].Clear();
			s_ModelToWristCache[i].Clear();
		}
		s_PhoneScreenSlot.Clear();
		g_ViewModelLens = ViewModelLensInfo{};
		ResetFlashlightGripAnim();
		ResetPhoneScreenTouch();
	}

	void UpdateVisualHandPhysics(Game* game, VR* vr, const Vector& eyePosition)
	{
		if (!vr || !vr->m_IsVREnabled)
		{
			ResetVisualHandPhysics();
			return;
		}

		void* localPlayer = nullptr;
		void* vm0Ent = nullptr;
		void* vm1Ent = nullptr;
		if (game && game->m_EngineClient)
		{
			const int localIdx = game->m_EngineClient->GetLocalPlayer();
			if (localIdx > 0)
				localPlayer = game->GetClientEntity(localIdx);
		}
		if (!localPlayer)
		{
			ResetVisualHandPhysics();
			return;
		}

		if (game && game->m_BaseClient != 0 && g_Game != nullptr)
		{
			typedef void* (__thiscall *tGetViewModel)(void* thisptr, int index);
			auto fnGetViewModel = reinterpret_cast<tGetViewModel>(game->m_BaseClient + 0x55260);
			vm0Ent = fnGetViewModel(localPlayer, 0);
			vm1Ent = fnGetViewModel(localPlayer, 1);
		}

		const unsigned long long nowMs = GetTickCount64();
		const auto steadyNow = std::chrono::steady_clock::now();
		if (!vr->m_PhoneEquipped)
			s_PhoneScreenSlot.Clear();
		const bool isPaused = (game && game->m_EngineClient && game->m_EngineClient->IsPaused());
		PhoneScreenQuad phoneQuad{};
		const bool hasPhoneQuad = vr->m_PhoneEquipped && s_PhoneScreenSlot.Read(phoneQuad, nowMs, 250ULL);
		HeldHandPhysicsFeedback fb{};
		const bool hasHeldFb = vr->m_IsHoldingObject && s_HeldPhysicsSlot.Read(fb, nowMs, 250ULL) && fb.heldEntIndex >= 0;

		void* heldClientEnt = nullptr;
		if (hasHeldFb && game && game->m_ClientEntityList)
			heldClientEnt = game->GetClientEntity(fb.heldEntIndex);

		Vector headAnchor = vr->GetViewOrigin(eyePosition);
		if (VecLen3(headAnchor - eyePosition) > 48.0f || VecLen3(headAnchor) < 1e-4f)
			headAnchor = eyePosition;

		const bool checkNPCs = (game && game->m_BaseClient != 0);
		CHandWorldPropTraceFilter palmFilter(
			reinterpret_cast<IHandleEntity*>(localPlayer),
			reinterpret_cast<IHandleEntity*>(heldClientEnt),
			reinterpret_cast<IHandleEntity*>(vm0Ent),
			reinterpret_cast<IHandleEntity*>(vm1Ent),
			checkNPCs,
			false);
		CHandWorldPropTraceFilter fingerFilter(
			reinterpret_cast<IHandleEntity*>(localPlayer),
			reinterpret_cast<IHandleEntity*>(heldClientEnt),
			reinterpret_cast<IHandleEntity*>(vm0Ent),
			reinterpret_cast<IHandleEntity*>(vm1Ent),
			checkNPCs,
			true);

		for (int handIdx = 0; handIdx < 2; ++handIdx)
		{
			const bool physicalLeft = (handIdx == 1);
			matrix3x4_t rawM2W{}, rawWrist{}, targetWrist{};
			bool usePhoneOpen = false;
			bool isLeftMesh = false;
			HandSkinPreset skinPreset = HandSkinPreset::RightCam;
			Vector fwd, left, up;
			if (!ComputeRawControllerWristPose(
				vr, physicalLeft, eyePosition, nowMs,
				rawM2W, rawWrist, usePhoneOpen, isLeftMesh, fwd, left, up, &skinPreset))
			{
				continue;
			}

			const Vector palmLocal = GetPalmCenterInWristLocal(skinPreset);
			const Vector rawWristPos(rawWrist[0][3], rawWrist[1][3], rawWrist[2][3]);
			const Vector rawPalmPos = TransformPoint3x4(rawWrist, palmLocal.x, palmLocal.y, palmLocal.z);

			Vector hmdRight = vr->m_HmdRight;
			if (VecLen3(hmdRight) < 0.5f)
				hmdRight = Vector(-left.x, -left.y, -left.z);
			hmdRight = NormalizeSafe(hmdRight, Vector(0.0f, -1.0f, 0.0f));
			const Vector shoulderAnchor = headAnchor + hmdRight * (physicalLeft ? -7.0f : 7.0f) - Vector(0.0f, 0.0f, 8.5f);

			Vector safeShoulder = shoulderAnchor;
			if (game && game->m_EngineTrace)
			{
				Ray_t shRay;
				std::memset(&shRay, 0, sizeof(shRay));
				shRay.Init(
					headAnchor,
					shoulderAnchor,
					Vector(-kShoulderHullRadius, -kShoulderHullRadius, -kShoulderHullRadius),
					Vector( kShoulderHullRadius,  kShoulderHullRadius,  kShoulderHullRadius));
				CGameTrace trShoulder;
				std::memset(&trShoulder, 0, sizeof(trShoulder));
				trShoulder.fraction = 1.0f;
				game->m_EngineTrace->TraceRay(shRay, kMaskStaticWorldSolid, &palmFilter, &trShoulder);
				if (trShoulder.startsolid)
					safeShoulder = headAnchor;
				else if (trShoulder.fraction < 1.0f)
					safeShoulder = trShoulder.endpos;
			}

			float liveCurls[5]{};
			{
				const auto& srcCurls = physicalLeft ? vr->m_LeftFingerCurl : vr->m_RightFingerCurl;
				float maxClamp[5] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
				const bool hasClamp = GetFingerCurlLimits(physicalLeft, maxClamp);
				for (int fi = 0; fi < 5; ++fi)
					liveCurls[fi] = ClampF(srcCurls[fi], 0.0f, hasClamp ? maxClamp[fi] : 1.0f);

				if (isLeftMesh && vr->m_FlashlightEquipped)
				{
					liveCurls[0] = 0.55f; liveCurls[1] = 0.68f; liveCurls[2] = 0.72f; liveCurls[3] = 0.75f; liveCurls[4] = 0.78f;
				}
				else if (!isLeftMesh && !vr->m_RightHandEmpty)
				{
					liveCurls[2] = 0.72f; liveCurls[3] = 0.75f; liveCurls[4] = 0.78f;
				}
			}

			VisualHandSmoothingState& st = s_VisualSmooth[handIdx];
			const bool resetSmooth = !st.initialized ||
				(nowMs < st.lastTickMs) ||
				((nowMs - st.lastTickMs) > 250ULL) ||
				(VecLen3(rawPalmPos - st.prevRawPalm) > 24.0f);

			const bool isHoldingThisHand = (hasHeldFb && fb.holdingLeftHand == physicalLeft);
			float targetAlpha = 0.0f;
			targetWrist = rawWrist;
			PalmContactResult palmRes{};

			if (isHoldingThisHand)
			{
				ComputeCoupledHeldWrist(fb, rawWrist, targetWrist, targetAlpha);
				if (game && game->m_EngineTrace)
				{
					const Vector coupledPalmPos = TransformPoint3x4(targetWrist, palmLocal.x, palmLocal.y, palmLocal.z);
					const Vector wristDistal = NormalizeSafe(
						Vector(targetWrist[0][0], targetWrist[1][0], targetWrist[2][0]), fwd);
					const Vector elbowAnchor = ComputeSafeElbowAnchor(safeShoulder, coupledPalmPos, wristDistal);
					const Vector safePalm = SweepHandPalmHullWithSlide(
						game->m_EngineTrace,
						palmFilter,
						headAnchor,
						safeShoulder,
						elbowAnchor,
						coupledPalmPos,
						kHeldSafetyWallRadius,
						kMaskStaticWorldSolid);
					const Vector wallAdj = safePalm - coupledPalmPos;
					targetWrist[0][3] += wallAdj.x;
					targetWrist[1][3] += wallAdj.y;
					targetWrist[2][3] += wallAdj.z;
				}
			}
			else if (!isPaused && vr->m_ShowHands && game && game->m_EngineTrace)
			{
				const bool usePrevPalm = !resetSmooth && st.hasPrevVisualPalm;
				const auto& liveSplays = physicalLeft ? vr->m_LeftFingerSplay : vr->m_RightFingerSplay;
				const bool isBareHand = isLeftMesh ? (!vr->m_FlashlightEquipped) : vr->m_RightHandEmpty;
				palmRes = ResolveOpenHandPalmContact(
					game->m_EngineTrace,
					fingerFilter,
					fingerFilter,
					headAnchor,
					safeShoulder,
					rawPalmPos,
					rawWrist,
					skinPreset,
					liveCurls,
					liveSplays,
					isBareHand,
					usePhoneOpen,
					usePrevPalm,
					st.prevVisualPalm,
					kMaskOpenHandSolid);
				const Vector wallShift = palmRes.resolvedPalm - rawPalmPos;
				targetWrist[0][3] += wallShift.x;
				targetWrist[1][3] += wallShift.y;
				targetWrist[2][3] += wallShift.z;
			}

			if (vr->m_PhoneEquipped && hasPhoneQuad && !isHoldingThisHand && (physicalLeft != vr->m_LeftHanded))
			{
				const bool isBareHand = isLeftMesh ? (!vr->m_FlashlightEquipped) : vr->m_RightHandEmpty;
				if (isBareHand)
				{
					const auto& liveSplays = physicalLeft ? vr->m_LeftFingerSplay : vr->m_RightFingerSplay;
					const float baseSplaySign = isLeftMesh ? 1.0f : -1.0f;
					const float indexCurl = ClampF(liveCurls[1], 0.0f, 1.0f);
					const float indexSplay = baseSplaySign * (liveSplays[0] * 10.0f + (0.5f - indexCurl) * kSplayWeights[0] * 4.0f);
					float idxDeg[4]{};
					EvalBareFingerCurlDeg(0, indexCurl, idxDeg);
					Vector ptsIdx[5]{};
					if (isLeftMesh && usePhoneOpen)
					{
						EvalMirroredPhoneLeftFingerFromWrist(rawWrist, 0, idxDeg, indexSplay, ptsIdx);
					}
					else
					{
						const auto& openFingers = isLeftMesh
							? s_OpenCamLeftFingers
							: (usePhoneOpen ? s_OpenPhoneRightFingers : s_OpenCamRightFingers);
						const float (&tipExtents)[5] = isLeftMesh ? kLeftTipExtent : kRightTipExtent;
						matrix3x4_t wIdx[4]{};
						EvalFingerWorldChainFromWrist(rawWrist, openFingers[0], tipExtents[1], idxDeg, indexSplay, wIdx, ptsIdx);
					}

					const Vector rel = ptsIdx[4] - phoneQuad.ul;
					const float d = DotProduct(rel, phoneQuad.normal);
					const float u = DotProduct(rel, phoneQuad.rightVec) / phoneQuad.widthLenSq;
					const float v = DotProduct(rel, phoneQuad.downVec) / phoneQuad.heightLenSq;
					if (u >= -0.06f && u <= 1.06f && v >= -0.06f && v <= 1.06f && d < 0.22f && d > -1.50f)
					{
						const Vector glassPush = phoneQuad.normal * (0.22f - d);
						targetWrist[0][3] += glassPush.x;
						targetWrist[1][3] += glassPush.y;
						targetWrist[2][3] += glassPush.z;
					}
				}
			}

			Vector targetOffset(
				targetWrist[0][3] - rawWristPos.x,
				targetWrist[1][3] - rawWristPos.y,
				targetWrist[2][3] - rawWristPos.z);
			targetOffset = ClampVectorMaxLength(targetOffset, kMaxTetherDist);

			float dt = 1.0f / 90.0f;
			if (st.hasSteadyTime)
			{
				const float elapsed = std::chrono::duration<float>(steadyNow - st.lastSteadyTime).count();
				dt = ClampF(elapsed, 0.0005f, 0.05f);
			}
			st.lastSteadyTime = steadyNow;
			st.hasSteadyTime = true;

			if (resetSmooth)
			{
				st.initialized = true;
				st.smoothedOffset = targetOffset;
				st.smoothedCouplingAlpha = targetAlpha;
			}
			else
			{
				const Vector deltaOff = targetOffset - st.smoothedOffset;
				const float deltaLen = VecLen3(deltaOff);
				const float posRate = 24.0f + 41.0f * Smoothstep01(0.06f, 0.45f, deltaLen);
				const float posBlend = 1.0f - std::exp(-dt * posRate);
				const float rotBlend = 1.0f - std::exp(-dt * 38.0f);

				st.smoothedOffset = st.smoothedOffset + deltaOff * posBlend;
				if (!isHoldingThisHand && palmRes.hadPalmContact && VecLen3(palmRes.contactNormal) > 0.5f)
				{
					const Vector curVisPalm = rawPalmPos + st.smoothedOffset;
					const float penBehindCore = DotProduct(palmRes.hardSafePalm - curVisPalm, palmRes.contactNormal);
					if (penBehindCore > 0.0f)
						st.smoothedOffset = st.smoothedOffset + palmRes.contactNormal * penBehindCore;
				}
				st.smoothedOffset = ClampVectorMaxLength(st.smoothedOffset, kMaxTetherDist);
				st.smoothedCouplingAlpha += (targetAlpha - st.smoothedCouplingAlpha) * rotBlend;
			}

			const Vector prevRawPalmSample = st.prevRawPalm;
			st.lastTickMs = nowMs;
			st.prevRawPalm = rawPalmPos;
			st.prevVisualPalm = rawPalmPos + st.smoothedOffset;
			st.hasPrevVisualPalm = true;

			matrix3x4_t finalVisualWrist{};
			const Vector finalPos = rawWristPos + st.smoothedOffset;
			if (st.smoothedCouplingAlpha > 0.001f)
				SlerpOrtho3x4(rawWrist, targetWrist, st.smoothedCouplingAlpha, finalPos, finalVisualWrist);
			else
			{
				finalVisualWrist = rawWrist;
				finalVisualWrist[0][3] = finalPos.x;
				finalVisualWrist[1][3] = finalPos.y;
				finalVisualWrist[2][3] = finalPos.z;
			}

			matrix3x4_t rawWristInv{}, worldDelta{};
			InvertOrtho3x4(rawWrist, rawWristInv);
			Mul3x4(finalVisualWrist, rawWristInv, worldDelta);
			s_VisualWristSlots[handIdx].Write(finalVisualWrist, worldDelta, rawM2W, nowMs);

			const bool fingerTouchNow = SolveHandFingerSurfaceConformation(
				handIdx,
				physicalLeft,
				isLeftMesh,
				usePhoneOpen,
				isHoldingThisHand,
				vr,
				game,
				fingerFilter,
				finalVisualWrist,
				rawPalmPos,
				palmLocal,
				liveCurls,
				palmRes,
				dt,
				nowMs);

			const float vrScale = (vr->m_VRScale > 1.0f && vr->m_VRScale < 300.0f) ? vr->m_VRScale : 43.2f;
			const float trackedSpeedMps = resetSmooth
				? 0.0f
				: ((VecLen3(rawPalmPos - prevRawPalmSample) / std::max(dt, 0.001f)) / vrScale);
			const float ctrlSpeedMps = physicalLeft ? vr->m_LeftHandSpeedMps : vr->m_RightHandSpeedMps;
			const float impactSpeedMps = std::max(trackedSpeedMps, ctrlSpeedMps);

			VisualHandTouchHapticState& hSt = s_TouchHaptic[handIdx];
			if (resetSmooth)
			{
				hSt = VisualHandTouchHapticState{};
				hSt.suppressUntilMs = nowMs + 250ULL;
			}
			else if (isHoldingThisHand || (vr->m_IsHoldingObject && vr->m_HoldingWithLeftHand == physicalLeft) || !vr->m_ShowHands)
			{
				hSt.palmArmed = false;
				hSt.fingerArmed = false;
				hSt.palmClearSec = 0.0f;
				hSt.fingerClearSec = 0.0f;
			}
			else
			{
				const bool palmEngage = palmRes.hadPalmContact || (palmRes.palmarSurfaceDist <= 0.10f);
				const bool palmStillNear = palmRes.hadPalmContact || (palmRes.palmarSurfaceDist <= 0.42f);
				if (palmStillNear)
				{
					hSt.palmClearSec = 0.0f;
				}
				else
				{
					hSt.palmClearSec += dt;
					if (hSt.palmClearSec >= 0.08f)
						hSt.palmArmed = true;
				}

				const bool effectiveFingerTouch = fingerTouchNow || palmRes.hadFingerPushback;
				if (effectiveFingerTouch)
				{
					hSt.fingerClearSec = 0.0f;
				}
				else
				{
					hSt.fingerClearSec += dt;
					if (hSt.fingerClearSec >= 0.08f)
						hSt.fingerArmed = true;
				}

				const bool canFire = (nowMs >= hSt.suppressUntilMs) &&
				                     (hSt.lastImpulseMs == 0ULL || (nowMs >= hSt.lastImpulseMs && (nowMs - hSt.lastImpulseMs) >= 110ULL));
				const bool firePalm = palmEngage && hSt.palmArmed;
				const bool fireFinger = effectiveFingerTouch && hSt.fingerArmed;

				if (firePalm || fireFinger)
				{
					if (firePalm)
						hSt.palmArmed = false;
					if (fireFinger)
						hSt.fingerArmed = false;

					if (canFire)
					{
						if (firePalm)
						{
							// Single crisp short impact tap when palm lands (or palm + fingers land together)
							hSt.fingerArmed = false;
							constexpr unsigned short kPalmTouchDurUs = 850;
							const float amp = VR::ComputeImpactHapticAmplitude(impactSpeedMps, 0.10f, 2.3f);
							vr->TriggerHapticPulseOnHand(physicalLeft, kPalmTouchDurUs, amp);
						}
						else
						{
							// Delicate fingertip touch tap
							constexpr unsigned short kFingerTouchDurUs = 750;
							const float amp = VR::ComputeImpactHapticAmplitude(impactSpeedMps, 0.08f, 2.3f);
							vr->TriggerHapticPulseOnHand(physicalLeft, kFingerTouchDurUs, amp);
						}
						hSt.lastImpulseMs = nowMs;
					}
				}
			}
		}
	}

	bool GetVisualViewModelTransform(
		VR* vr,
		bool isVm0,
		const Vector& eyePosition,
		const Vector& rawVmOrigin,
		const QAngle& rawVmAngles,
		Vector& outVmOrigin,
		QAngle& outVmAngles)
	{
		outVmOrigin = rawVmOrigin;
		outVmAngles = rawVmAngles;
		if (!vr || !vr->m_IsVREnabled)
			return false;

		const bool physicalLeft = isVm0 ? vr->m_LeftHanded : !vr->m_LeftHanded;
		const unsigned long long nowMs = GetTickCount64();

		matrix3x4_t rawM2W{}, rawWrist{}, visualWrist{}, mDelta{};
		bool usePhoneOpen = false;
		bool isLeftMesh = false;
		Vector fwd, left, up;
		if (!ComputeRawControllerWristPose(vr, physicalLeft, eyePosition, nowMs, rawM2W, rawWrist, usePhoneOpen, isLeftMesh, fwd, left, up))
			return false;

		if (!s_VisualWristSlots[physicalLeft ? 1 : 0].Read(visualWrist, &mDelta, nullptr, nowMs, 250ULL))
		{
			HeldHandPhysicsFeedback fb{};
			if (!vr->m_IsHoldingObject || !s_HeldPhysicsSlot.Read(fb, nowMs, 250ULL) || fb.holdingLeftHand != physicalLeft)
				return false;
			float alpha = 0.0f;
			ComputeCoupledHeldWrist(fb, rawWrist, visualWrist, alpha);
			if (alpha <= 0.001f)
				return false;
			matrix3x4_t rawWristInv{};
			InvertOrtho3x4(rawWrist, rawWristInv);
			Mul3x4(visualWrist, rawWristInv, mDelta);
		}

		const float posDiff = std::fabs(mDelta[0][3]) + std::fabs(mDelta[1][3]) + std::fabs(mDelta[2][3]);
		const float traceR = mDelta[0][0] + mDelta[1][1] + mDelta[2][2];
		if (posDiff <= 0.002f && traceR >= 2.99995f)
			return false;

		matrix3x4_t rawInputM2W{}, visualM2W{};
		AnglePosToMatrix3x4(rawVmAngles, rawVmOrigin, rawInputM2W);
		Mul3x4(mDelta, rawInputM2W, visualM2W);
		Matrix3x4ToAngles(visualM2W, outVmOrigin, outVmAngles);
		return true;
	}

	bool ComputeHandGripPose(
		VR* vr,
		bool physicalLeftHand,
		const Vector& eyePosition,
		VRHandGripPose& outPose,
		bool applyVisualPhysics)
	{
		outPose = VRHandGripPose{};
		if (!vr || !vr->m_IsVREnabled)
			return false;

		const unsigned long long nowMs = GetTickCount64();
		matrix3x4_t modelToWorld{}, wristToWorld{};
		bool usePhoneOpen = false;
		bool isLeftMesh = false;
		Vector fwd, left, up;
		if (!ComputeRawControllerWristPose(vr, physicalLeftHand, eyePosition, nowMs, modelToWorld, wristToWorld, usePhoneOpen, isLeftMesh, fwd, left, up))
			return false;

		if (applyVisualPhysics)
		{
			matrix3x4_t visualWrist{};
			if (s_VisualWristSlots[physicalLeftHand ? 1 : 0].Read(visualWrist, nullptr, nullptr, nowMs, 250ULL))
			{
				wristToWorld = visualWrist;
			}
			else if (vr->m_IsHoldingObject)
			{
				HeldHandPhysicsFeedback fb{};
				if (s_HeldPhysicsSlot.Read(fb, nowMs, 250ULL) && fb.holdingLeftHand == physicalLeftHand)
				{
					matrix3x4_t coupledWrist{};
					float alpha = 0.0f;
					ComputeCoupledHeldWrist(fb, wristToWorld, coupledWrist, alpha);
					wristToWorld = coupledWrist;
				}
			}
		}

		// Calibrated palmar VVD skin surface and closed-hand grip-hollow center (~9.5 mm in front of palm skin):
		//   Right Hand (cam/light): palmSkin = (+3.88, +0.48, +0.22), grip = (+3.88, +0.90, +0.22)
		//   Right Hand (v_phone):   palmSkin = (+3.88, +0.76, +0.22), grip = (+3.88, +1.16, +0.22)
		//   Left Hand  (v_light):   palmSkin = (+3.92, +0.50, -0.42), grip = (+3.92, +0.92, -0.42)
		const bool phoneOpenR = (!isLeftMesh && usePhoneOpen);
		const float psx = isLeftMesh ?  3.92f : 3.88f;
		const float psy = isLeftMesh ?  0.50f : (phoneOpenR ? 0.76f : 0.48f);
		const float psz = isLeftMesh ? -0.42f : 0.22f;
		const float gx  = isLeftMesh ?  3.92f : 3.88f;
		const float gy  = isLeftMesh ?  0.92f : (phoneOpenR ? 1.16f : 0.90f);
		const float gz  = isLeftMesh ? -0.42f : 0.22f;

		outPose.wristToWorld = wristToWorld;
		outPose.wristWorldPos = Vector(wristToWorld[0][3], wristToWorld[1][3], wristToWorld[2][3]);
		outPose.palmSkinWorldPos = TransformPoint3x4(wristToWorld, psx, psy, psz);
		outPose.gripWorldPos = TransformPoint3x4(wristToWorld, gx, gy, gz);
		outPose.fingerDistalDir = NormalizeSafe(
			Vector(wristToWorld[0][0], wristToWorld[1][0], wristToWorld[2][0]), fwd);
		outPose.palmOutwardNormal = NormalizeSafe(
			Vector(wristToWorld[0][1], wristToWorld[1][1], wristToWorld[2][1]), up);
		const float thumbSign = isLeftMesh ? -1.0f : 1.0f;
		outPose.thumbRadialDir = NormalizeSafe(
			Vector(wristToWorld[0][2] * thumbSign, wristToWorld[1][2] * thumbSign, wristToWorld[2][2] * thumbSign), left);
		outPose.isLeftHandMesh = isLeftMesh;
		outPose.usePhoneOpenTable = usePhoneOpen;
		outPose.valid = true;
		return true;
	}

	void EvaluateFingerWorldChain(
		const VRHandGripPose& handPose,
		int fingerIdx,
		float curl01,
		float splay01,
		Vector outJoints[5],
		float outRadii[4])
	{
		if (!outJoints)
			return;

		const float c = ClampF(curl01, 0.0f, 1.0f);
		const float s = ClampF(splay01, -1.0f, 1.0f);
		const bool isLeft = handPose.isLeftHandMesh;

		// Effective palmar capsule radii (in Source units, including radial fullness scaling)
		static constexpr float kThumbRadii[4]  = { 0.56f, 0.46f, 0.41f, 0.39f };
		static constexpr float kFingerRadii[4][4] = {
			{ 0.54f, 0.44f, 0.39f, 0.35f }, // Index
			{ 0.54f, 0.44f, 0.39f, 0.35f }, // Middle
			{ 0.50f, 0.42f, 0.37f, 0.33f }, // Ring
			{ 0.48f, 0.39f, 0.34f, 0.30f }  // Pinky
		};

		const int fClamped = (fingerIdx < 0) ? 0 : ((fingerIdx > 4) ? 4 : fingerIdx);
		const float tipX = isLeft ? kLeftTipExtent[fClamped] : kRightTipExtent[fClamped];

		if (fClamped == 0)
		{
			if (outRadii)
			{
				for (int i = 0; i < 4; ++i)
					outRadii[i] = kThumbRadii[i];
			}

			const auto& openThumb = isLeft
				? (handPose.usePhoneOpenTable ? s_OpenPhoneLeftThumb  : s_OpenCamLeftThumb)
				: (handPose.usePhoneOpenTable ? s_OpenPhoneRightThumb : s_OpenCamRightThumb);
			float jointDeg[3]{};
			EvalBareThumbCurlDeg(c, jointDeg, isLeft);
			const float splayDeg = (isLeft ? -1.0f : 1.0f) * s * 10.0f;

			matrix3x4_t W_parent = handPose.wristToWorld;
			matrix3x4_t W_bone[3]{};
			for (int k = 0; k < 3; ++k)
			{
				matrix3x4_t T_base{}, R_delta{}, T_kin{};
				std::memcpy(&T_base, openThumb[k], sizeof(matrix3x4_t));
				BuildJointDeltaRotation(jointDeg[k], (k == 1) ? splayDeg : 0.0f, R_delta);
				Mul3x4(T_base, R_delta, T_kin);
				Mul3x4(W_parent, T_kin, W_bone[k]);
				W_parent = W_bone[k];
			}

			outJoints[0] = Vector(W_bone[0][0][3], W_bone[0][1][3], W_bone[0][2][3]);
			outJoints[1] = Vector(W_bone[1][0][3], W_bone[1][1][3], W_bone[1][2][3]);
			outJoints[2] = Vector(W_bone[2][0][3], W_bone[2][1][3], W_bone[2][2][3]);
			outJoints[3] = TransformPoint3x4(W_bone[2], tipX * 0.5f, 0.0f, 0.0f);
			outJoints[4] = TransformPoint3x4(W_bone[2], tipX, 0.0f, 0.0f);
			return;
		}

		const int f = fClamped - 1;
		if (outRadii)
		{
			for (int i = 0; i < 4; ++i)
				outRadii[i] = kFingerRadii[f][i];
		}

		float jointDeg[4]{};
		EvalBareFingerCurlDeg(f, c, jointDeg);
		const float splaySign = isLeft ? 1.0f : -1.0f;
		const float splayDeg = splaySign * (s * 10.0f + (0.5f - c) * kSplayWeights[f] * 4.0f);
		if (isLeft && handPose.usePhoneOpenTable)
		{
			EvalMirroredPhoneLeftFingerFromWrist(handPose.wristToWorld, f, jointDeg, splayDeg, outJoints);
			return;
		}

		const auto& openFingers = isLeft
			? (handPose.usePhoneOpenTable ? s_OpenPhoneLeftFingers  : s_OpenCamLeftFingers)
			: (handPose.usePhoneOpenTable ? s_OpenPhoneRightFingers : s_OpenCamRightFingers);

		matrix3x4_t W_parent = handPose.wristToWorld;
		matrix3x4_t W_last{};
		for (int k = 0; k < 4; ++k)
		{
			matrix3x4_t T_base{}, R_delta{}, T_kin{}, W_cur{};
			std::memcpy(&T_base, openFingers[f][k], sizeof(matrix3x4_t));
			BuildJointDeltaRotation(jointDeg[k], (k == 1) ? splayDeg : 0.0f, R_delta);
			Mul3x4(T_base, R_delta, T_kin);
			Mul3x4(W_parent, T_kin, W_cur);
			outJoints[k] = Vector(W_cur[0][3], W_cur[1][3], W_cur[2][3]);
			W_parent = W_cur;
			W_last = W_cur;
		}
		outJoints[4] = TransformPoint3x4(W_last, tipX, 0.0f, 0.0f);
	}

	bool GetVisualIndexTip(
		VR* vr,
		bool physicalLeftHand,
		const Vector& eyePosition,
		Vector& outVisualTip,
		Vector* outRawTip,
		Vector* outVisualPalm)
	{
		if (!vr || !vr->m_IsVREnabled)
			return false;

		VRHandGripPose hpVis{}, hpRaw{};
		const bool visValid = ComputeHandGripPose(vr, physicalLeftHand, eyePosition, hpVis, true) && hpVis.valid;
		const bool rawValid = ComputeHandGripPose(vr, physicalLeftHand, eyePosition, hpRaw, false) && hpRaw.valid;
		if (!visValid && !rawValid)
			return false;
		if (!visValid)
			hpVis = hpRaw;
		if (!rawValid)
			hpRaw = hpVis;

		const float* curls = physicalLeftHand ? vr->m_LeftFingerCurl : vr->m_RightFingerCurl;
		const float* splays = physicalLeftHand ? vr->m_LeftFingerSplay : vr->m_RightFingerSplay;

		if (outRawTip)
		{
			Vector rawJoints[5]{};
			EvaluateFingerWorldChain(hpRaw, 1, curls[1], splays[0], rawJoints, nullptr);
			*outRawTip = rawJoints[4];
		}

		const int handIdx = physicalLeftHand ? 1 : 0;
		const bool isLeftMesh = hpVis.isLeftHandMesh;
		const bool usePhoneOpen = hpVis.usePhoneOpenTable;
		const unsigned long long nowMs = GetTickCount64();

		HandSurfaceFingerPose surfPose{};
		if (s_SurfaceFingerSlots[handIdx].Read(surfPose, nowMs, 250ULL) && surfPose.blendAlpha > 0.005f)
		{
			const float c1 = ClampF(curls[1], 0.0f, 1.0f);
			const float baseSplaySign = isLeftMesh ? 1.0f : -1.0f;
			const float baseSplay = baseSplaySign * (ClampF(splays[0], -1.0f, 1.0f) * 10.0f + (0.5f - c1) * kSplayWeights[0] * 4.0f);
			float userDeg[4]{};
			EvalBareFingerCurlDeg(0, c1, userDeg);

			float deg[4]{};
			for (int k = 0; k < 4; ++k)
				deg[k] = userDeg[k] + (surfPose.fingerCurlDeg[0][k] - userDeg[k]) * surfPose.blendAlpha;
			const float splayDeg = baseSplay + (surfPose.fingerSplayDeg[0] - baseSplay) * surfPose.blendAlpha;

			Vector visJoints[5]{};
			if (isLeftMesh && usePhoneOpen)
			{
				EvalMirroredPhoneLeftFingerFromWrist(hpVis.wristToWorld, 0, deg, splayDeg, visJoints);
			}
			else
			{
				const auto& openFingers = isLeftMesh
					? s_OpenCamLeftFingers
					: (usePhoneOpen ? s_OpenPhoneRightFingers : s_OpenCamRightFingers);
				const float (&tipExtents)[5] = isLeftMesh ? kLeftTipExtent : kRightTipExtent;
				matrix3x4_t wTmp[4]{};
				EvalFingerWorldChainFromWrist(hpVis.wristToWorld, openFingers[0], tipExtents[1], deg, splayDeg, wTmp, visJoints);
			}
			outVisualTip = visJoints[4];
		}
		else
		{
			Vector visJoints[5]{};
			EvaluateFingerWorldChain(hpVis, 1, curls[1], splays[0], visJoints, nullptr);
			outVisualTip = visJoints[4];
		}

		if (outVisualPalm)
			*outVisualPalm = hpVis.palmSkinWorldPos;
		return true;
	}

	void ResetFlashlightGripAnim()
	{
		std::lock_guard<std::mutex> lock(s_FlashlightGripAnimMutex);
		s_FlashlightGripAnim.snapPending = true;
		s_FlashlightGripAnim.hasTime = false;
		s_FlashlightGripAnim.lastEyePass = 0;
		if (g_Game && g_Game->m_VR)
		{
			s_FlashlightGripAnim.blend = g_Game->m_VR->m_FlashlightOverhandGrip ? 1.0f : 0.0f;
			s_FlashlightGripAnim.initialized = true;
		}
		else
		{
			s_FlashlightGripAnim.initialized = false;
		}
	}

	bool ApplyViewModelHandRig(
		VR* vr,
		Game* game,
		void* dmsState,
		const ModelRenderInfo_t& info,
		matrix3x4_t* activeBones,
		const float* maxFingerCurlOverride,
		ViewModelLensInfo* outLensInfo)
	{
		if (outLensInfo)
			*outLensInfo = ViewModelLensInfo{};

		if (!vr || !vr->m_IsVREnabled || !vr->m_ShowHands || !game || !game->m_ModelInfo || !dmsState || !info.pModel || !activeBones)
			return false;

		const DrawModelStateSubset* dms = static_cast<const DrawModelStateSubset*>(dmsState);
		const int numBones = (dms->m_pStudioHdr) ? *reinterpret_cast<const int*>(reinterpret_cast<uintptr_t>(dms->m_pStudioHdr) + 156) : 0;
		if (numBones < 36 || numBones > 128)
			return false;

		const char* rawModelName = game->m_ModelInfo->GetModelName(const_cast<model_t*>(info.pModel));
		if (!rawModelName || std::strncmp(rawModelName, "models/weapons/v_", 17) != 0)
			return false;

		const char* vmSub = rawModelName + 17;
		const bool isCameraVm     = (std::strncmp(vmSub, "camera.mdl", 10) == 0);
		const bool isPhoneVm      = (!isCameraVm && std::strncmp(vmSub, "phone.mdl", 9) == 0);
		const bool isLightVm      = (!isCameraVm && !isPhoneVm && std::strncmp(vmSub, "light.mdl", 9) == 0);
		const bool isSuperLightVm = (!isCameraVm && !isPhoneVm && !isLightVm && std::strncmp(vmSub, "superlight.mdl", 14) == 0);

		if (!isCameraVm && !isPhoneVm && !isLightVm && !isSuperLightVm)
			return false;

		// Identify whether this DrawModelExecute call is drawing client ViewModel 0 (Right Hand / Camera / Phone)
		// or ViewModel 1 (Left Hand / Flashlight), and resolve live client weapon state on vm0 so bare-hand
		// finger extension never stalls before the camera is equipped for the first time.
		bool isDrawingVm0 = (isCameraVm || isPhoneVm);
		bool isBareRightHand = vr->m_RightHandEmpty;

		if (dms->m_pRenderable && game->m_BaseClient && game->m_EngineClient)
		{
			typedef void* (__thiscall *tGetViewModel)(void* thisptr, int index);
			typedef void* (__thiscall *tClientGetOwningWeapon)(void* thisptr);

			auto fnGetViewModel          = reinterpret_cast<tGetViewModel>(game->m_BaseClient + 0x55260);
			auto fnClientGetOwningWeapon = reinterpret_cast<tClientGetOwningWeapon>(game->m_BaseClient + 0x15FE0);

			const int localIdx = game->m_EngineClient->GetLocalPlayer();
			void* localPlayer = (localIdx > 0) ? game->GetClientEntity(localIdx) : nullptr;
			if (localPlayer)
			{
				void* clientVm0 = fnGetViewModel(localPlayer, 0);
				void* clientVm1 = fnGetViewModel(localPlayer, 1);
				if (clientVm0 && dms->m_pRenderable == reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(clientVm0) + 4))
				{
					isDrawingVm0 = true;
					if (isLightVm || isSuperLightVm)
					{
						// v_light / v_superlight on vm0 only ever occurs via SelectBareHandModel
						isBareRightHand = true;
					}
					else
					{
						void* clientWeapon = fnClientGetOwningWeapon(clientVm0);
						const uintptr_t handsVtable  = game->m_BaseClient + 0x4DDDF4; // C_INFRAHands
						const uintptr_t cameraVtable = game->m_BaseClient + 0x4DAFB4; // C_INFRACamera
						const uintptr_t phoneVtable  = game->m_BaseClient + 0x4DFB1C; // C_INFRAPhone
						if (!clientWeapon || *reinterpret_cast<const uintptr_t*>(clientWeapon) == handsVtable)
							isBareRightHand = true;
						else if (*reinterpret_cast<const uintptr_t*>(clientWeapon) == cameraVtable ||
						         *reinterpret_cast<const uintptr_t*>(clientWeapon) == phoneVtable)
							isBareRightHand = false;
					}
				}
				else if (clientVm1 && dms->m_pRenderable == reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(clientVm1) + 4))
				{
					isDrawingVm0 = false;
				}
			}
		}

		// Update viewmodel material visibility flags
		static PinnedMaterialSlot s_BodyMatSlot;
		if (IMaterial* bodyMat = s_BodyMatSlot.Get(game->m_MaterialSystem, "models/characters/player_body_work", "Model textures"))
			bodyMat->SetMaterialVarFlag(MATERIAL_VAR_NO_DRAW, true);

		static PinnedMaterialSlot s_SecondaryPropMatSlots[3];
		static const char* const s_SecondaryPropMats[3] = {
			"models/weapons/v_camera/maglite",
			"models/weapons/v_light/camera",
			"models/weapons/v_superlight/camera"
		};
		for (int i = 0; i < 3; ++i)
		{
			if (IMaterial* mat = s_SecondaryPropMatSlots[i].Get(game->m_MaterialSystem, s_SecondaryPropMats[i], "Model textures"))
				mat->SetMaterialVarFlag(MATERIAL_VAR_NO_DRAW, true);
		}

		if (isCameraVm)
		{
			// Keep v_camera/camera drawable because w_camera.mdl on the belt shares the same material;
			// bare viewmodel hides the camera prop by collapsing bones 10 & 11 into the wrist.
			static PinnedMaterialSlot s_DmeCamMatSlot;
			if (IMaterial* camMat = s_DmeCamMatSlot.Get(game->m_MaterialSystem, "models/weapons/v_camera/camera", "Model textures"))
				camMat->SetMaterialVarFlag(MATERIAL_VAR_NO_DRAW, false);
		}
		else if (isPhoneVm)
		{
			static PinnedMaterialSlot s_DmePhoneMatSlot;
			if (IMaterial* phoneMat = s_DmePhoneMatSlot.Get(game->m_MaterialSystem, "models/weapons/v_phone/mobile_phone_001", "Model textures"))
				phoneMat->SetMaterialVarFlag(MATERIAL_VAR_NO_DRAW, !vr->m_PhoneEquipped);
		}
		else if ((isLightVm || isSuperLightVm) && !isDrawingVm0)
		{
			// Keep v_light/maglite and v_superlight/led_flashlight* drawable because w_light.mdl / w_superlight.mdl
			// on the belt share these materials; bare viewmodel collapses prop bones 34/35 & 55/56 into the wrist.
			static PinnedMaterialSlot s_DmeFlashlightMatSlots[5];
			static const char* const s_FlashlightMats[5] = {
				"models/weapons/v_light/maglite",
				"models/weapons/v_superlight/led_flashlight",
				"models/weapons/v_superlight/led_flashlight_backplate",
				"models/weapons/v_superlight/led_flashlight_backplate_skin1",
				"models/weapons/v_superlight/led_flashlight_backplate_skin2"
			};
			for (int i = 0; i < 5; ++i)
			{
				if (IMaterial* flMat = s_DmeFlashlightMatSlots[i].Get(game->m_MaterialSystem, s_FlashlightMats[i], "Model textures"))
					flMat->SetMaterialVarFlag(MATERIAL_VAR_NO_DRAW, false);
			}
		}

		matrix3x4_t backupBones[128];
		std::memcpy(backupBones, activeBones, sizeof(matrix3x4_t) * numBones);

		const matrix3x4_t* pModelToWorld = dms->m_pModelToWorld ? dms->m_pModelToWorld : info.pModelToWorld;
		const unsigned long long nowMs = GetTickCount64();

		// Resolve per-hand curl clamping for held physics objects
		float clampBuf[5] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
		const bool physicalLeftForHand = isDrawingVm0 ? vr->m_LeftHanded : !vr->m_LeftHanded;
		const float* effectiveMaxCurl = maxFingerCurlOverride;
		if (!effectiveMaxCurl && GetFingerCurlLimits(physicalLeftForHand, clampBuf))
			effectiveMaxCurl = clampBuf;

		HandSurfaceFingerPose surfacePose{};
		const HandSurfaceFingerPose* effectiveSurfacePose = nullptr;
		if (!effectiveMaxCurl &&
			s_SurfaceFingerSlots[physicalLeftForHand ? 1 : 0].Read(surfacePose, nowMs, 250ULL) &&
			surfacePose.blendAlpha > 0.002f)
		{
			effectiveSurfacePose = &surfacePose;
		}

		auto applyVisualWristOverride = [&](int wristBone, bool cachePhoneOpen, HandSkinPreset preset, bool allowBoneShift = true)
		{
			if (pModelToWorld && wristBone >= 0 && wristBone < numBones)
			{
				matrix3x4_t mInv{}, mToWrist{};
				InvertOrtho3x4(*pModelToWorld, mInv);
				Mul3x4(mInv, backupBones[wristBone], mToWrist);
				s_ModelToWristCache[isDrawingVm0 ? 0 : 1].Write(mToWrist, cachePhoneOpen, preset, nowMs);
			}

			if (!allowBoneShift || wristBone < 0 || wristBone >= numBones || !IsRotationBasisValid(backupBones[wristBone]))
				return;

			// dCalcViewModelView already bakes s_VisualWristSlots delta into Frame N's *pModelToWorld so viewmodel geometry
			// and attached C_VGuiScreen panels move in lockstep while walking. Only apply fallback held-object coupling if the slot is empty.
			matrix3x4_t visualWrist{}, mDelta{};
			bool haveDelta = false;
			if (!s_VisualWristSlots[physicalLeftForHand ? 1 : 0].Read(visualWrist, nullptr, nullptr, nowMs, 250ULL) &&
			    vr->m_IsHoldingObject)
			{
				HeldHandPhysicsFeedback fb{};
				if (s_HeldPhysicsSlot.Read(fb, nowMs, 250ULL) && fb.holdingLeftHand == physicalLeftForHand)
				{
					float alpha = 0.0f;
					ComputeCoupledHeldWrist(fb, backupBones[wristBone], visualWrist, alpha);
					if (alpha > 0.001f)
					{
						matrix3x4_t incomingInv{};
						InvertOrtho3x4(backupBones[wristBone], incomingInv);
						Mul3x4(visualWrist, incomingInv, mDelta);
						haveDelta = true;
					}
				}
			}
			if (!haveDelta)
				return;

			const float posDiff = std::fabs(mDelta[0][3]) + std::fabs(mDelta[1][3]) + std::fabs(mDelta[2][3]);
			const float traceR = mDelta[0][0] + mDelta[1][1] + mDelta[2][2];
			if (posDiff <= 0.002f && traceR >= 2.99995f)
				return;

			for (int b = 0; b < numBones; ++b)
			{
				matrix3x4_t tmpBackup{}, tmpActive{};
				Mul3x4(mDelta, backupBones[b], tmpBackup);
				Mul3x4(mDelta, activeBones[b], tmpActive);
				backupBones[b] = tmpBackup;
				activeBones[b] = tmpActive;
			}
		};

		if (isDrawingVm0)
		{
			const auto& rCurls  = vr->m_LeftHanded ? vr->m_LeftFingerCurl  : vr->m_RightFingerCurl;
			const auto& rSplays = vr->m_LeftHanded ? vr->m_LeftFingerSplay : vr->m_RightFingerSplay;

			if (isCameraVm)
			{
				if (isBareRightHand && pModelToWorld)
				{
					Mul3x4(*pModelToWorld, s_CamRightForearm, backupBones[8]);
					Mul3x4(*pModelToWorld, s_CamRightHand,    backupBones[9]);
					activeBones[8] = backupBones[8];
					activeBones[9] = backupBones[9];
				}
				applyVisualWristOverride(9, false, HandSkinPreset::RightCam, isBareRightHand);

				const float wx = backupBones[8][0][3], wy = backupBones[8][1][3], wz = backupBones[8][2][3];
				for (int b = 0; b <= 7; ++b)
					CollapseBoneToPoint(activeBones, numBones, b, wx, wy, wz);
				for (int b = 31; b < numBones; ++b)
					CollapseBoneToPoint(activeBones, numBones, b, wx, wy, wz);

				static constexpr int kCamRightThumb[3] = { 28, 29, 30 };
				static constexpr int kCamRightFingers[4][4] = {
					{ 24, 25, 26, 27 }, // Index
					{ 20, 21, 22, 23 }, // Middle
					{ 16, 17, 18, 19 }, // Ring
					{ 12, 13, 14, 15 }  // Pinky
				};
				AnimateHandRig(numBones, 9, kCamRightThumb, kCamRightFingers, backupBones, activeBones,
				               rCurls, rSplays, effectiveMaxCurl, false, isBareRightHand, false, effectiveSurfacePose);

				if (isBareRightHand)
				{
					CollapseBoneToPoint(activeBones, numBones, 10, wx, wy, wz);
					CollapseBoneToPoint(activeBones, numBones, 11, wx, wy, wz);
					g_ViewModelLens.cameraUpdated = false;
					if (outLensInfo)
						outLensInfo->cameraUpdated = false;
				}
				else if (numBones > 10 && IsRotationBasisValid(activeBones[10]))
				{
					const matrix3x4_t& W10 = activeBones[10];
					const Vector camBody(W10[0][3], W10[1][3], W10[2][3]);
					const Vector camFlash = TransformPoint3x4(W10, -1.65f, -0.48f, 1.08f);
					const Vector camFlashN = NormalizeSafe(Vector(-W10[0][1], -W10[1][1], -W10[2][1]), Vector(0.0f, -1.0f, 0.0f));
					const Vector camLed = TransformPoint3x4(W10, -2.05f, 0.43f, 1.25f);
					const Vector camLedN = NormalizeSafe(Vector(W10[0][1], W10[1][1], W10[2][1]), Vector(0.0f, 1.0f, 0.0f));

					g_ViewModelLens.camBodyPos = camBody;
					g_ViewModelLens.camFlashPos = camFlash;
					g_ViewModelLens.camFlashNormal = camFlashN;
					g_ViewModelLens.camLedPos = camLed;
					g_ViewModelLens.camLedNormal = camLedN;
					g_ViewModelLens.cameraUpdated = true;

					if (outLensInfo)
					{
						outLensInfo->camBodyPos = camBody;
						outLensInfo->camFlashPos = camFlash;
						outLensInfo->camFlashNormal = camFlashN;
						outLensInfo->camLedPos = camLed;
						outLensInfo->camLedNormal = camLedN;
						outLensInfo->cameraUpdated = true;
					}
				}
				else
				{
					g_ViewModelLens.cameraUpdated = false;
					if (outLensInfo)
						outLensInfo->cameraUpdated = false;
				}
			}
			else if (isPhoneVm)
			{
				// Reposition v_phone.mdl's bare Right Hand from @idle01 (59.08-deg wrist twist) to the calibrated VR pose
				if (isBareRightHand && pModelToWorld)
				{
					Mul3x4(*pModelToWorld, s_PhoneRightForearm, backupBones[31]);
					Mul3x4(*pModelToWorld, s_CamRightHand,      backupBones[32]);
					activeBones[31] = backupBones[31];
					activeBones[32] = backupBones[32];
				}
				applyVisualWristOverride(32, isBareRightHand, isBareRightHand ? HandSkinPreset::RightPhone : HandSkinPreset::RightCam, isBareRightHand);

				if (!isBareRightHand && numBones > 53 && IsRotationBasisValid(backupBones[53]))
				{
					PhoneScreenQuad pq{};
					pq.ul = TransformPoint3x4(backupBones[53], 0.200f,  2.090f,  1.130f);
					const Vector ur = TransformPoint3x4(backupBones[53], 0.200f,  2.090f, -1.140f);
					const Vector ll = TransformPoint3x4(backupBones[53], 0.200f, -1.750f,  1.130f);
					pq.rightVec = ur - pq.ul;
					pq.downVec = ll - pq.ul;
					pq.widthLenSq = DotProduct(pq.rightVec, pq.rightVec);
					pq.heightLenSq = DotProduct(pq.downVec, pq.downVec);
					pq.normal = NormalizeSafe(
						Vector(backupBones[53][0][0], backupBones[53][1][0], backupBones[53][2][0]),
						Vector(0.0f, 0.0f, 1.0f));
					s_PhoneScreenSlot.Write(pq, nowMs);
				}
				else
				{
					s_PhoneScreenSlot.Clear();
				}

				const float wx = backupBones[31][0][3], wy = backupBones[31][1][3], wz = backupBones[31][2][3];
				for (int b = 0; b <= 30; ++b)
					CollapseBoneToPoint(activeBones, numBones, b, wx, wy, wz);

				static constexpr int kPhoneRightThumb[3] = { 33, 34, 35 };
				static constexpr int kPhoneRightFingers[4][4] = {
					{ 36, 37, 38, 39 }, // Index
					{ 40, 41, 42, 43 }, // Middle
					{ 44, 45, 46, 47 }, // Ring
					{ 48, 49, 50, 51 }  // Pinky
				};
				AnimateHandRig(numBones, 32, kPhoneRightThumb, kPhoneRightFingers, backupBones, activeBones,
				               rCurls, rSplays, effectiveMaxCurl, false, isBareRightHand, true, effectiveSurfacePose);

				if (isBareRightHand)
				{
					CollapseBoneToPoint(activeBones, numBones, 52, wx, wy, wz);
					CollapseBoneToPoint(activeBones, numBones, 53, wx, wy, wz);
				}
			}
			else if (isLightVm)
			{
				if (pModelToWorld)
				{
					Mul3x4(*pModelToWorld, s_CamRightForearm, backupBones[8]);
					Mul3x4(*pModelToWorld, s_CamRightHand,    backupBones[9]);
					activeBones[8] = backupBones[8];
					activeBones[9] = backupBones[9];
				}
				applyVisualWristOverride(9, false, HandSkinPreset::RightCam);

				const float wx = backupBones[8][0][3], wy = backupBones[8][1][3], wz = backupBones[8][2][3];
				for (int b = 0; b <= 7; ++b)
					CollapseBoneToPoint(activeBones, numBones, b, wx, wy, wz);
				CollapseBoneToPoint(activeBones, numBones, 10, wx, wy, wz);
				for (int b = 30; b < numBones; ++b)
					CollapseBoneToPoint(activeBones, numBones, b, wx, wy, wz);

				static constexpr int kLightRightThumb[3] = { 27, 28, 29 };
				static constexpr int kLightRightFingers[4][4] = {
					{ 23, 24, 25, 26 }, // Index
					{ 19, 20, 21, 22 }, // Middle
					{ 15, 16, 17, 18 }, // Ring
					{ 11, 12, 13, 14 }  // Pinky
				};
				AnimateHandRig(numBones, 9, kLightRightThumb, kLightRightFingers, backupBones, activeBones,
				               rCurls, rSplays, effectiveMaxCurl, false, true, false, effectiveSurfacePose);
			}
			else if (isSuperLightVm)
			{
				// v_superlight.mdl shifts Right Hand fingers to 10..28 and places camera prop Mesh002 at bone 29
				if (pModelToWorld)
				{
					Mul3x4(*pModelToWorld, s_CamRightForearm, backupBones[8]);
					Mul3x4(*pModelToWorld, s_CamRightHand,    backupBones[9]);
					activeBones[8] = backupBones[8];
					activeBones[9] = backupBones[9];
				}
				applyVisualWristOverride(9, false, HandSkinPreset::RightSuper);

				const float wx = backupBones[8][0][3], wy = backupBones[8][1][3], wz = backupBones[8][2][3];
				for (int b = 0; b <= 7; ++b)
					CollapseBoneToPoint(activeBones, numBones, b, wx, wy, wz);
				for (int b = 29; b < numBones; ++b)
					CollapseBoneToPoint(activeBones, numBones, b, wx, wy, wz);

				static constexpr int kSuperLightRightThumb[3] = { 26, 27, 28 };
				static constexpr int kSuperLightRightFingers[4][4] = {
					{ 22, 23, 24, 25 }, // Index
					{ 18, 19, 20, 21 }, // Middle
					{ 14, 15, 16, 17 }, // Ring
					{ 10, 11, 12, 13 }  // Pinky
				};
				AnimateHandRig(numBones, 9, kSuperLightRightThumb, kSuperLightRightFingers, backupBones, activeBones,
				               rCurls, rSplays, effectiveMaxCurl, false, true, false, effectiveSurfacePose);
			}

			if (!isCameraVm)
			{
				g_ViewModelLens.cameraUpdated = false;
				if (outLensInfo)
					outLensInfo->cameraUpdated = false;
			}
		}
		else if (isPhoneVm)
		{
			// Bare Left Hand on vm1: render v_phone.mdl's clean Right Hand rig (bones 31..51)
			// animated in Left Hand wrist space and reflected across the wrist Z=0 mirror plane.
			if (pModelToWorld)
			{
				Mul3x4(*pModelToWorld, s_PhoneLeftForearm, backupBones[8]);
				Mul3x4(*pModelToWorld, s_LightLeftHand,    backupBones[9]);
				activeBones[8] = backupBones[8];
				activeBones[9] = backupBones[9];
			}
			applyVisualWristOverride(9, true, HandSkinPreset::LeftPhone, true);

			matrix3x4_t invCamRight{}, forearmRel{};
			InvertOrtho3x4(s_CamRightHand, invCamRight);
			Mul3x4(invCamRight, s_PhoneRightForearm, forearmRel);

			const matrix3x4_t virtWrist = backupBones[9];
			backupBones[32] = virtWrist;
			activeBones[32] = virtWrist;
			Mul3x4(virtWrist, forearmRel, backupBones[31]);
			activeBones[31] = backupBones[31];

			static constexpr int kPhoneRightThumb[3] = { 33, 34, 35 };
			static constexpr int kPhoneRightFingers[4][4] = {
				{ 36, 37, 38, 39 }, // Index
				{ 40, 41, 42, 43 }, // Middle
				{ 44, 45, 46, 47 }, // Ring
				{ 48, 49, 50, 51 }  // Pinky
			};
			const auto& lCurls  = vr->m_LeftHanded ? vr->m_RightFingerCurl  : vr->m_LeftFingerCurl;
			const auto& lSplays = vr->m_LeftHanded ? vr->m_RightFingerSplay : vr->m_LeftFingerSplay;

			HandSurfaceFingerPose mirroredSurfPose{};
			const HandSurfaceFingerPose* rightSurfPose = effectiveSurfacePose;
			if (effectiveSurfacePose)
			{
				mirroredSurfPose = *effectiveSurfacePose;
				for (int f = 0; f < 4; ++f)
					mirroredSurfPose.fingerSplayDeg[f] = -mirroredSurfPose.fingerSplayDeg[f];
				mirroredSurfPose.thumbSplayDeg[0] = -mirroredSurfPose.thumbSplayDeg[0];
				mirroredSurfPose.thumbSplayDeg[1] = -mirroredSurfPose.thumbSplayDeg[1];
				float leftNomThumb[3]{}, rightNomThumb[3]{};
				EvalBareThumbCurlDeg(lCurls[0], leftNomThumb, true);
				EvalBareThumbCurlDeg(lCurls[0], rightNomThumb, false);
				for (int k = 0; k < 3; ++k)
					mirroredSurfPose.thumbCurlDeg[k] = rightNomThumb[k] + (mirroredSurfPose.thumbCurlDeg[k] - leftNomThumb[k]);
				rightSurfPose = &mirroredSurfPose;
			}

			AnimateHandRig(numBones, 32, kPhoneRightThumb, kPhoneRightFingers, backupBones, activeBones,
			               lCurls, lSplays, effectiveMaxCurl, false, true, true, rightSurfPose);

			matrix3x4_t invVirtWrist{};
			InvertOrtho3x4(virtWrist, invVirtWrist);
			for (int b = 31; b <= 51 && b < numBones; ++b)
			{
				matrix3x4_t localB{};
				Mul3x4(invVirtWrist, activeBones[b], localB);
				localB[2][0] = -localB[2][0];
				localB[2][1] = -localB[2][1];
				localB[2][2] = -localB[2][2];
				localB[2][3] = -localB[2][3];
				Mul3x4(virtWrist, localB, activeBones[b]);
			}

			const float wx = activeBones[31][0][3], wy = activeBones[31][1][3], wz = activeBones[31][2][3];
			for (int b = 0; b <= 30; ++b)
				CollapseBoneToPoint(activeBones, numBones, b, wx, wy, wz);
			for (int b = 52; b < numBones; ++b)
				CollapseBoneToPoint(activeBones, numBones, b, wx, wy, wz);

			if (outLensInfo)
				outLensInfo->flipCull = true;

			SyncFlashlightGripWhenBare(vr);
			g_ViewModelLens.updated = false;
			g_ViewModelLens.tailUpdated = false;
			if (outLensInfo)
			{
				outLensInfo->updated = false;
				outLensInfo->tailUpdated = false;
			}
		}
		else if (isLightVm || isSuperLightVm)
		{
			// Left Hand on vm1 (bone 32 = Bip001 L Forearm, bone 33 = Bip001 L Hand, 36..54 = L Fingers)
			const bool isBareLeftHand = !vr->m_FlashlightEquipped;
			if (isBareLeftHand && pModelToWorld)
			{
				Mul3x4(*pModelToWorld, s_LightLeftForearm, backupBones[32]);
				Mul3x4(*pModelToWorld, s_LightLeftHand,    backupBones[33]);
				activeBones[32] = backupBones[32];
				activeBones[33] = backupBones[33];
			}
			applyVisualWristOverride(33, false, isSuperLightVm ? HandSkinPreset::LeftSuper : HandSkinPreset::LeftLight);

			const float wx = backupBones[33][0][3], wy = backupBones[33][1][3], wz = backupBones[33][2][3];
			for (int b = 0; b <= 31; ++b)
				CollapseBoneToPoint(activeBones, numBones, b, wx, wy, wz);

			static constexpr int kLeftThumb[3] = { 52, 53, 54 };
			static constexpr int kLeftFingers[4][4] = {
				{ 48, 49, 50, 51 }, // Index
				{ 44, 45, 46, 47 }, // Middle
				{ 40, 41, 42, 43 }, // Ring
				{ 36, 37, 38, 39 }  // Pinky
			};
			const auto& lCurls  = vr->m_LeftHanded ? vr->m_RightFingerCurl  : vr->m_LeftFingerCurl;
			const auto& lSplays = vr->m_LeftHanded ? vr->m_RightFingerSplay : vr->m_LeftFingerSplay;
			AnimateHandRig(numBones, 33, kLeftThumb, kLeftFingers, backupBones, activeBones,
			               lCurls, lSplays, effectiveMaxCurl, true, isBareLeftHand, false, effectiveSurfacePose);

			if (isBareLeftHand)
			{
				CollapseBoneToPoint(activeBones, numBones, 34, wx, wy, wz);
				CollapseBoneToPoint(activeBones, numBones, 35, wx, wy, wz);
				CollapseBoneToPoint(activeBones, numBones, 55, wx, wy, wz);
				CollapseBoneToPoint(activeBones, numBones, 56, wx, wy, wz);
				SyncFlashlightGripWhenBare(vr);
				g_ViewModelLens.updated = false;
				g_ViewModelLens.tailUpdated = false;
				if (outLensInfo)
				{
					outLensInfo->updated = false;
					outLensInfo->tailUpdated = false;
				}
			}
			else
			{
				const int propBone  = isSuperLightVm ? 55 : 34;
				const int childBone = isSuperLightVm ? 56 : 35;

				// Verified v_superlight.mdl (bone 55 Tube002): flared reflector/lens is at -Z (Z=-0.30, r=1.75),
				// tail cap / led_flashlight_backplate (bone 56) is at +Z (Z=+12.30..+12.50, r=0.74..0.85).
				const Vector localPivot = isSuperLightVm
					? Vector(0.0f, 0.0f, 6.100f)
					: Vector(-0.1565f, 0.2275f, 0.3510f);
				const Vector localBarrel = isSuperLightVm
					? Vector(0.0f, 0.0f, -1.0f)
					: NormalizeSafe(Vector(0.0831f, -0.9892f, -0.1201f), Vector(0.0f, -1.0f, 0.0f));
				const Vector localLens = isSuperLightVm
					? Vector(0.0f, 0.0f, -0.25f)
					: Vector(0.350f, -5.803f, -0.381f);
				const Vector localTail = isSuperLightVm
					? Vector(0.0f, 0.0f, 12.46f)
					: Vector(-0.68413f, 6.14524f, 1.12118f);
				const Vector localTailNorm = isSuperLightVm
					? Vector(0.0f, 0.0f, 1.0f)
					: NormalizeSafe(Vector(-0.095191f, 0.986078f, 0.136340f), Vector(0.0f, 1.0f, 0.0f));

				Vector localTailU(1.0f, 0.0f, 0.0f);
				Vector localTailV(0.0f, 1.0f, 0.0f);
				if (!isSuperLightVm)
				{
					localTailU = NormalizeSafe(
						Vector(1.0f, 0.0f, 0.0f) - localTailNorm * localTailNorm.x,
						Vector(1.0f, 0.0f, 0.0f));
					localTailV = NormalizeSafe(
						CrossProduct(localTailNorm, localTailU),
						Vector(0.0f, 0.0f, 1.0f));
				}
				const float tailRadius = isSuperLightVm ? 0.720f : 0.6867f;

				const float flipBlend = EvaluateFlashlightGripBlend(vr);
				const float thetaDeg = flipBlend * 180.0f;
				const float overhandPitchDownDeg = flipBlend * 15.0f;
				if ((thetaDeg > 0.001f || overhandPitchDownDeg > 0.001f) &&
				    numBones > propBone && numBones > 33 && IsRotationBasisValid(activeBones[propBone]))
				{
					const matrix3x4_t& W_hand = backupBones[33];
					const matrix3x4_t& W_prop = activeBones[propBone];
					const Vector pivotWorld = TransformPoint3x4(W_prop, localPivot.x, localPivot.y, localPivot.z);
					const Vector barrelWorld = NormalizeSafe(RotateVec3x4(W_prop, localBarrel), Vector(1.0f, 0.0f, 0.0f));
					const Vector palmOutward(W_hand[0][1], W_hand[1][1], W_hand[2][1]);
					const Vector handDistal(W_hand[0][0], W_hand[1][0], W_hand[2][0]);
					Vector orthoAxis = palmOutward - barrelWorld * DotProduct(palmOutward, barrelWorld);
					if (DotProduct(orthoAxis, orthoAxis) <= 1e-4f)
						orthoAxis = handDistal - barrelWorld * DotProduct(handDistal, barrelWorld);
					const Vector axisWorld = NormalizeSafe(orthoAxis, Vector(0.0f, 0.0f, 1.0f));

					auto rotateBoneAroundAxis = [&](int b, const Vector& rotAxis, float rotDeg)
					{
						if (b < 0 || b >= numBones || !IsRotationBasisValid(activeBones[b]))
							return;
						matrix3x4_t& M = activeBones[b];
						const Vector col0 = VectorRotate(Vector(M[0][0], M[1][0], M[2][0]), rotAxis, rotDeg);
						const Vector col1 = VectorRotate(Vector(M[0][1], M[1][1], M[2][1]), rotAxis, rotDeg);
						const Vector col2 = VectorRotate(Vector(M[0][2], M[1][2], M[2][2]), rotAxis, rotDeg);
						const Vector pos(M[0][3], M[1][3], M[2][3]);
						const Vector newPos = pivotWorld + VectorRotate(pos - pivotWorld, rotAxis, rotDeg);
						M = matrix3x4_t(col0, col1, col2, newPos);
						backupBones[b] = M;
					};

					if (thetaDeg > 0.001f)
					{
						rotateBoneAroundAxis(propBone, axisWorld, thetaDeg);
						rotateBoneAroundAxis(childBone, axisWorld, thetaDeg);
					}

					if (overhandPitchDownDeg > 0.001f)
					{
						const Vector flippedBarrel = NormalizeSafe(
							RotateVec3x4(activeBones[propBone], localBarrel),
							Vector(1.0f, 0.0f, 0.0f));
						const Vector handDistalUnit = NormalizeSafe(handDistal, Vector(0.0f, 0.0f, -1.0f));
						const Vector downPerp = handDistalUnit - flippedBarrel * DotProduct(handDistalUnit, flippedBarrel);
						if (DotProduct(downPerp, downPerp) > 1e-4f)
						{
							const Vector downUnit = NormalizeSafe(downPerp, Vector(0.0f, 0.0f, -1.0f));
							const Vector pitchAxis = NormalizeSafe(CrossProduct(flippedBarrel, downUnit), axisWorld);
							rotateBoneAroundAxis(propBone, pitchAxis, overhandPitchDownDeg);
							rotateBoneAroundAxis(childBone, pitchAxis, overhandPitchDownDeg);
						}
					}
				}

				if (numBones > propBone && IsRotationBasisValid(activeBones[propBone]))
				{
					const matrix3x4_t& W_final = activeBones[propBone];
					const Vector lensPos = TransformPoint3x4(W_final, localLens.x, localLens.y, localLens.z);
					const Vector lensNorm = NormalizeSafe(RotateVec3x4(W_final, localBarrel), Vector(1.0f, 0.0f, 0.0f));
					const Vector tailCenter = TransformPoint3x4(W_final, localTail.x, localTail.y, localTail.z);
					const Vector tailNorm = NormalizeSafe(RotateVec3x4(W_final, localTailNorm), Vector(0.0f, 0.0f, -1.0f));
					const Vector tailU = NormalizeSafe(RotateVec3x4(W_final, localTailU), Vector(1.0f, 0.0f, 0.0f));
					const Vector tailV = NormalizeSafe(RotateVec3x4(W_final, localTailV), Vector(0.0f, 1.0f, 0.0f));

					g_ViewModelLens.pos = lensPos;
					g_ViewModelLens.normal = lensNorm;
					g_ViewModelLens.isSuperLight = isSuperLightVm;
					g_ViewModelLens.tailPos = tailCenter;
					g_ViewModelLens.tailNormal = tailNorm;
					g_ViewModelLens.tailAxisU = tailU;
					g_ViewModelLens.tailAxisV = tailV;
					g_ViewModelLens.tailCapRadius = tailRadius;
					g_ViewModelLens.updated = true;
					g_ViewModelLens.tailUpdated = true;

					if (outLensInfo)
					{
						outLensInfo->pos = lensPos;
						outLensInfo->normal = lensNorm;
						outLensInfo->isSuperLight = isSuperLightVm;
						outLensInfo->tailPos = tailCenter;
						outLensInfo->tailNormal = tailNorm;
						outLensInfo->tailAxisU = tailU;
						outLensInfo->tailAxisV = tailV;
						outLensInfo->tailCapRadius = tailRadius;
						outLensInfo->updated = vr->m_FlashlightBeamOn;
						outLensInfo->tailUpdated = true;
					}
				}
				else
				{
					g_ViewModelLens.updated = false;
					g_ViewModelLens.tailUpdated = false;
					if (outLensInfo)
					{
						outLensInfo->updated = false;
						outLensInfo->tailUpdated = false;
					}
				}
			}
		}

		return true;
	}

	void ApplyEquippedPhoneTilt(
		Vector& ioVmOrigin,
		Vector& ioFwd,
		Vector& ioRight,
		Vector& ioUp,
		QAngle* outAngles)
	{
		// Verified in v_phone.mdl @idle01 (vm0 space: +X = fwd, +Y = left = -right, +Z = up):
		//   bone 32 wrist pivot       = (19.615, -8.040, -6.931)
		//   bone 53 screen-right axis = (-0.3313, -0.9435, -0.0040)
		//   +25 deg rotation around screen-right axis brings the top edge (+Y of bone 53) toward the user (+X screen normal).
		static const Vector kPivotLocal(19.615f, -8.040f, -6.931f);
		static const Vector kAxisLocal(-0.3313f, -0.9435f, -0.0040f);
		constexpr float kPhoneTiltDeg = 25.0f;

		const Vector oldLeft(-ioRight.x, -ioRight.y, -ioRight.z);
		const Vector pivotWorld = ioVmOrigin + (ioFwd * kPivotLocal.x) + (oldLeft * kPivotLocal.y) + (ioUp * kPivotLocal.z);
		const Vector axisWorld = NormalizeSafe(
			(ioFwd * kAxisLocal.x) + (oldLeft * kAxisLocal.y) + (ioUp * kAxisLocal.z),
			ioRight);

		ioFwd = NormalizeSafe(VectorRotate(ioFwd, axisWorld, kPhoneTiltDeg), ioFwd);
		const Vector rotRight = VectorRotate(ioRight, axisWorld, kPhoneTiltDeg);
		ioRight = NormalizeSafe(rotRight - ioFwd * DotProduct(rotRight, ioFwd), ioRight);
		const Vector newLeft(-ioRight.x, -ioRight.y, -ioRight.z);
		ioUp = NormalizeSafe(CrossProduct(ioFwd, newLeft), ioUp);
		ioVmOrigin = pivotWorld - ((ioFwd * kPivotLocal.x) + (newLeft * kPivotLocal.y) + (ioUp * kPivotLocal.z));

		if (outAngles)
		{
			const matrix3x4_t tiltedM(ioFwd, newLeft, ioUp, ioVmOrigin);
			Vector unusedPos;
			Matrix3x4ToAngles(tiltedM, unusedPos, *outAngles);
		}
	}

	void ResetPhoneScreenTouch()
	{
		std::lock_guard<std::mutex> lock(s_PhoneTouchMutex);
		s_PhoneTouch = PhoneTouchState{};
	}

	PhoneScreenTouchResult PollPhoneScreenTouch(VR* vr, const Vector& eyePosition)
	{
		PhoneScreenTouchResult out{};
		std::lock_guard<std::mutex> lock(s_PhoneTouchMutex);
		const bool tapPhysLeft = vr ? !vr->m_LeftHanded : true;
		out.tappingLeftHand = tapPhysLeft;

		const unsigned long long nowMs = GetTickCount64();
		const auto steadyNow = std::chrono::steady_clock::now();

		PhoneScreenQuad pq{};
		const bool hasQuad = vr && vr->m_IsVREnabled && vr->m_PhoneEquipped &&
			s_PhoneScreenSlot.Read(pq, nowMs, 250ULL);
		if (!hasQuad)
		{
			if (s_PhoneTouch.pendingRelease)
			{
				out.releasedEdge = true;
				out.u = s_PhoneTouch.lockedU;
				out.v = s_PhoneTouch.lockedV;
			}
			s_PhoneTouch = PhoneTouchState{};
			return out;
		}

		const bool isBareTapHand = tapPhysLeft ? !vr->m_FlashlightEquipped : vr->m_RightHandEmpty;
		const bool holdingWithTapHand = vr->m_IsHoldingObject && (vr->m_HoldingWithLeftHand == tapPhysLeft);
		if (!isBareTapHand || holdingWithTapHand)
		{
			if (s_PhoneTouch.pendingRelease)
			{
				out.releasedEdge = true;
				out.u = s_PhoneTouch.lockedU;
				out.v = s_PhoneTouch.lockedV;
			}
			s_PhoneTouch = PhoneTouchState{};
			return out;
		}

		if (s_PhoneTouch.pendingRelease)
		{
			out.hovering = true;
			out.u = s_PhoneTouch.lockedU;
			out.v = s_PhoneTouch.lockedV;
			if (nowMs >= s_PhoneTouch.pressStartMs && (nowMs - s_PhoneTouch.pressStartMs) >= 45ULL)
			{
				out.releasedEdge = true;
				s_PhoneTouch.pendingRelease = false;
			}
			return out;
		}

		VRHandGripPose tapPose{};
		if (!ComputeHandGripPose(vr, tapPhysLeft, eyePosition, tapPose, false) || !tapPose.valid)
			return out;

		const float indexCurl = ClampF((tapPhysLeft ? vr->m_LeftFingerCurl[1] : vr->m_RightFingerCurl[1]), 0.0f, 1.0f);
		const float indexSplay = ClampF((tapPhysLeft ? vr->m_LeftFingerSplay[0] : vr->m_RightFingerSplay[0]), -1.0f, 1.0f);
		Vector idxJoints[5]{};
		EvaluateFingerWorldChain(tapPose, 1, indexCurl, indexSplay, idxJoints, nullptr);
		const Vector& tipPos = idxJoints[4];

		float tipSpeedMps = 0.0f;
		if (s_PhoneTouch.hasPrevTip)
		{
			const float tipDt = std::chrono::duration<float>(steadyNow - s_PhoneTouch.prevTipTime).count();
			if (tipDt >= 0.0005f && tipDt <= 0.25f)
			{
				const float vrScale = (vr->m_VRScale > 1.0f && vr->m_VRScale < 300.0f) ? vr->m_VRScale : 43.2f;
				tipSpeedMps = (VecLen3(tipPos - s_PhoneTouch.prevTipPos) / tipDt) / vrScale;
			}
		}
		s_PhoneTouch.prevTipPos = tipPos;
		s_PhoneTouch.prevTipTime = steadyNow;
		s_PhoneTouch.hasPrevTip = true;

		const Vector rel = tipPos - pq.ul;
		const float d = DotProduct(rel, pq.normal);
		const float uRaw = DotProduct(rel, pq.rightVec) / pq.widthLenSq;
		const float vRaw = DotProduct(rel, pq.downVec) / pq.heightLenSq;

		const bool inHover = (uRaw >= -0.08f && uRaw <= 1.08f &&
		                      vRaw >= -0.08f && vRaw <= 1.08f &&
		                      d >= -1.20f && d <= 2.80f);
		if (inHover)
		{
			out.hovering = true;
			out.u = ClampF(uRaw, 0.0f, 1.0f);
			out.v = ClampF(vRaw, 0.0f, 1.0f);
		}

		if (d >= 0.90f || !inHover)
		{
			if (!s_PhoneTouch.hasClearSince)
			{
				s_PhoneTouch.hasClearSince = true;
				s_PhoneTouch.clearSince = steadyNow;
			}
			else
			{
				const float clearMs = std::chrono::duration<float, std::milli>(steadyNow - s_PhoneTouch.clearSince).count();
				if (clearMs >= 70.0f)
					s_PhoneTouch.armed = true;
			}
		}
		else
		{
			s_PhoneTouch.hasClearSince = false;
		}

		const bool inTapBounds = (uRaw >= -0.03f && uRaw <= 1.03f &&
		                          vRaw >= -0.03f && vRaw <= 1.03f &&
		                          d <= 0.45f && d >= -1.20f);
		const bool cooldownOk = (s_PhoneTouch.lastTapMs == 0ULL) ||
			(nowMs >= s_PhoneTouch.lastTapMs && (nowMs - s_PhoneTouch.lastTapMs) >= 280ULL);
		if (s_PhoneTouch.armed && cooldownOk && indexCurl < 0.65f && inTapBounds)
		{
			out.hovering = true;
			out.pressedEdge = true;
			out.u = ClampF(uRaw, 0.0f, 1.0f);
			out.v = ClampF(vRaw, 0.0f, 1.0f);

			s_PhoneTouch.lockedU = out.u;
			s_PhoneTouch.lockedV = out.v;
			s_PhoneTouch.pendingRelease = true;
			s_PhoneTouch.armed = false;
			s_PhoneTouch.hasClearSince = false;
			s_PhoneTouch.pressStartMs = nowMs;
			s_PhoneTouch.lastTapMs = nowMs;

			const float ctrlSpeedMps = tapPhysLeft ? vr->m_LeftHandSpeedMps : vr->m_RightHandSpeedMps;
			const float tapImpactMps = std::max(tipSpeedMps, ctrlSpeedMps);
			const float tapAmp = VR::ComputeImpactHapticAmplitude(tapImpactMps, 0.08f, 2.3f);
			const float holdAmp = ClampF(tapAmp * 0.55f, 0.08f, 0.65f);
			vr->TriggerHapticPulseOnHand(tapPhysLeft, 850, tapAmp);
			vr->TriggerHapticPulseOnHand(!tapPhysLeft, 750, holdAmp);
		}

		return out;
	}
}
