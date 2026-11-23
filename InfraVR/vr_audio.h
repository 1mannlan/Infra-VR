#pragma once

#include <string>

class Game;
class Vector;
class QAngle;

namespace VRAudio
{
	// Initializes Steam Audio (phonon.dll) and installs Source Engine audio hooks.
	// Safe to call even if SpatialAudioMode != "steam_audio"; hooks stay passive unless active.
	bool Init(Game* game);
	void Shutdown();

	// Updates live configuration from VR\config.txt.
	void SetConfig(const std::string& mode, bool occlusion, float roomSend);

	// Called every frame from Hooks::dRenderView with the live HMD world pose.
	// Updates the audio-thread listener snapshot and computes smoothed wall occlusion traces.
	void UpdateListenerPose(const Vector& origin, const QAngle& angles, bool vrEnabled, Game* game);

	// True only when mode == "steam_audio", Steam Audio initialized, and DSOAL is not loaded.
	bool IsSteamAudioActive();
}
