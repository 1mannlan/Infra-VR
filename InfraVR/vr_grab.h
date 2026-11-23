#pragma once

#include <cstdint>
#include "vector.h"

class VR;
class Game;
class CUserCmd;

struct VRGrabUpdateScope
{
	void* grabCtrl = nullptr;
	void* serverPlayer = nullptr;
	Vector preTargetPos{ 0.0f, 0.0f, 0.0f };
	QAngle preTargetAng{ 0.0f, 0.0f, 0.0f };
	bool prevOverrideEyeAngles = false;
	bool prevInPickupUpdate = false;
	bool active = false;
};

namespace VRGrab
{
	// Returns true when VR is active and a physics object is currently held by either VR hand.
	bool IsHoldingObject(const VR* vr);

	// Resets grab attachment state, held entity tracking, and per-hand finger curl limits.
	void OnObjectDropped(VR* vr);

	// Samples OpenVR controller linear & angular velocity into a rolling throw-history ring buffer.
	// Call once per VR frame at the end of VR::UpdateTracking().
	void RecordHandTrackingVelocity(VR* vr);

	// Releases the currently carried physics object via CBasePlayer::ForceDropOfCarriedPhysObjects,
	// then immediately applies the peak/weighted VR throw velocity (linear + angular + player velocity)
	// directly to its detached IPhysicsObject(s) so DetachEntity's velocity clear/clamp cannot kill throws.
	void ReleaseCarriedObject(Game* game, VR* vr, void* grabCtrl, void* pServerPlayer);

	// Filters CUserCmd buttons during CreateMove so closing the index finger on Right Trigger (IN_ATTACK)
	// while holding a prop never triggers CPlayerPickupController::Use's throw/punt impulse.
	void FilterCreateMoveButtons(VR* vr, CUserCmd* cmd);

	// Keeps server-side pickup state healthy on every server tick (clears buffered IN_ATTACK while holding,
	// locks CGrabController::m_errorTime < 0 so unhooked CPlayerPickupController::Use never force-drops on
	// >70-degree backward hand tilt, and wakes the attached IPhysicsObject).
	void MaintainServerGrabState(Game* game, VR* vr, void* grabCtrl, void* pServerPlayer);

	// Unified prologue/epilogue for CGrabController::UpdateObject and CGrabController::UpdateObjectVM.
	// Returns false when grip was released and the carried object was cleanly dropped via ForceDropOfCarriedPhysObjects.
	bool BeginPickupUpdate(
		Game* game,
		VR* vr,
		void* grabCtrl,
		void* pServerPlayer,
		VRGrabUpdateScope& outScope,
		float& inOutMaxError);

	void NotifySetTargetPositionCalled();
	bool WasSetTargetPositionCalledInScope();

	void EndPickupUpdate(
		Game* game,
		VR* vr,
		bool updateSucceeded,
		bool bIsTeleport,
		VRGrabUpdateScope& scope);

	// Returns the grip-pocket shoot origin and non-flipping horizontal reference angles used while
	// m_InPickupUpdate is active so UpdateObject / UpdateObjectVM / ComputePlayerMatrix never hit
	// pitch singularities or 180-degree yaw flips when tilting the VR hand >70 degrees backwards.
	Vector GetPickupShootPosition(VR* vr);
	QAngle& GetPickupEyeAngles(VR* vr);

	// Computes the 6-DOF Havok shadow target position and orientation for CGrabController::SetTargetPosition
	// directly in the rendered 3D VR hand's palmar grip pocket (with object-local OBB center compensation
	// so held props sit centered between thumb and fingers at every wrist angle without lever-arm swing).
	bool ComputeTargetTransform(
		Game* game,
		VR* vr,
		void* grabCtrl,
		void* fallbackServerEnt,
		const Vector& engineTargetPos,
		const QAngle& engineTargetAng,
		bool bIsTeleport,
		Vector& outTargetPos,
		QAngle& outTargetAng);

	// Call once per VR frame on the client main thread in Hooks::dRenderView (after m_SetupOrigin is updated).
	// Sweeps the 5 rendered VR hand finger chains [0=Thumb, 1=Index, 2=Middle, 3=Ring, 4=Pinky] against
	// the held client entity's collideable surface (IEngineTrace::ClipRayToCollideable + OBB slab fallback),
	// updates VRHands::SetFingerCurlLimits for the holding hand, and optionally writes the 5 curl limits.
	bool UpdateHeldObjectFingerCollision(Game* game, VR* vr, float outMaxFingerCurl[5] = nullptr);

	// Reads the latest computed finger curl limits [0..1] for the requested physical hand.
	bool GetFingerCurlLimits(bool physicalLeftHand, float outMaxFingerCurl[5]);

	// Computes a right-handed, gimbal-free controller basis (outFwd=+X, outRight=-Y, outUp=+Z, det([Fwd,-Right,Up])=+1)
	// directly from OpenVR controller basis vectors without +/-90-degree Euler pitch-pole flips.
	void GetGimbalFreeControllerBasis(
		VR* vr,
		bool physicalLeftHand,
		const Vector& eyePosition,
		Vector& outPos,
		Vector& outFwd,
		Vector& outRight,
		Vector& outUp,
		QAngle& outAngles);

	// Updates close-contact candidate availability for a physical hand (call once per client selection frame for both hands).
	// Bare-finger curl grab initiation is gated on close-touch proximity (or <=6.5u transfer proximity to a held object).
	void NotifyHandCloseTargetProximity(bool physicalLeftHand, bool hasCloseCandidate);

	// Notifies VRGrab whether INFRA's server-side continuous CUseController (+0x1840, valves/doors) is currently active.
	void NotifyServerUseControllerActive(bool active);

	// Evaluates VR hand grip activation combining digital grip actions (m_ActionCrouch / m_ActionReload)
	// with edge-armed, close-contact-gated Knuckles/Index skeletal finger-curl hysteresis (re-arm <= 0.50,
	// engage >= 0.76 w/ >=2 of MRP >= 0.70 on close target, release <= 0.42, 120 ms auto-expire if unattached).
	bool EvaluateHandGripInput(VR* vr, bool physicalLeftHand, bool currentlyHoldingOnThisHand = false);

	// Updates CUserCmd::buttons IN_USE bit in Hooks::dCreateMove, guaranteeing a clean 0->1 m_afButtonPressed & IN_USE
	// rising edge in CBasePlayer::PlayerUse even if the opposite hand already held IN_USE high while not carrying an object.
	void UpdateCreateMoveUseButton(VR* vr, CUserCmd* cmd);

	// Returns true when the debounced CreateMove grip state considers either hand's +use grip active
	// (prevents un-debounced +use/-use console command chatter on strain-gauge micro-fluctuations).
	bool IsDebouncedUseGripDown(VR* vr);

	// Server-side guard called in Hooks::dFindUseEntity on non-null candidates to prevent open-hand door
	// push <-> Grip PlayerUse cross-toggles and sub-300ms (450ms on doors) double-edge retoggles.
	bool FilterServerPlayerUseTarget(Game* game, void* srvEnt, int entIndex, bool isNewUsePress);

	// Returns calibrated anatomical selection origins and reach direction for +use / prop pickup:
	//   outPalmSkinPos : inner palmar VVD skin point in world space
	//   outPinchPos    : thumb-index pinch aperture in world space
	//   outPointDir    : natural palmar-distal reach direction
	bool GetCalibratedHandSelectPose(
		VR* vr,
		bool physicalLeftHand,
		const Vector& eyePosition,
		Vector& outPalmSkinPos,
		Vector& outPinchPos,
		Vector& outPointDir);

	// Evaluates hold-to-grab continuity including seamless hand-to-hand transfer.
	// Call in BeginPickupUpdate and Hooks::dProcessUsercmds instead of a raw holding-hand grip check:
	// when the holding hand opens while the opposite hand grips near the held object,
	// carry ownership transfers smoothly without dropping or snapping.
	bool EvaluateCarryGripHeld(Game* game, VR* vr, void* pServerPlayer);

	// Server-tick open-hand world/prop/door physics interaction.
	// Call once per server tick in Hooks::dProcessUsercmds while in-game (VR enabled, not main menu).
	// Sweeps each free hand's calibrated VVD palm slab against movable physics props and unlocked doors,
	// wakes sleeping objects cleanly, applies damped point impulses, and never collides against localPlayer.
	void UpdateOpenHandPhysics(Game* game, VR* vr, void* pServerPlayer = nullptr);

	// Releases tracked open-hand physics state (call on level change, menu entry, or VR shutdown).
	void ResetOpenHandPhysics();
}
