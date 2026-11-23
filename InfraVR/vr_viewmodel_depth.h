#pragma once

#include <cstdint>

namespace VRViewModelDepth
{
	// Replaces CViewRender::DrawViewModels' hardcoded DepthRange(0.0f, 0.1f)
	// ceiling with 1.0f in client.dll so VR viewmodels share the full [0.0, 1.0]
	// world depth-buffer range instead of overwriting held world props positioned
	// in front of the VR hand palm. Resolves sites via SigScanner, validates the
	// referenced float constants, uses scoped VirtualProtect + FlushInstructionCache,
	// and cleanly restores the original 0.1f operand pointers on disable/shutdown.
	bool SetEnabled(uintptr_t clientBase, bool enabled);
	bool SetEnabled(bool enabled);
	bool IsEnabled();
	bool IsResolved();
	void Shutdown();
}
