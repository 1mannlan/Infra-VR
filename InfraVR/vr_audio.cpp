#include "vr_audio.h"
#include "vr.h"
#include "game.h"
#include "offsets.h"
#include "hooks.h"
#include "sdk.h"
#include "trace.h"
#include "MinHook.h"
#include "phonon.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

namespace
{
	// Verified engine.dll globals & channel_t layout (ImageBase 0x10000000)
	constexpr uintptr_t kRvaChannels          = 0x428F70;
	constexpr uintptr_t kRvaPaintedTime       = 0x428940;
	constexpr uintptr_t kRvaSoundOpSystem     = 0x428D70;
	constexpr uintptr_t kRvaPaintBuffers      = 0x479730;
	constexpr uintptr_t kRvaAudioDevice       = 0x4B9B5C;
	constexpr uintptr_t kRvaHostTimescale     = 0x673B48;
	constexpr uintptr_t kRvaTimescaleParent   = 0x673B64;
	constexpr uintptr_t kRvaTimescaleFloat    = 0x673B74;
	constexpr uintptr_t kRvaDemoPlayer        = 0x40CBF0;
	constexpr uintptr_t kRvaUpdateMouth       = 0x2ECF0;
	constexpr uintptr_t kRvaSndPauseAll       = 0x479890;
	constexpr uintptr_t kRvaSndPauseAllParent = 0x4798AC;
	constexpr uintptr_t kRvaSndPauseAllInt    = 0x4798C0;
	constexpr uintptr_t kRvaSoundServices     = 0x40A170;

	constexpr int       kMaxChannels      = 128;
	constexpr uintptr_t kChannelStride    = 0x14C;
	constexpr int       kMaxMixSamples    = 1020;

	// channel_t field offsets
	constexpr uintptr_t kChGuid           = 0x00;
	constexpr uintptr_t kChSfx            = 0x08;
	constexpr uintptr_t kChMixer          = 0x0C;
	constexpr uintptr_t kChOpStack        = 0x10;
	constexpr uintptr_t kChPitch          = 0xB8;
	constexpr uintptr_t kChOrigin         = 0xE4;
	constexpr uintptr_t kChDistMult       = 0xFC;
	constexpr uintptr_t kChSoundChar      = 0x136;
	constexpr uintptr_t kChFlags0         = 0x148;
	constexpr uintptr_t kChFlags1         = 0x149;

	// Steam Audio settings
	constexpr int   kSampleRate            = 44100;
	constexpr int   kFrameSize             = 256;
	constexpr int   kOutRingSize           = 2048;
	constexpr int   kOutRingMask           = kOutRingSize - 1;
	constexpr float kOccludedGain          = 0.58f;
	constexpr float kOcclusionSmoothTauSec = 0.065f;
	constexpr float kMinOcclusionDist      = 140.0f;
	constexpr float kMaxOcclusionDist      = 3000.0f;

#pragma pack(push, 1)
	struct PortableSamplePair
	{
		int32_t left;
		int32_t right;
	};

	struct PaintBuffer
	{
		uint8_t factive;
		uint8_t fsurround;
		uint8_t fsurround_center;
		uint8_t pad03;
		int32_t iflags;
		PortableSamplePair* pbuf;
		uint8_t rest[0x138 - 0x0C];
	};
	static_assert(sizeof(PaintBuffer) == 0x138, "PaintBuffer size mismatch");

	struct CChannelList
	{
		int32_t count;
		int16_t list[128];
		uint8_t quashed[128];
	};
#pragma pack(pop)

	enum class AudioMode
	{
		SteamAudio,
		Dsoal,
		Native
	};

	// Dynamic phonon.dll C API function pointers
	typedef IPLerror            (IPLCALL* tIplContextCreate)(IPLContextSettings*, IPLContext*);
	typedef void                (IPLCALL* tIplContextRelease)(IPLContext*);
	typedef IPLerror            (IPLCALL* tIplHRTFCreate)(IPLContext, IPLAudioSettings*, IPLHRTFSettings*, IPLHRTF*);
	typedef void                (IPLCALL* tIplHRTFRelease)(IPLHRTF*);
	typedef IPLerror            (IPLCALL* tIplBinauralEffectCreate)(IPLContext, IPLAudioSettings*, IPLBinauralEffectSettings*, IPLBinauralEffect*);
	typedef void                (IPLCALL* tIplBinauralEffectRelease)(IPLBinauralEffect*);
	typedef void                (IPLCALL* tIplBinauralEffectReset)(IPLBinauralEffect);
	typedef IPLAudioEffectState (IPLCALL* tIplBinauralEffectApply)(IPLBinauralEffect, IPLBinauralEffectParams*, IPLAudioBuffer*, IPLAudioBuffer*);

	struct PhononAPI
	{
		HMODULE dll = nullptr;
		tIplContextCreate        contextCreate = nullptr;
		tIplContextRelease       contextRelease = nullptr;
		tIplHRTFCreate           hrtfCreate = nullptr;
		tIplHRTFRelease          hrtfRelease = nullptr;
		tIplBinauralEffectCreate binauralCreate = nullptr;
		tIplBinauralEffectRelease binauralRelease = nullptr;
		tIplBinauralEffectReset  binauralReset = nullptr;
		tIplBinauralEffectApply  binauralApply = nullptr;

		bool IsLoaded() const
		{
			return dll && contextCreate && contextRelease && hrtfCreate && hrtfRelease &&
			       binauralCreate && binauralRelease && binauralReset && binauralApply;
		}
	};

	// Engine function pointer types
	typedef void (__cdecl*    tS_SpatializeChannel)(void* pChannel);
	typedef void (__thiscall* tSpatializeChannelOpStack)(void* thisptr, int nSlot, float* pVolumes, const Vector* pDir, float flMinDistFactor, float flMaxGain);
	typedef void (__thiscall* tSpatializeChannelLegacy)(void* thisptr, int nSlot, int* pVolumes, int masterVol, const Vector* pDir, float gain, float flMinDistFactor, int soundChar);
	typedef void (__cdecl*    tMIX_MixChannelsToPaintbuffer)(CChannelList* pList, int64_t end, int flags, int rate, int rateMax);
	typedef void (__thiscall* tMixMono)(void* thisptr, void* pChannel, const void* pData, int outputOffset, int inputOffset, int rateScaleFix, int outCount, int timecompress);
	typedef bool (__cdecl*    tMIX_ScaleChannelVolume)(PaintBuffer* pb, void* pChannel, int32_t* pVolume, int iChans);
	typedef void (__cdecl*    tS_FreeChannel)(void* pChannel);
	typedef void (__thiscall* tCSosOpStack_Execute)(void* thisptr, int mode, void* pChannel, void* pSoundOpSystem);
	typedef void (__cdecl*    tS_UpdateMouth)(void* pChannel, void* pSource, int count);

	struct ChannelAudioState
	{
		int32_t lastGuid = 0;
		uint32_t lastEpoch = 0u;
		std::atomic<bool> isSpatialized{ false };
		std::atomic<int32_t> spatialGuid{ 0 };
		std::atomic<float> lastDirX{ 1.0f };
		std::atomic<float> lastDirY{ 0.0f };
		std::atomic<float> lastDirZ{ 0.0f };
		std::atomic<float> minDistFactor{ 0.0f };
		std::atomic<float> occlusionGain{ 1.0f };
		float cachedOcclusionTarget = 1.0f; // Main-thread-only cache
		int32_t cachedTargetGuid = 0;       // Main-thread-only cache

		IPLBinauralEffect effect = nullptr;
		float lpfState = 0.0f;
		float inFifo[kFrameSize]{};
		int inCount = 0;
		float outL[kOutRingSize]{};
		float outR[kOutRingSize]{};
		uint32_t outRead = 0;
		uint32_t outWrite = 0;
	};

	struct ListenerPoseSnapshot
	{
		bool valid = false;
		Vector origin{ 0.0f, 0.0f, 0.0f };
		Vector forward{ 1.0f, 0.0f, 0.0f };
		Vector right{ 0.0f, -1.0f, 0.0f };
		Vector up{ 0.0f, 0.0f, 1.0f };
	};

	struct MonoCaptureState
	{
		bool active = false;
		float* monoBuf = nullptr;
		int capacity = 0;
	};

	// Thread-local hook context
	thread_local void* tl_SpatializingChannel = nullptr;
	thread_local int   tl_SpatializingSlot = -1;
	thread_local bool  tl_SpatializeCalled = false;
	thread_local MonoCaptureState tl_Capture{};

	// Global audio subsystem state
	std::mutex g_StateMutex;
	bool g_Initialized = false;
	bool g_HooksInstalled = false;
	std::atomic<bool> g_ShuttingDown{ false };
	std::atomic<int> g_AudioHooksInFlight{ 0 };
	std::atomic<uint32_t> g_AudioModeEpoch{ 1u };
	std::atomic<bool> g_EnforcedSteamAudioCvars{ false };
	Game* g_GamePtr = nullptr;
	uintptr_t g_EngineBase = 0;

	struct ScopedAudioHookGuard
	{
		ScopedAudioHookGuard()  { g_AudioHooksInFlight.fetch_add(1, std::memory_order_acq_rel); }
		~ScopedAudioHookGuard() { g_AudioHooksInFlight.fetch_sub(1, std::memory_order_release); }
	};

	std::atomic<AudioMode> g_ConfiguredMode{ AudioMode::SteamAudio };
	std::atomic<bool> g_OcclusionEnabled{ true };
	std::atomic<float> g_RoomSendScale{ 1.0f };
	std::atomic<bool> g_DsoalDetected{ false };

	PhononAPI g_Phonon{};
	IPLContext g_IplContext = nullptr;
	IPLHRTF g_IplHrtf = nullptr;
	IPLAudioSettings g_IplAudioSettings{ kSampleRate, kFrameSize };

	std::array<ChannelAudioState, kMaxChannels> g_ChannelsState;

	std::mutex g_PoseMutex;
	ListenerPoseSnapshot g_ListenerPose{};

	// Engine originals & helpers
	tS_SpatializeChannel          g_OrigS_SpatializeChannel = nullptr;
	tSpatializeChannelOpStack     g_OrigSpatializeOpStack = nullptr;
	tSpatializeChannelLegacy      g_OrigSpatializeLegacy = nullptr;
	tMIX_MixChannelsToPaintbuffer g_OrigMIX_MixChannelsToPaintbuffer = nullptr;
	tMixMono                      g_OrigMix8Mono = nullptr;
	tMixMono                      g_OrigMix16Mono = nullptr;

	tMIX_ScaleChannelVolume       g_FnScaleChannelVolume = nullptr;
	tS_FreeChannel                g_FnFreeChannel = nullptr;
	tCSosOpStack_Execute          g_FnSosOpStackExecute = nullptr;
	tS_UpdateMouth                g_FnUpdateMouth = nullptr;

	void* g_TargetS_SpatializeChannel = nullptr;
	void* g_TargetSpatializeOpStack = nullptr;
	void* g_TargetSpatializeLegacy = nullptr;
	void* g_TargetMixChannelsToPaintbuffer = nullptr;
	void* g_TargetMix8Mono = nullptr;
	void* g_TargetMix16Mono = nullptr;

	// Diagnostic counters
	std::atomic<uint64_t> g_StatHrtfChunks{ 0 };
	std::atomic<uint64_t> g_StatNativeChunks{ 0 };
	std::atomic<uint64_t> g_StatOcclusionRays{ 0 };
	std::atomic<uint64_t> g_StatOccludedHits{ 0 };
	std::chrono::steady_clock::time_point g_LastLogTime{};
	std::chrono::steady_clock::time_point g_LastOccTime{};
	std::chrono::steady_clock::time_point g_LastOccTraceTime{};
	bool g_HasLastOccTime = false;
	bool g_HasLastOccTraceTime = false;
	std::mutex g_LogMutex;

	void WriteLogLine(const std::string& msg)
	{
		std::lock_guard<std::mutex> lock(g_LogMutex);
		const std::string vrDir = VR::ResolveGameRelativePath("VR");
		std::error_code ec;
		std::filesystem::create_directories(vrDir, ec);
		const std::string logPath = VR::ResolveGameRelativePath("VR\\spatial_audio.log");
		std::ofstream out(logPath, std::ios::app);
		if (out.is_open())
		{
			out << msg << "\n";
		}
	}

	void ResetLogFile()
	{
		std::lock_guard<std::mutex> lock(g_LogMutex);
		const std::string vrDir = VR::ResolveGameRelativePath("VR");
		std::error_code ec;
		std::filesystem::create_directories(vrDir, ec);
		const std::string logPath = VR::ResolveGameRelativePath("VR\\spatial_audio.log");
		std::ofstream out(logPath, std::ios::trunc);
		if (out.is_open())
		{
			out << "[VRAudio] Session log started\n";
		}
	}

	std::string ToLowerTrimmed(std::string s)
	{
		while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
			s.pop_back();
		while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
			s.erase(s.begin());
		for (char& c : s)
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return s;
	}

	AudioMode ParseAudioMode(const std::string& modeStr)
	{
		const std::string low = ToLowerTrimmed(modeStr);
		if (low == "dsoal")
			return AudioMode::Dsoal;
		if (low == "native")
			return AudioMode::Native;
		return AudioMode::SteamAudio;
	}

	const char* AudioModeName(AudioMode mode)
	{
		switch (mode)
		{
		case AudioMode::SteamAudio: return "steam_audio";
		case AudioMode::Dsoal:      return "dsoal";
		case AudioMode::Native:     return "native";
		}
		return "steam_audio";
	}

	uint8_t* GetChannelPtr(int slot)
	{
		return reinterpret_cast<uint8_t*>(g_EngineBase + kRvaChannels + static_cast<uintptr_t>(slot) * kChannelStride);
	}

	int ChannelPtrToSlot(const void* pChannel)
	{
		if (!g_EngineBase || !pChannel)
			return -1;
		const uintptr_t base = g_EngineBase + kRvaChannels;
		const uintptr_t addr = reinterpret_cast<uintptr_t>(pChannel);
		if (addr < base)
			return -1;
		const uintptr_t diff = addr - base;
		if (diff % kChannelStride != 0)
			return -1;
		const int slot = static_cast<int>(diff / kChannelStride);
		return (slot >= 0 && slot < kMaxChannels) ? slot : -1;
	}

	bool IsChannelStereo(const void* pChannel)
	{
		if (!pChannel)
			return false;
		const uintptr_t ch = reinterpret_cast<uintptr_t>(pChannel);
		void* sfx = *reinterpret_cast<void* const*>(ch + kChSfx);
		if (!sfx)
			return false;
		void* pSource = *reinterpret_cast<void* const*>(reinterpret_cast<uintptr_t>(sfx) + 0x08);
		if (!pSource)
			return false;
		typedef bool(__thiscall* tIsStereoWav)(void*);
		void** vt = *reinterpret_cast<void***>(pSource);
		if (!vt || !vt[13])
			return false;
		auto fnIsStereo = reinterpret_cast<tIsStereoWav>(vt[13]);
		return fnIsStereo(pSource);
	}

	float GetHostTimescale()
	{
		if (!g_EngineBase)
			return 1.0f;
		void* pConVarSelf = reinterpret_cast<void*>(g_EngineBase + kRvaHostTimescale);
		void* pParent = *reinterpret_cast<void* const*>(g_EngineBase + kRvaTimescaleParent);
		float scale = 1.0f;
		if (pParent == pConVarSelf)
		{
			scale = *reinterpret_cast<const float*>(g_EngineBase + kRvaTimescaleFloat);
		}
		else if (pParent)
		{
			void** vt = *reinterpret_cast<void***>(pParent);
			if (vt && vt[12])
			{
				auto fnGetFloat = reinterpret_cast<float(__thiscall*)(void*)>(vt[12]);
				scale = fnGetFloat(pParent);
			}
		}
		void* pDemoPlayer = *reinterpret_cast<void* const*>(g_EngineBase + kRvaDemoPlayer);
		if (pDemoPlayer)
		{
			void** vt = *reinterpret_cast<void***>(pDemoPlayer);
			if (vt && vt[0x15C / 4])
			{
				auto fnPlaybackRate = reinterpret_cast<float(__thiscall*)(void*)>(vt[0x15C / 4]);
				scale = fnPlaybackRate(pDemoPlayer);
			}
		}
		if (!std::isfinite(scale) || scale <= 0.0f)
			scale = 1.0f;
		return scale;
	}

	int GetSndPauseAll()
	{
		if (!g_EngineBase)
			return 0;
		void* pConVarSelf = reinterpret_cast<void*>(g_EngineBase + kRvaSndPauseAll);
		void* pParent = *reinterpret_cast<void* const*>(g_EngineBase + kRvaSndPauseAllParent);
		if (pParent == pConVarSelf)
			return *reinterpret_cast<const int*>(g_EngineBase + kRvaSndPauseAllInt);
		if (pParent)
		{
			void** vt = *reinterpret_cast<void***>(pParent);
			if (vt && vt[13])
			{
				auto fnGetInt = reinterpret_cast<int(__thiscall*)(void*)>(vt[13]);
				return fnGetInt(pParent);
			}
		}
		return 0;
	}

	bool IsSoundEnginePaused()
	{
		if (g_EngineBase)
		{
			void* pSvc = *reinterpret_cast<void* const*>(g_EngineBase + kRvaSoundServices);
			if (pSvc)
			{
				void** vt = *reinterpret_cast<void***>(pSvc);
				if (vt && vt[16])
				{
					auto fnIsPaused = reinterpret_cast<bool(__thiscall*)(void*)>(vt[16]);
					return fnIsPaused(pSvc);
				}
			}
		}
		return (g_GamePtr && g_GamePtr->m_EngineClient) ? g_GamePtr->m_EngineClient->IsPaused() : false;
	}

	bool LoadPhononLibrary()
	{
		if (g_Phonon.IsLoaded())
			return true;

		const std::string binPath = VR::ResolveGameRelativePath("bin\\phonon.dll");
		const std::string rootPath = VR::ResolveGameRelativePath("phonon.dll");
		HMODULE hDll = LoadLibraryA(binPath.c_str());
		if (!hDll)
			hDll = LoadLibraryA(rootPath.c_str());
		if (!hDll)
			hDll = LoadLibraryA("phonon.dll");
		if (!hDll)
		{
			char buf[256];
			snprintf(buf, sizeof(buf), "[VRAudio] Failed to load phonon.dll (Win32 err=%lu). Tried: %s",
				GetLastError(), binPath.c_str());
			WriteLogLine(buf);
			return false;
		}

		auto resolveProc = [hDll](const char* decorated, const char* undecorated) -> FARPROC {
			FARPROC proc = GetProcAddress(hDll, decorated);
			return proc ? proc : GetProcAddress(hDll, undecorated);
		};

		g_Phonon.dll = hDll;
		g_Phonon.contextCreate   = reinterpret_cast<tIplContextCreate>(resolveProc("_iplContextCreate@8", "iplContextCreate"));
		g_Phonon.contextRelease  = reinterpret_cast<tIplContextRelease>(resolveProc("_iplContextRelease@4", "iplContextRelease"));
		g_Phonon.hrtfCreate      = reinterpret_cast<tIplHRTFCreate>(resolveProc("_iplHRTFCreate@16", "iplHRTFCreate"));
		g_Phonon.hrtfRelease     = reinterpret_cast<tIplHRTFRelease>(resolveProc("_iplHRTFRelease@4", "iplHRTFRelease"));
		g_Phonon.binauralCreate  = reinterpret_cast<tIplBinauralEffectCreate>(resolveProc("_iplBinauralEffectCreate@16", "iplBinauralEffectCreate"));
		g_Phonon.binauralRelease = reinterpret_cast<tIplBinauralEffectRelease>(resolveProc("_iplBinauralEffectRelease@4", "iplBinauralEffectRelease"));
		g_Phonon.binauralReset   = reinterpret_cast<tIplBinauralEffectReset>(resolveProc("_iplBinauralEffectReset@4", "iplBinauralEffectReset"));
		g_Phonon.binauralApply   = reinterpret_cast<tIplBinauralEffectApply>(resolveProc("_iplBinauralEffectApply@16", "iplBinauralEffectApply"));

		if (!g_Phonon.IsLoaded())
		{
			WriteLogLine("[VRAudio] phonon.dll loaded but missing required Steam Audio exports");
			FreeLibrary(hDll);
			g_Phonon = PhononAPI{};
			return false;
		}

		IPLContextSettings ctxSettings{};
		ctxSettings.version = STEAMAUDIO_VERSION;
		ctxSettings.simdLevel = IPL_SIMDLEVEL_AVX2;
		IPLerror err = g_Phonon.contextCreate(&ctxSettings, &g_IplContext);
		if (err != IPL_STATUS_SUCCESS || !g_IplContext)
		{
			char buf[128];
			snprintf(buf, sizeof(buf), "[VRAudio] iplContextCreate failed: err=%d", static_cast<int>(err));
			WriteLogLine(buf);
			FreeLibrary(hDll);
			g_Phonon = PhononAPI{};
			return false;
		}

		IPLHRTFSettings hrtfSettings{};
		hrtfSettings.type = IPL_HRTFTYPE_DEFAULT;
		hrtfSettings.volume = 1.0f;
		hrtfSettings.normType = IPL_HRTFNORMTYPE_NONE;
		err = g_Phonon.hrtfCreate(g_IplContext, &g_IplAudioSettings, &hrtfSettings, &g_IplHrtf);
		if (err != IPL_STATUS_SUCCESS || !g_IplHrtf)
		{
			char buf[128];
			snprintf(buf, sizeof(buf), "[VRAudio] iplHRTFCreate failed: err=%d", static_cast<int>(err));
			WriteLogLine(buf);
			g_Phonon.contextRelease(&g_IplContext);
			g_IplContext = nullptr;
			FreeLibrary(hDll);
			g_Phonon = PhononAPI{};
			return false;
		}

		char okBuf[256];
		snprintf(okBuf, sizeof(okBuf), "[VRAudio] Steam Audio v%u.%u.%u initialized (rate=%d, frameSize=%d)",
			STEAMAUDIO_VERSION_MAJOR, STEAMAUDIO_VERSION_MINOR, STEAMAUDIO_VERSION_PATCH,
			kSampleRate, kFrameSize);
		WriteLogLine(okBuf);
		return true;
	}

	void ResetChannelFifo(ChannelAudioState& st, int32_t newGuid, uint32_t epoch)
	{
		st.lastGuid = newGuid;
		st.lastEpoch = epoch;
		st.lpfState = 0.0f;
		st.inCount = 0;
		st.outRead = 0;
		// Pre-pad one 256-sample frame of silence so arbitrary mix chunk sizes
		// can always pull sampleCount stereo samples immediately without underruns.
		for (int i = 0; i < kFrameSize; ++i)
		{
			st.outL[i] = 0.0f;
			st.outR[i] = 0.0f;
		}
		st.outWrite = kFrameSize;
		if (st.effect && g_Phonon.binauralReset)
			g_Phonon.binauralReset(st.effect);
	}

	bool ProcessChannelBinaural(ChannelAudioState& st, int32_t guid,
	                            const float* monoIn, int sampleCount,
	                            const IPLVector3& dirLocal, float spatialBlend,
	                            float* outLeft, float* outRight)
	{
		if (!g_IplContext || !g_IplHrtf || !g_Phonon.binauralCreate || !g_Phonon.binauralApply)
			return false;

		const uint32_t curEpoch = g_AudioModeEpoch.load(std::memory_order_acquire);
		if (!st.effect)
		{
			IPLBinauralEffectSettings effSettings{};
			effSettings.hrtf = g_IplHrtf;
			if (g_Phonon.binauralCreate(g_IplContext, &g_IplAudioSettings, &effSettings, &st.effect) != IPL_STATUS_SUCCESS || !st.effect)
				return false;
			ResetChannelFifo(st, guid, curEpoch);
		}
		else if (st.lastGuid != guid || st.lastEpoch != curEpoch)
		{
			ResetChannelFifo(st, guid, curEpoch);
		}

		int consumed = 0;
		while (consumed < sampleCount)
		{
			const int needed = kFrameSize - st.inCount;
			const int avail = sampleCount - consumed;
			const int copyCount = (avail < needed) ? avail : needed;
			std::memcpy(st.inFifo + st.inCount, monoIn + consumed, static_cast<size_t>(copyCount) * sizeof(float));
			st.inCount += copyCount;
			consumed += copyCount;

			if (st.inCount == kFrameSize)
			{
				float* inPtrs[1] = { st.inFifo };
				float blkL[kFrameSize]{};
				float blkR[kFrameSize]{};
				float* outPtrs[2] = { blkL, blkR };

				IPLAudioBuffer inBuf{};
				inBuf.numChannels = 1;
				inBuf.numSamples = kFrameSize;
				inBuf.data = inPtrs;

				IPLAudioBuffer outBuf{};
				outBuf.numChannels = 2;
				outBuf.numSamples = kFrameSize;
				outBuf.data = outPtrs;

				IPLBinauralEffectParams params{};
				params.direction = dirLocal;
				params.interpolation = IPL_HRTFINTERPOLATION_BILINEAR;
				params.spatialBlend = spatialBlend;
				params.hrtf = g_IplHrtf;
				params.peakDelays = nullptr;

				g_Phonon.binauralApply(st.effect, &params, &inBuf, &outBuf);

				for (int i = 0; i < kFrameSize; ++i)
				{
					const uint32_t w = (st.outWrite + static_cast<uint32_t>(i)) & kOutRingMask;
					st.outL[w] = blkL[i];
					st.outR[w] = blkR[i];
				}
				st.outWrite += kFrameSize;
				st.inCount = 0;
			}
		}

		// Ensure enough output samples exist (in case ring fell behind).
		while (static_cast<int>(st.outWrite - st.outRead) < sampleCount)
		{
			const uint32_t w = st.outWrite & kOutRingMask;
			st.outL[w] = 0.0f;
			st.outR[w] = 0.0f;
			++st.outWrite;
		}

		for (int i = 0; i < sampleCount; ++i)
		{
			const uint32_t r = (st.outRead + static_cast<uint32_t>(i)) & kOutRingMask;
			outLeft[i] = st.outL[r];
			outRight[i] = st.outR[r];
		}
		st.outRead += static_cast<uint32_t>(sampleCount);
		return true;
	}

	// -----------------------------------------------------------------------
	// Hook 1: S_SpatializeChannel(channel_t* ch)
	// -----------------------------------------------------------------------
	void __cdecl dS_SpatializeChannel(void* pChannel)
	{
		ScopedAudioHookGuard hookGuard;
		struct ScopeGuard
		{
			void* prevChan;
			int prevSlot;
			bool prevCalled;
			~ScopeGuard()
			{
				tl_SpatializingChannel = prevChan;
				tl_SpatializingSlot = prevSlot;
				tl_SpatializeCalled = prevCalled;
			}
		} guard{ tl_SpatializingChannel, tl_SpatializingSlot, tl_SpatializeCalled };

		const int slot = ChannelPtrToSlot(pChannel);
		tl_SpatializingChannel = pChannel;
		tl_SpatializingSlot = slot;
		tl_SpatializeCalled = false;

		g_OrigS_SpatializeChannel(pChannel);

		if (slot >= 0)
		{
			ChannelAudioState& st = g_ChannelsState[slot];
			if (tl_SpatializeCalled && VRAudio::IsSteamAudioActive())
			{
				const uintptr_t chAddr = reinterpret_cast<uintptr_t>(pChannel);
				st.spatialGuid.store(*reinterpret_cast<const int32_t*>(chAddr + kChGuid), std::memory_order_relaxed);
				st.isSpatialized.store(true, std::memory_order_release);
			}
			else
			{
				st.isSpatialized.store(false, std::memory_order_release);
			}
		}
	}

	// -----------------------------------------------------------------------
	// Hook 2: CAudioDeviceBase::SpatializeChannel (opstack path, vt[11], ret 0x14)
	// -----------------------------------------------------------------------
	void __fastcall dSpatializeChannelOpStack(void* ecx, void* edx, int nSlot, float* pVolumes,
	                                         const Vector* pDir, float flMinDistFactor, float flMaxGain)
	{
		ScopedAudioHookGuard hookGuard;
		if (VRAudio::IsSteamAudioActive() &&
		    tl_SpatializingChannel != nullptr &&
		    tl_SpatializingSlot >= 0 &&
		    pVolumes != nullptr &&
		    flMinDistFactor < 0.99f)
		{
			const uintptr_t chAddr = reinterpret_cast<uintptr_t>(tl_SpatializingChannel);
			const uint8_t soundChar = *reinterpret_cast<const uint8_t*>(chAddr + kChSoundChar);
			const float distMult = *reinterpret_cast<const float*>(chAddr + kChDistMult);
			const Vector chOrg = *reinterpret_cast<const Vector*>(chAddr + kChOrigin);
			const float orgLenSq = chOrg.x * chOrg.x + chOrg.y * chOrg.y + chOrg.z * chOrg.z;
			if (soundChar != '@' && distMult > 0.0f && orgLenSq > 1.0f && !IsChannelStereo(tl_SpatializingChannel))
			{
				tl_SpatializeCalled = true;
				ChannelAudioState& st = g_ChannelsState[tl_SpatializingSlot];
				if (pDir)
				{
					st.lastDirX.store(pDir->x, std::memory_order_relaxed);
					st.lastDirY.store(pDir->y, std::memory_order_relaxed);
					st.lastDirZ.store(pDir->z, std::memory_order_relaxed);
				}
				st.minDistFactor.store(std::clamp(flMinDistFactor, 0.0f, 1.0f), std::memory_order_relaxed);

				// Keep unity L/R so operator stacks preserve exact distance/ducking gain in ch->leftvol/rightvol
				pVolumes[0] = 1.0f;
				pVolumes[1] = 1.0f;
				pVolumes[2] = 0.0f;
				pVolumes[3] = 0.0f;
				pVolumes[4] = 0.0f;
				pVolumes[5] = 0.0f;
				return;
			}
		}

		g_OrigSpatializeOpStack(ecx, nSlot, pVolumes, pDir, flMinDistFactor, flMaxGain);
	}

	// -----------------------------------------------------------------------
	// Hook 3: CAudioDeviceBase::SpatializeChannel (legacy path, vt[12], ret 0x1C)
	// -----------------------------------------------------------------------
	void __fastcall dSpatializeChannelLegacy(void* ecx, void* edx, int nSlot, int* pVolumes, int masterVol,
	                                        const Vector* pDir, float gain, float flMinDistFactor, int soundChar)
	{
		ScopedAudioHookGuard hookGuard;
		if (VRAudio::IsSteamAudioActive() &&
		    tl_SpatializingChannel != nullptr &&
		    tl_SpatializingSlot >= 0 &&
		    pVolumes != nullptr &&
		    soundChar != '@' &&
		    flMinDistFactor < 0.99f)
		{
			const uintptr_t chAddr = reinterpret_cast<uintptr_t>(tl_SpatializingChannel);
			const float distMult = *reinterpret_cast<const float*>(chAddr + kChDistMult);
			const Vector chOrg = *reinterpret_cast<const Vector*>(chAddr + kChOrigin);
			const float orgLenSq = chOrg.x * chOrg.x + chOrg.y * chOrg.y + chOrg.z * chOrg.z;
			if (distMult > 0.0f && orgLenSq > 1.0f && !IsChannelStereo(tl_SpatializingChannel))
			{
				tl_SpatializeCalled = true;
				ChannelAudioState& st = g_ChannelsState[tl_SpatializingSlot];
				if (pDir)
				{
					st.lastDirX.store(pDir->x, std::memory_order_relaxed);
					st.lastDirY.store(pDir->y, std::memory_order_relaxed);
					st.lastDirZ.store(pDir->z, std::memory_order_relaxed);
				}
				st.minDistFactor.store(std::clamp(flMinDistFactor, 0.0f, 1.0f), std::memory_order_relaxed);

				const int vol = std::clamp(static_cast<int>(static_cast<float>(masterVol) * gain), 0, 255);
				pVolumes[0] = vol;
				pVolumes[1] = vol;
				pVolumes[2] = 0;
				pVolumes[3] = 0;
				pVolumes[4] = 0;
				pVolumes[5] = 0;
				return;
			}
		}

		g_OrigSpatializeLegacy(ecx, nSlot, pVolumes, masterVol, pDir, gain, flMinDistFactor, soundChar);
	}

	// -----------------------------------------------------------------------
	// Hooks 4 & 5: Mix8Mono & Mix16Mono (capture mono PCM during Steam Audio pass)
	// -----------------------------------------------------------------------
	template <typename SampleT>
	void CaptureMonoSamples(const SampleT* pData, int outputOffset, int inputOffset, int rateScaleFix, int outCount, float scale)
	{
		if (!pData || !tl_Capture.monoBuf || outCount <= 0 || outputOffset < 0)
			return;

		uint32_t step14 = static_cast<uint32_t>(rateScaleFix) >> 14;
		uint32_t frac14 = static_cast<uint32_t>(inputOffset) >> 14;
		int idx = static_cast<int>(frac14 >> 14);
		frac14 &= 0x3FFFu;

		for (int i = 0; i < outCount; ++i)
		{
			const int dstIdx = outputOffset + i;
			if (dstIdx >= tl_Capture.capacity)
				break;

			int sampleInt = static_cast<int>(pData[idx]);
			if (frac14 != 0)
			{
				const int s1 = static_cast<int>(pData[idx + 1]);
				sampleInt += ((s1 - sampleInt) * static_cast<int>(frac14)) >> 14;
			}
			tl_Capture.monoBuf[dstIdx] += static_cast<float>(sampleInt) * scale;

			frac14 += step14;
			idx += static_cast<int>(frac14 >> 14);
			frac14 &= 0x3FFFu;
		}
	}

	void __fastcall dMix8Mono(void* ecx, void* edx, void* pChannel, const void* pData,
	                          int outputOffset, int inputOffset, int rateScaleFix, int outCount, int timecompress)
	{
		ScopedAudioHookGuard hookGuard;
		if (tl_Capture.active)
		{
			CaptureMonoSamples<int8_t>(
				reinterpret_cast<const int8_t*>(pData), outputOffset, inputOffset, rateScaleFix, outCount, 1.0f / 128.0f);
			return;
		}
		g_OrigMix8Mono(ecx, pChannel, pData, outputOffset, inputOffset, rateScaleFix, outCount, timecompress);
	}

	void __fastcall dMix16Mono(void* ecx, void* edx, void* pChannel, const void* pData,
	                           int outputOffset, int inputOffset, int rateScaleFix, int outCount, int timecompress)
	{
		ScopedAudioHookGuard hookGuard;
		if (tl_Capture.active)
		{
			CaptureMonoSamples<int16_t>(
				reinterpret_cast<const int16_t*>(pData), outputOffset, inputOffset, rateScaleFix, outCount, 1.0f / 32768.0f);
			return;
		}
		g_OrigMix16Mono(ecx, pChannel, pData, outputOffset, inputOffset, rateScaleFix, outCount, timecompress);
	}

	// -----------------------------------------------------------------------
	// Hook 6: MIX_MixChannelsToPaintbuffer
	// -----------------------------------------------------------------------
	void __cdecl dMIX_MixChannelsToPaintbuffer(CChannelList* pList, int64_t end, int flags, int rate, int rateMax)
	{
		ScopedAudioHookGuard hookGuard;
		if (!VRAudio::IsSteamAudioActive() || !pList || flags != 0 || rateMax <= 0 ||
		    (rate != 11025 && rate != 22050 && rate != 44100))
		{
			if (pList && rate == 44100 && flags == 0)
				g_StatNativeChunks.fetch_add(static_cast<uint64_t>(pList->count), std::memory_order_relaxed);
			g_OrigMIX_MixChannelsToPaintbuffer(pList, end, flags, rate, rateMax);
			return;
		}

		const int64_t paintedTime = *reinterpret_cast<const int64_t*>(g_EngineBase + kRvaPaintedTime);
		const int sampleCount = static_cast<int>(end - paintedTime);
		if (sampleCount <= 0)
		{
			g_OrigMIX_MixChannelsToPaintbuffer(pList, end, flags, rate, rateMax);
			return;
		}

		if (rate == 44100)
		{
			PaintBuffer* pbs = *reinterpret_cast<PaintBuffer* const*>(g_EngineBase + kRvaPaintBuffers);
			void* dev = *reinterpret_cast<void* const*>(g_EngineBase + kRvaAudioDevice);
			if (rateMax != 44100 || !pbs || pbs[2].factive == 0 || !pbs[2].pbuf || !dev)
			{
				g_StatNativeChunks.fetch_add(static_cast<uint64_t>(pList->count), std::memory_order_relaxed);
				g_OrigMIX_MixChannelsToPaintbuffer(pList, end, flags, rate, rateMax);
				return;
			}
		}

		struct SavedEntry
		{
			int16_t chanIdx;
			uint8_t quashed;
		};
		SavedEntry saved[kMaxChannels]{};
		int savedCount = 0;

		const int origCount = std::clamp(static_cast<int>(pList->count), 0, kMaxChannels);
		int keptCount = 0;
		for (int i = 0; i < origCount; ++i)
		{
			const int16_t chanIdx = pList->list[i];
			const uint8_t quashed = pList->quashed[i];
			bool isHrtf = false;
			if (chanIdx >= 0 && chanIdx < kMaxChannels)
			{
				const uint8_t* chPtr = GetChannelPtr(chanIdx);
				const int32_t guid = *reinterpret_cast<const int32_t*>(chPtr + kChGuid);
				void* sfx = *reinterpret_cast<void* const*>(chPtr + kChSfx);
				void* mixer = *reinterpret_cast<void* const*>(chPtr + kChMixer);
				const uint8_t flags0 = *reinterpret_cast<const uint8_t*>(chPtr + kChFlags0);
				const ChannelAudioState& st = g_ChannelsState[chanIdx];
				if (sfx && mixer && !(flags0 & 0x0C) &&
				    st.isSpatialized.load(std::memory_order_acquire) &&
				    st.spatialGuid.load(std::memory_order_relaxed) == guid &&
				    !IsChannelStereo(chPtr))
				{
					isHrtf = true;
				}
			}

			if (isHrtf)
			{
				saved[savedCount++] = { chanIdx, quashed };
			}
			else
			{
				pList->list[keptCount] = chanIdx;
				pList->quashed[keptCount] = quashed;
				++keptCount;
			}
		}
		pList->count = keptCount;

		if (rate == 44100 && keptCount > 0)
			g_StatNativeChunks.fetch_add(static_cast<uint64_t>(keptCount), std::memory_order_relaxed);

		// Mix all 2D/stereo/music/UI/untagged sounds through vanilla Source Engine
		g_OrigMIX_MixChannelsToPaintbuffer(pList, end, flags, rate, rateMax);

		if (savedCount == 0)
			return;

		// Preserve HRTF channels across 11025 and 22050 passes until the 44100 pass runs
		if (rate != 44100 || rateMax != 44100)
		{
			for (int i = 0; i < savedCount && pList->count < kMaxChannels; ++i)
			{
				const int dst = pList->count++;
				pList->list[dst] = saved[i].chanIdx;
				pList->quashed[dst] = saved[i].quashed;
			}
			return;
		}

		const int clampedSamples = std::min(sampleCount, kMaxMixSamples);
		PaintBuffer* paintBuffers = *reinterpret_cast<PaintBuffer* const*>(g_EngineBase + kRvaPaintBuffers);
		void* audioDevice = *reinterpret_cast<void* const*>(g_EngineBase + kRvaAudioDevice);
		void* soundOpSystem = reinterpret_cast<void*>(g_EngineBase + kRvaSoundOpSystem);

		if (!paintBuffers || paintBuffers[2].factive == 0 || !paintBuffers[2].pbuf || !audioDevice)
		{
			// Fallback: re-append and let vanilla mix them
			for (int i = 0; i < savedCount && pList->count < kMaxChannels; ++i)
			{
				const int dst = pList->count++;
				pList->list[dst] = saved[i].chanIdx;
				pList->quashed[dst] = saved[i].quashed;
			}
			return;
		}

		ListenerPoseSnapshot pose;
		{
			std::lock_guard<std::mutex> lock(g_PoseMutex);
			pose = g_ListenerPose;
		}

		const float timescale = GetHostTimescale();
		const int sndPauseAll = GetSndPauseAll();
		const bool gamePaused = IsSoundEnginePaused();
		const float roomSendScale = std::clamp(g_RoomSendScale.load(std::memory_order_relaxed), 0.0f, 1.5f);
		const bool roomActive = (paintBuffers[1].factive != 0 && paintBuffers[1].pbuf != nullptr);
		const bool awayActive = (paintBuffers[3].factive != 0);

		typedef int  (__thiscall* tMixDataToDevice)(void* thisptr, void* pDevice, void* pChannel, int sampleCount, int outputRate, int outputOffset);
		typedef int  (__thiscall* tSkipSamples)(void* thisptr, void* pChannel, int sampleCount, int outputRate, int outputOffset);
		typedef bool (__thiscall* tShouldContinueMixing)(void* thisptr);

		float monoBuf[kMaxMixSamples + 4]{};
		float directMonoBuf[kMaxMixSamples + 4]{};
		float binL[kMaxMixSamples + 4]{};
		float binR[kMaxMixSamples + 4]{};

		for (int i = 0; i < savedCount; ++i)
		{
			const int16_t chanIdx = saved[i].chanIdx;
			const uint8_t quashed = saved[i].quashed;
			uint8_t* chPtr = GetChannelPtr(chanIdx);

			void* sfx = *reinterpret_cast<void* const*>(chPtr + kChSfx);
			const uint8_t flags1 = *reinterpret_cast<const uint8_t*>(chPtr + kChFlags1);
			bool pauseable = ((flags1 & 0x40u) != 0);
			if (sndPauseAll != 0 && sfx)
			{
				const uint8_t sfxFlags = *reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(sfx) + 0x0C);
				pauseable = ((sfxFlags & 0x02u) == 0);
			}
			if (pauseable && gamePaused)
			{
				if (pList->count < kMaxChannels)
				{
					const int dst = pList->count++;
					pList->list[dst] = chanIdx;
					pList->quashed[dst] = quashed;
				}
				continue;
			}

			void* mixer = *reinterpret_cast<void* const*>(chPtr + kChMixer);
			if (!mixer)
				continue;

			void** mixerVt = *reinterpret_cast<void***>(mixer);
			auto fnMixDataToDevice = reinterpret_cast<tMixDataToDevice>(mixerVt[1]);
			auto fnSkipSamples = reinterpret_cast<tSkipSamples>(mixerVt[2]);
			auto fnShouldContinue = reinterpret_cast<tShouldContinueMixing>(mixerVt[3]);

			float* pPitch = reinterpret_cast<float*>(chPtr + kChPitch);
			const float origPitch = *pPitch;

			if (quashed != 0)
			{
				*pPitch = origPitch * timescale;
				if (fnSkipSamples)
					fnSkipSamples(mixer, chPtr, clampedSamples, kSampleRate, 0);
			}
			else if (fnMixDataToDevice && g_FnScaleChannelVolume)
			{
				void* pSource = sfx ? *reinterpret_cast<void* const*>(reinterpret_cast<const uint8_t*>(sfx) + 0x08) : nullptr;
				if (g_FnUpdateMouth && pSource && (flags1 & 0x40u) != 0)
				{
					g_FnUpdateMouth(chPtr, pSource, clampedSamples);
				}

				*pPitch = origPitch * timescale;
				std::memset(monoBuf, 0, static_cast<size_t>(clampedSamples) * sizeof(float));

				// Snapshot all 6 factive flags and leave ONLY IFACINGBUFFER active so
				// MixDataToDevice invokes Mix8Mono/Mix16Mono exactly once per chunk.
				uint8_t savedActive[6]{};
				for (int pb = 0; pb < 6; ++pb)
				{
					savedActive[pb] = paintBuffers[pb].factive;
					paintBuffers[pb].factive = (pb == 2) ? 1 : 0;
				}

				tl_Capture.monoBuf = monoBuf;
				tl_Capture.capacity = clampedSamples;
				tl_Capture.active = true;
				fnMixDataToDevice(mixer, audioDevice, chPtr, clampedSamples, kSampleRate, 0);
				tl_Capture.active = false;
				tl_Capture.monoBuf = nullptr;
				tl_Capture.capacity = 0;

				for (int pb = 0; pb < 6; ++pb)
					paintBuffers[pb].factive = savedActive[pb];

				// Compute exact Source gains for dry (facing + facingaway) and room reverb bus
				int32_t dryVol[12]{};
				int32_t totalDryInt = 0;
				if (g_FnScaleChannelVolume(&paintBuffers[2], chPtr, dryVol, 1))
					totalDryInt += dryVol[0];
				if (awayActive)
				{
					int32_t awayVol[12]{};
					if (g_FnScaleChannelVolume(&paintBuffers[3], chPtr, awayVol, 1))
						totalDryInt += awayVol[0];
				}
				const float dryGain = static_cast<float>(totalDryInt) / 256.0f;

				float roomGain = 0.0f;
				if (roomActive)
				{
					int32_t roomVol[12]{};
					if (g_FnScaleChannelVolume(&paintBuffers[1], chPtr, roomVol, 1))
						roomGain = (static_cast<float>(roomVol[0]) / 256.0f) * roomSendScale;
				}

				ChannelAudioState& st = g_ChannelsState[chanIdx];
				const float occ = g_OcclusionEnabled.load(std::memory_order_relaxed)
					? st.occlusionGain.load(std::memory_order_relaxed)
					: 1.0f;
				const float directGain = dryGain * occ;

				if (directGain > 1e-5f || roomGain > 1e-5f)
				{
					Vector dirWorld(
						st.lastDirX.load(std::memory_order_relaxed),
						st.lastDirY.load(std::memory_order_relaxed),
						st.lastDirZ.load(std::memory_order_relaxed));
					if (pose.valid)
					{
						const Vector srcOrigin = *reinterpret_cast<const Vector*>(chPtr + kChOrigin);
						const float srcOrigLenSq = srcOrigin.x * srcOrigin.x + srcOrigin.y * srcOrigin.y + srcOrigin.z * srcOrigin.z;
						if (srcOrigLenSq > 1.0f)
						{
							const Vector toSrc = srcOrigin - pose.origin;
							const float lenSq = toSrc.x * toSrc.x + toSrc.y * toSrc.y + toSrc.z * toSrc.z;
							if (lenSq > 1e-4f)
							{
								const float invLen = 1.0f / std::sqrt(lenSq);
								dirWorld = Vector(toSrc.x * invLen, toSrc.y * invLen, toSrc.z * invLen);
							}
						}
					}

					IPLVector3 dirLocal{ 0.0f, 0.0f, -1.0f };
					if (pose.valid)
					{
						dirLocal.x = DotProduct(dirWorld, pose.right);
						dirLocal.y = DotProduct(dirWorld, pose.up);
						dirLocal.z = -DotProduct(dirWorld, pose.forward);
						const float dLenSq = dirLocal.x * dirLocal.x + dirLocal.y * dirLocal.y + dirLocal.z * dirLocal.z;
						if (dLenSq > 1e-6f)
						{
							const float invD = 1.0f / std::sqrt(dLenSq);
							dirLocal.x *= invD;
							dirLocal.y *= invD;
							dirLocal.z *= invD;
						}
						else
						{
							dirLocal = { 0.0f, 0.0f, -1.0f };
						}
					}

					const int32_t guid = *reinterpret_cast<const int32_t*>(chPtr + kChGuid);
					const float minDist = st.minDistFactor.load(std::memory_order_relaxed);
					const float spatialBlend = std::clamp(1.0f - minDist, 0.0f, 1.0f);

					const float occFrac = std::clamp((1.0f - occ) / (1.0f - kOccludedGain), 0.0f, 1.0f);
					const float* directInput = monoBuf;
					if (occFrac > 0.01f)
					{
						const float cutoffHz = 16000.0f * std::pow(2000.0f / 16000.0f, occFrac);
						const float lpfAlpha = 1.0f - std::exp(-2.0f * 3.14159265f * cutoffHz / static_cast<float>(kSampleRate));
						for (int s = 0; s < clampedSamples; ++s)
						{
							st.lpfState += lpfAlpha * (monoBuf[s] - st.lpfState);
							directMonoBuf[s] = st.lpfState;
						}
						directInput = directMonoBuf;
					}
					else if (clampedSamples > 0)
					{
						st.lpfState = monoBuf[clampedSamples - 1];
					}

					if (ProcessChannelBinaural(st, guid, directInput, clampedSamples, dirLocal, spatialBlend, binL, binR))
					{
						g_StatHrtfChunks.fetch_add(1, std::memory_order_relaxed);
						if (directGain > 1e-5f)
						{
							PortableSamplePair* pbFacing = paintBuffers[2].pbuf;
							const float scale = directGain * 32767.0f;
							for (int s = 0; s < clampedSamples; ++s)
							{
								pbFacing[s].left  += static_cast<int32_t>(binL[s] * scale);
								pbFacing[s].right += static_cast<int32_t>(binR[s] * scale);
							}
						}
					}
					else if (directGain > 1e-5f)
					{
						PortableSamplePair* pbFacing = paintBuffers[2].pbuf;
						const float scale = directGain * 32767.0f;
						for (int s = 0; s < clampedSamples; ++s)
						{
							const int32_t m = static_cast<int32_t>(directInput[s] * scale);
							pbFacing[s].left  += m;
							pbFacing[s].right += m;
						}
					}

					if (roomActive && roomGain > 1e-5f)
					{
						PortableSamplePair* pbRoom = paintBuffers[1].pbuf;
						const float scale = roomGain * 32767.0f;
						for (int s = 0; s < clampedSamples; ++s)
						{
							const int32_t r = static_cast<int32_t>(monoBuf[s] * scale);
							pbRoom[s].left  += r;
							pbRoom[s].right += r;
						}
					}
				}
			}

			*pPitch = origPitch;

			const bool cont = fnShouldContinue ? fnShouldContinue(mixer) : false;
			if (cont)
			{
				if (pList->count < kMaxChannels)
				{
					const int dst = pList->count++;
					pList->list[dst] = chanIdx;
					pList->quashed[dst] = quashed;
				}
			}
			else
			{
				void* pOpStack = *reinterpret_cast<void* const*>(chPtr + kChOpStack);
				if (pOpStack && g_FnSosOpStackExecute)
					g_FnSosOpStackExecute(pOpStack, 4, chPtr, soundOpSystem);
				if (g_FnFreeChannel)
					g_FnFreeChannel(chPtr);
			}
		}
	}

	bool CreateAndEnableHook(void* target, void* detour, void** origOut, const char* name)
	{
		if (!target)
		{
			char buf[160];
			snprintf(buf, sizeof(buf), "[VRAudio] Null target for hook %s", name);
			WriteLogLine(buf);
			return false;
		}
		MH_STATUS st = MH_CreateHook(target, detour, origOut);
		if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED)
		{
			char buf[160];
			snprintf(buf, sizeof(buf), "[VRAudio] MH_CreateHook failed for %s (status=%d)", name, static_cast<int>(st));
			WriteLogLine(buf);
			return false;
		}
		st = MH_EnableHook(target);
		if (st != MH_OK && st != MH_ERROR_ENABLED)
		{
			char buf[160];
			snprintf(buf, sizeof(buf), "[VRAudio] MH_EnableHook failed for %s (status=%d)", name, static_cast<int>(st));
			WriteLogLine(buf);
			return false;
		}
		return true;
	}
}

namespace VRAudio
{
	bool Init(Game* game)
	{
		std::lock_guard<std::mutex> lock(g_StateMutex);
		if (g_Initialized)
			return true;

		ResetLogFile();
		g_ShuttingDown.store(false, std::memory_order_release);
		g_HasLastOccTime = false;
		g_HasLastOccTraceTime = false;
		g_EnforcedSteamAudioCvars.store(false, std::memory_order_relaxed);
		g_GamePtr = game;
		g_EngineBase = game ? game->m_BaseEngine : reinterpret_cast<uintptr_t>(GetModuleHandleA("engine.dll"));
		g_DsoalDetected.store(GetModuleHandleA("dsoal-aldrv.dll") != nullptr, std::memory_order_relaxed);
		g_LastLogTime = std::chrono::steady_clock::now();

		if (!game || !game->m_Offsets || !g_EngineBase)
		{
			WriteLogLine("[VRAudio] Missing Game/Offsets/engine.dll base; Steam Audio hooks skipped");
			return false;
		}

		// Sync initial config if VR already parsed config.txt
		if (game->m_VR)
		{
			g_ConfiguredMode.store(ParseAudioMode(game->m_VR->m_SpatialAudioMode), std::memory_order_relaxed);
			g_OcclusionEnabled.store(game->m_VR->m_SpatialAudioOcclusion, std::memory_order_relaxed);
			g_RoomSendScale.store(std::clamp(game->m_VR->m_SpatialAudioRoomSend, 0.0f, 1.5f), std::memory_order_relaxed);
		}

		bool phononOk = LoadPhononLibrary();

		g_FnScaleChannelVolume = reinterpret_cast<tMIX_ScaleChannelVolume>(game->m_Offsets->MIX_ScaleChannelVolume.address);
		g_FnFreeChannel        = reinterpret_cast<tS_FreeChannel>(game->m_Offsets->S_FreeChannel.address);
		g_FnSosOpStackExecute  = reinterpret_cast<tCSosOpStack_Execute>(game->m_Offsets->CSosOpStack_Execute.address);
		g_FnUpdateMouth        = nullptr;
		if (g_EngineBase)
		{
			const uint8_t* pMouth = reinterpret_cast<const uint8_t*>(g_EngineBase + kRvaUpdateMouth);
			if (pMouth[0] == 0x55 && pMouth[1] == 0x8B && pMouth[2] == 0xEC &&
			    pMouth[3] == 0x53 && pMouth[4] == 0x8B && pMouth[5] == 0x5D && pMouth[6] == 0x08)
			{
				g_FnUpdateMouth = reinterpret_cast<tS_UpdateMouth>(const_cast<uint8_t*>(pMouth));
			}
		}

		g_TargetS_SpatializeChannel     = reinterpret_cast<void*>(game->m_Offsets->S_SpatializeChannel.address);
		g_TargetSpatializeOpStack       = reinterpret_cast<void*>(game->m_Offsets->AudioDevice_SpatializeChannelOpStack.address);
		g_TargetSpatializeLegacy        = reinterpret_cast<void*>(game->m_Offsets->AudioDevice_SpatializeChannelLegacy.address);
		g_TargetMixChannelsToPaintbuffer = reinterpret_cast<void*>(game->m_Offsets->MIX_MixChannelsToPaintbuffer.address);
		g_TargetMix8Mono                = reinterpret_cast<void*>(game->m_Offsets->AudioDevice_Mix8Mono.address);
		g_TargetMix16Mono               = reinterpret_cast<void*>(game->m_Offsets->AudioDevice_Mix16Mono.address);

		bool hooksOk = true;
		hooksOk &= CreateAndEnableHook(g_TargetS_SpatializeChannel, reinterpret_cast<void*>(&dS_SpatializeChannel),
			reinterpret_cast<void**>(&g_OrigS_SpatializeChannel), "S_SpatializeChannel");
		hooksOk &= CreateAndEnableHook(g_TargetSpatializeOpStack, reinterpret_cast<void*>(&dSpatializeChannelOpStack),
			reinterpret_cast<void**>(&g_OrigSpatializeOpStack), "SpatializeChannelOpStack");
		hooksOk &= CreateAndEnableHook(g_TargetSpatializeLegacy, reinterpret_cast<void*>(&dSpatializeChannelLegacy),
			reinterpret_cast<void**>(&g_OrigSpatializeLegacy), "SpatializeChannelLegacy");
		hooksOk &= CreateAndEnableHook(g_TargetMixChannelsToPaintbuffer, reinterpret_cast<void*>(&dMIX_MixChannelsToPaintbuffer),
			reinterpret_cast<void**>(&g_OrigMIX_MixChannelsToPaintbuffer), "MIX_MixChannelsToPaintbuffer");
		hooksOk &= CreateAndEnableHook(g_TargetMix8Mono, reinterpret_cast<void*>(&dMix8Mono),
			reinterpret_cast<void**>(&g_OrigMix8Mono), "Mix8Mono");
		hooksOk &= CreateAndEnableHook(g_TargetMix16Mono, reinterpret_cast<void*>(&dMix16Mono),
			reinterpret_cast<void**>(&g_OrigMix16Mono), "Mix16Mono");

		g_HooksInstalled = hooksOk;
		g_Initialized = true;

		char summary[512];
		snprintf(summary, sizeof(summary),
			"[VRAudio] Init complete: mode=%s dsoal=%d phonon=%d hooks=%d active=%d "
			"(S_Spat=%p OpSpat=%p LegSpat=%p MixPB=%p Mix8=%p Mix16=%p)",
			AudioModeName(g_ConfiguredMode.load(std::memory_order_relaxed)),
			g_DsoalDetected.load(std::memory_order_relaxed) ? 1 : 0,
			phononOk ? 1 : 0,
			hooksOk ? 1 : 0,
			IsSteamAudioActive() ? 1 : 0,
			g_TargetS_SpatializeChannel, g_TargetSpatializeOpStack, g_TargetSpatializeLegacy,
			g_TargetMixChannelsToPaintbuffer, g_TargetMix8Mono, g_TargetMix16Mono);
		WriteLogLine(summary);
		return phononOk && hooksOk;
	}

	void Shutdown()
	{
		std::lock_guard<std::mutex> lock(g_StateMutex);
		if (!g_Initialized)
			return;

		// Stop active interception and drain async audio thread detours before freeing Steam Audio
		g_ShuttingDown.store(true, std::memory_order_release);
		g_ConfiguredMode.store(AudioMode::Native, std::memory_order_release);
		g_HasLastOccTime = false;
		g_HasLastOccTraceTime = false;

		void* targets[] = {
			g_TargetS_SpatializeChannel,
			g_TargetSpatializeOpStack,
			g_TargetSpatializeLegacy,
			g_TargetMixChannelsToPaintbuffer,
			g_TargetMix8Mono,
			g_TargetMix16Mono
		};
		for (void* t : targets)
		{
			if (t)
				MH_DisableHook(t);
		}

		for (int wait = 0; wait < 250 && g_AudioHooksInFlight.load(std::memory_order_acquire) > 0; ++wait)
		{
			Sleep(1);
		}

		for (ChannelAudioState& st : g_ChannelsState)
		{
			if (st.effect && g_Phonon.binauralRelease)
				g_Phonon.binauralRelease(&st.effect);
			st.effect = nullptr;
			st.isSpatialized.store(false, std::memory_order_relaxed);
			st.occlusionGain.store(1.0f, std::memory_order_relaxed);
			st.cachedOcclusionTarget = 1.0f;
			st.cachedTargetGuid = 0;
		}

		if (g_IplHrtf && g_Phonon.hrtfRelease)
		{
			g_Phonon.hrtfRelease(&g_IplHrtf);
			g_IplHrtf = nullptr;
		}
		if (g_IplContext && g_Phonon.contextRelease)
		{
			g_Phonon.contextRelease(&g_IplContext);
			g_IplContext = nullptr;
		}
		if (g_Phonon.dll)
		{
			FreeLibrary(g_Phonon.dll);
			g_Phonon = PhononAPI{};
		}

		g_HooksInstalled = false;
		g_Initialized = false;
		g_FnUpdateMouth = nullptr;
		WriteLogLine("[VRAudio] Shutdown complete");
	}

	void SetConfig(const std::string& mode, bool occlusion, float roomSend)
	{
		const AudioMode parsed = ParseAudioMode(mode);
		const AudioMode prev = g_ConfiguredMode.exchange(parsed, std::memory_order_release);
		g_OcclusionEnabled.store(occlusion, std::memory_order_relaxed);
		g_RoomSendScale.store(std::clamp(roomSend, 0.0f, 1.5f), std::memory_order_relaxed);

		if (prev != parsed)
		{
			g_AudioModeEpoch.fetch_add(1u, std::memory_order_acq_rel);
			g_EnforcedSteamAudioCvars.store(false, std::memory_order_relaxed);
		}

		if (g_Initialized && prev != parsed)
		{
			char buf[256];
			snprintf(buf, sizeof(buf), "[VRAudio] Live config update: mode=%s occlusion=%d roomSend=%.2f active=%d",
				AudioModeName(parsed), occlusion ? 1 : 0, roomSend, IsSteamAudioActive() ? 1 : 0);
			WriteLogLine(buf);
		}
	}

	bool IsSteamAudioActive()
	{
		return g_Initialized &&
		       g_HooksInstalled &&
		       !g_ShuttingDown.load(std::memory_order_acquire) &&
		       g_Phonon.IsLoaded() &&
		       g_IplHrtf != nullptr &&
		       !g_DsoalDetected.load(std::memory_order_relaxed) &&
		       (g_ConfiguredMode.load(std::memory_order_acquire) == AudioMode::SteamAudio);
	}

	void UpdateListenerPose(const Vector& origin, const QAngle& angles, bool vrEnabled, Game* game)
	{
		// Periodically refresh DSOAL check in case dsound.dll loaded delayed
		if (!g_DsoalDetected.load(std::memory_order_relaxed) && GetModuleHandleA("dsoal-aldrv.dll") != nullptr)
		{
			g_DsoalDetected.store(true, std::memory_order_relaxed);
			WriteLogLine("[VRAudio] Detected dsoal-aldrv.dll loaded - Steam Audio auto-disabled to prevent double-HRTF");
		}

		Vector fwd, right, up;
		QAngle::AngleVectors(angles, &fwd, &right, &up);
		{
			std::lock_guard<std::mutex> lock(g_PoseMutex);
			g_ListenerPose.valid = true;
			g_ListenerPose.origin = origin;
			g_ListenerPose.forward = fwd;
			g_ListenerPose.right = right;
			g_ListenerPose.up = up;
		}

		if (!vrEnabled || !IsSteamAudioActive() || !g_EngineBase)
		{
			g_HasLastOccTime = false;
			g_HasLastOccTraceTime = false;
			return;
		}

		if (game && game->m_EngineClient &&
		    !g_EnforcedSteamAudioCvars.exchange(true, std::memory_order_acq_rel))
		{
			game->m_EngineClient->ClientCmd_Unrestricted("dsp_enhance_stereo 0");
		}

		// Occlusion raycasts on main thread only (safe with IEngineTrace), throttled to 20 Hz
		if (g_OcclusionEnabled.load(std::memory_order_relaxed) &&
		    game && game->m_EngineTrace && game->m_EngineClient && game->m_EngineClient->IsInGame())
		{
			const auto occNow = std::chrono::steady_clock::now();
			float occDt = 1.0f / 90.0f;
			if (g_HasLastOccTime)
			{
				occDt = std::clamp(std::chrono::duration<float>(occNow - g_LastOccTime).count(), 0.001f, 0.1f);
			}
			g_LastOccTime = occNow;
			g_HasLastOccTime = true;
			const float occAlpha = 1.0f - std::exp(-occDt / kOcclusionSmoothTauSec);

			const bool doTraceNow = !g_HasLastOccTraceTime ||
			                        (std::chrono::duration<float>(occNow - g_LastOccTraceTime).count() >= 0.05f);
			if (doTraceNow)
			{
				g_LastOccTraceTime = occNow;
				g_HasLastOccTraceTime = true;
			}

			class CTraceFilterWorldOnly : public CTraceFilter
			{
			public:
				CTraceFilterWorldOnly() : CTraceFilter(nullptr, 0) {}
				virtual bool ShouldHitEntity(IHandleEntity*, int) override { return false; }
				virtual TraceType GetTraceType() const override { return TraceType::TRACE_WORLD_ONLY; }
			};
			CTraceFilterWorldOnly worldFilter;

			for (int slot = 0; slot < kMaxChannels; ++slot)
			{
				ChannelAudioState& st = g_ChannelsState[slot];
				if (!st.isSpatialized.load(std::memory_order_acquire))
				{
					st.cachedOcclusionTarget = 1.0f;
					st.cachedTargetGuid = 0;
					st.occlusionGain.store(1.0f, std::memory_order_relaxed);
					continue;
				}

				const uint8_t* chPtr = GetChannelPtr(slot);
				const int32_t guid = *reinterpret_cast<const int32_t*>(chPtr + kChGuid);
				void* sfx = *reinterpret_cast<void* const*>(chPtr + kChSfx);
				if (!sfx || guid != st.spatialGuid.load(std::memory_order_relaxed))
					continue;

				float targetGain = st.cachedOcclusionTarget;
				if (doTraceNow || st.cachedTargetGuid != guid)
				{
					const Vector srcOrigin = *reinterpret_cast<const Vector*>(chPtr + kChOrigin);
					const float srcOrigLenSq = srcOrigin.x * srcOrigin.x + srcOrigin.y * srcOrigin.y + srcOrigin.z * srcOrigin.z;
					const Vector delta = srcOrigin - origin;
					const float dist = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);

					targetGain = 1.0f;
					if (srcOrigLenSq > 1.0f && dist >= kMinOcclusionDist && dist <= kMaxOcclusionDist)
					{
						CGameTrace trFeet;
						Ray_t rayFeet;
						rayFeet.Init(origin, srcOrigin);
						game->m_EngineTrace->TraceRay(rayFeet, CONTENTS_SOLID, &worldFilter, &trFeet);

						CGameTrace trHead;
						Ray_t rayHead;
						rayHead.Init(origin, srcOrigin + Vector(0.0f, 0.0f, 52.0f));
						game->m_EngineTrace->TraceRay(rayHead, CONTENTS_SOLID, &worldFilter, &trHead);

						g_StatOcclusionRays.fetch_add(1, std::memory_order_relaxed);
						if (trFeet.fraction < 0.96f && trHead.fraction < 0.96f)
						{
							targetGain = kOccludedGain;
							g_StatOccludedHits.fetch_add(1, std::memory_order_relaxed);
						}
					}
					st.cachedOcclusionTarget = targetGain;
					st.cachedTargetGuid = guid;
				}

				const float cur = st.occlusionGain.load(std::memory_order_relaxed);
				const float next = cur + (targetGain - cur) * occAlpha;
				st.occlusionGain.store(next, std::memory_order_relaxed);
			}
		}
		else
		{
			g_HasLastOccTime = false;
			g_HasLastOccTraceTime = false;
		}

#ifdef _DEBUG
		// Periodic diagnostic stats (~5s)
		const auto now = std::chrono::steady_clock::now();
		if (std::chrono::duration<double>(now - g_LastLogTime).count() >= 5.0)
		{
			g_LastLogTime = now;
			const uint64_t hrtfC = g_StatHrtfChunks.exchange(0, std::memory_order_relaxed);
			const uint64_t natC = g_StatNativeChunks.exchange(0, std::memory_order_relaxed);
			const uint64_t rays = g_StatOcclusionRays.exchange(0, std::memory_order_relaxed);
			const uint64_t hits = g_StatOccludedHits.exchange(0, std::memory_order_relaxed);
			if (hrtfC > 0 || natC > 0)
			{
				char statBuf[256];
				snprintf(statBuf, sizeof(statBuf),
					"[VRAudio] 5s stats: hrtfChunks=%llu nativeChunks=%llu occlusionRays=%llu occludedHits=%llu",
					static_cast<unsigned long long>(hrtfC),
					static_cast<unsigned long long>(natC),
					static_cast<unsigned long long>(rays),
					static_cast<unsigned long long>(hits));
				WriteLogLine(statBuf);
			}
		}
#endif
	}
}
