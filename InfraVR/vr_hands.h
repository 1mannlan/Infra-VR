#pragma once

#include <cstdint>
#include "openvr.h"
#include "vector.h"

class VR;
class Game;
struct ModelRenderInfo_t;

// World-space pose and anatomical basis of a rendered VR hand's grip pocket.
// Coordinates and vectors are in Source world units (1 unit ~= 23.15 mm at VRScale = 43.2).
struct VRHandGripPose
{
	Vector gripWorldPos{ 0.0f, 0.0f, 0.0f };       // Center of the closed-hand palmar grip pocket (~9.5 mm in front of inner palm skin)
	Vector palmSkinWorldPos{ 0.0f, 0.0f, 0.0f };   // Inner palmar VVD skin surface point in world space
	Vector wristWorldPos{ 0.0f, 0.0f, 0.0f };      // World-space origin of Bip001 L/R Hand
	Vector palmOutwardNormal{ 0.0f, 0.0f, 1.0f };  // Unit normal pointing out of the inner palm (+Y of Hand bone)
	Vector fingerDistalDir{ 1.0f, 0.0f, 0.0f };    // Unit vector from wrist toward MCP knuckles (+X of Hand bone)
	Vector thumbRadialDir{ 0.0f, 1.0f, 0.0f };     // Unit vector toward the thumb side (+Z on Right Hand, -Z on Left Hand)
	matrix3x4_t wristToWorld{};                    // Orthonormal bone-to-world transform of Bip001 L/R Hand
	bool valid = false;
	bool isLeftHandMesh = false;                   // True when backed by vm1 (Left Hand mesh), false when backed by vm0 (Right Hand mesh)
	bool usePhoneOpenTable = false;                // True when vm0 or vm1 is actively rendering bare-hand v_phone.mdl
};

namespace VRHands
{
	struct ViewModelLensInfo
	{
		bool updated = false;
		Vector pos{ 0.0f, 0.0f, 0.0f };
		Vector normal{ 1.0f, 0.0f, 0.0f };

		bool tailUpdated = false;
		bool isSuperLight = false;
		Vector tailPos{ 0.0f, 0.0f, 0.0f };
		Vector tailNormal{ 0.0f, 0.0f, -1.0f };
		Vector tailAxisU{ 1.0f, 0.0f, 0.0f };
		Vector tailAxisV{ 0.0f, 1.0f, 0.0f };
		float tailCapRadius = 0.892f;

		bool cameraUpdated = false;
		Vector camFlashPos{ 0.0f, 0.0f, 0.0f };
		Vector camFlashNormal{ 1.0f, 0.0f, 0.0f };
		Vector camLedPos{ 0.0f, 0.0f, 0.0f };
		Vector camLedNormal{ -1.0f, 0.0f, 0.0f };
		Vector camBodyPos{ 0.0f, 0.0f, 0.0f };

		bool flipCull = false;
	};

	inline ViewModelLensInfo g_ViewModelLens;

	void ResetFlashlightGripAnim();

	// Selects the best currently precached viewmodel to represent the bare Right VR Hand on vm0.
	const char* SelectBareHandModel(Game* game, int* outModelIndex = nullptr);

	// Updates 5-finger curl [0=Thumb, 1=Index, 2=Middle, 3=Ring, 4=Pinky] in [0..1]
	// and 4-finger splay in [-1..1] from OpenVR skeletal input or controller state fallback.
	void UpdateFingerInput(
		vr::IVRInput* input,
		vr::VRActionHandle_t skelAction,
		bool triggerPressed,
		bool gripPressed,
		bool hasCtrlState,
		const vr::VRControllerState_t& ctrlState,
		float (&curls)[5],
		float (&splays)[4],
		bool* outHasPartialOrFullSkeletal = nullptr);

	// Thread-safe per-hand upper curl limits [0=Thumb .. 4=Pinky] in [0..1] used to stop fingers
	// cleanly against held physics object surfaces during DrawModelExecute.
	void SetFingerDebugLogEnabled(bool enabled);
	void SetFingerCurlLimits(bool physicalLeftHand, const float maxCurls[5]);
	void ClearFingerCurlLimits(bool physicalLeftHand);
	bool GetFingerCurlLimits(bool physicalLeftHand, float outMaxCurls[5]);

	// Lock-free server physics feedback snapshot for the actively grabbed physics entity.
	// Published from vr_grab so the rendered holding hand follows actual blocked prop motion
	// instead of ghosting through walls or tables.
	struct HeldHandPhysicsFeedback
	{
		int heldEntIndex = -1;
		bool holdingLeftHand = false;
		Vector actualEntPos{ 0.0f, 0.0f, 0.0f };
		QAngle actualEntAng{ 0.0f, 0.0f, 0.0f };
		Vector targetEntPos{ 0.0f, 0.0f, 0.0f };
		QAngle targetEntAng{ 0.0f, 0.0f, 0.0f };
		matrix3x4_t targetWristToWorld{}; // Raw unblocked gp.wristToWorld from ComputeHandGripPose(..., false)
		float objRadius = 0.0f;
		bool isWallBlocked = false;
	};

	void PublishHeldHandPhysicsFeedback(const HeldHandPhysicsFeedback& fb);
	void ClearHeldHandPhysicsFeedback();
	bool GetHeldHandPhysicsFeedback(HeldHandPhysicsFeedback& outFb, unsigned long long maxAgeMs = 250ULL);

	// Sweeps open-hand world/prop hulls and couples held-object physics feedback on the client
	// main thread (call once per VR frame before eye rendering).
	void UpdateVisualHandPhysics(Game* game, VR* vr, const Vector& eyePosition);
	void ResetVisualHandPhysics();

	// Queries the physics/wall-resolved world-space origin and angles for viewmodel 0 or 1
	// (optional helper for dCalcViewModelView to align frustum culling and bone setup).
	bool GetVisualViewModelTransform(
		VR* vr,
		bool isVm0,
		const Vector& eyePosition,
		const Vector& rawVmOrigin,
		const QAngle& rawVmAngles,
		Vector& outVmOrigin,
		QAngle& outVmAngles);

	// Computes the exact world-space wrist transform and palmar grip-pocket pose for either physical controller.
	// When applyVisualPhysics is true, uses the latest wall-clipped / prop-coupled visual wrist pose.
	// Safe to call from both simulation/physics hooks and render hooks (works even when ShowHands is false).
	bool ComputeHandGripPose(
		VR* vr,
		bool physicalLeftHand,
		const Vector& eyePosition,
		VRHandGripPose& outPose,
		bool applyVisualPhysics = false);

	// Evaluates the 5 world-space joint/tip positions and 4 phalanx capsule radii (in Source units)
	// for fingerIdx in [0=Thumb, 1=Index, 2=Middle, 3=Ring, 4=Pinky] at a candidate curl01 in [0..1]
	// and splay01 in [-1..1], matching the exact forward kinematics of the rendered hand rig.
	void EvaluateFingerWorldChain(
		const VRHandGripPose& handPose,
		int fingerIdx,
		float curl01,
		float splay01,
		Vector outJoints[5],
		float outRadii[4]);

	// Applies calibrated wrist placement, anatomical finger kinematics, curl clamping, prop-bone collapsing,
	// and post-kinematic cross-sectional radial fullness to activeBones (pCustomBoneToWorld) for INFRA's
	// four viewmodels (v_camera.mdl, v_phone.mdl, v_light.mdl, v_superlight.mdl).
	// Returns true when info.pModel is a handled VR hand viewmodel and activeBones was updated in place.
	bool ApplyViewModelHandRig(
		VR* vr,
		Game* game,
		void* dmsState,
		const ModelRenderInfo_t& info,
		matrix3x4_t* activeBones,
		const float* maxFingerCurlOverride = nullptr,
		ViewModelLensInfo* outLensInfo = nullptr);

	// Tilts an equipped v_phone.mdl viewmodel + hand 25 deg toward the player around bone 32's wrist pivot
	// and bone 53's horizontal screen-right axis so the top edge of the phone tilts closer to the user.
	void ApplyEquippedPhoneTilt(
		Vector& ioVmOrigin,
		Vector& ioFwd,
		Vector& ioRight,
		Vector& ioUp,
		QAngle* outAngles = nullptr);

	struct PhoneScreenTouchResult
	{
		bool hovering = false;
		bool pressedEdge = false;
		bool releasedEdge = false;
		float u = 0.5f; // [0..1] left -> right (450 px)
		float v = 0.5f; // [0..1] top  -> bottom (750 px)
		bool tappingLeftHand = true;
	};

	// Evaluates the opposite bare hand's index fingertip against the live 3D phone screen quad
	// (bone 53 controlpanel0_ll / controlpanel0_ur) for hover tracking and Schmitt-triggered tap clicks.
	PhoneScreenTouchResult PollPhoneScreenTouch(VR* vr, const Vector& eyePosition);
	void ResetPhoneScreenTouch();

	// Returns the live visual world-space index fingertip (matching wall-clamped wrist + conformed finger pose),
	// plus optional raw (unclamped) index fingertip and visual palm skin position. Lock-free and thread-safe.
	bool GetVisualIndexTip(
		VR* vr,
		bool physicalLeftHand,
		const Vector& eyePosition,
		Vector& outVisualTip,
		Vector* outRawTip = nullptr,
		Vector* outVisualPalm = nullptr);
}
