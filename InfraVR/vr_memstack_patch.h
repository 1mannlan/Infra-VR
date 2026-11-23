#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <cstring>

namespace InfraVRMemPatch
{
	constexpr size_t    kRenderDataInitRva       = 0x00028129u;
	constexpr uint32_t  kOrigRenderDataReserve   = 0x00898000u; // 8,800 KiB (~8.59 MiB) in materialsystem.dll
	constexpr uint32_t  kStereoRenderDataReserve = 0x02000000u; // 32 MiB VirtualAlloc(MEM_RESERVE); only 32 KiB committed initially

	constexpr size_t    kCallQueueInitRva        = 0x0000DDB9u;
	constexpr uint32_t  kOrigCallQueueReserve    = 0x00800000u; // 8 MiB in CMatCallQueue::m_Allocator
	constexpr uint32_t  kStereoCallQueueReserve  = 0x01800000u; // 24 MiB VirtualAlloc(MEM_RESERVE) for mat_queue_mode 2 stereo frames

	// Expands Source Engine's per-frame render-data CMemoryStack reserve budgets in materialsystem.dll
	// so rendering two stereo VR eyes between BeginFrame() and EndFrame() never exhausts the flatscreen budget.
	// Safe to invoke under the DLL loader lock when isLiveCode == true (uses GetModuleHandleA only).
	inline bool PatchMaterialSystemStackBudgetsInImage(uintptr_t moduleBase, size_t imageSize = 0, bool isLiveCode = true)
	{
		if (!moduleBase)
			return false;

		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(moduleBase);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x1000)
			return false;

		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(moduleBase + static_cast<uintptr_t>(dos->e_lfanew));
		if (nt->Signature != IMAGE_NT_SIGNATURE)
			return false;

		const size_t peImageSize = static_cast<size_t>(nt->OptionalHeader.SizeOfImage);
		if (peImageSize < 0x110000u)
			return false;

		const size_t effectiveSize = (imageSize > 0 && imageSize < peImageSize) ? imageSize : peImageSize;
		if (effectiveSize <= (kRenderDataInitRva + 11u) || effectiveSize <= (kCallQueueInitRva + 16u))
			return false;

		auto patchImm32 = [isLiveCode](uint8_t* dst, uint32_t val) -> bool {
			if (isLiveCode)
			{
				DWORD oldProt = 0;
				if (!VirtualProtect(dst, sizeof(uint32_t), PAGE_EXECUTE_READWRITE, &oldProt))
					return false;
				std::memcpy(dst, &val, sizeof(uint32_t));
				VirtualProtect(dst, sizeof(uint32_t), oldProt, &oldProt);
				FlushInstructionCache(GetCurrentProcess(), dst, sizeof(uint32_t));
				return true;
			}
			std::memcpy(dst, &val, sizeof(uint32_t));
			return true;
		};

		// 1. CMatRenderContextBase::InitializeRenderData (materialsystem.dll+0x28129):
		//      85 C0           test eax, eax
		//      75 05           jne  +5
		//      BE 00 80 89 00  mov  esi, 898000h
		//      6A 20           push 20h
		uint8_t* pRender = reinterpret_cast<uint8_t*>(moduleBase + kRenderDataInitRva);
		bool renderOk = false;
		if (pRender[0] == 0x85u && pRender[1] == 0xC0u &&
		    pRender[2] == 0x75u && pRender[3] == 0x05u &&
		    pRender[4] == 0xBEu && pRender[9] == 0x6Au && pRender[10] == 0x20u)
		{
			uint32_t curBudget = 0;
			std::memcpy(&curBudget, pRender + 5, sizeof(uint32_t));
			if (curBudget == kStereoRenderDataReserve)
				renderOk = true;
			else if (curBudget == kOrigRenderDataReserve)
				renderOk = patchImm32(pRender + 5, kStereoRenderDataReserve);
		}

		// 2. CMatCallQueue::m_Allocator (materialsystem.dll+0xDDB9, used under mat_queue_mode 2):
		//      68 00 00 04 00  push 40000h
		//      68 00 00 01 00  push 10000h
		//      68 00 00 80 00  push 800000h
		//      8D ...          lea  ecx, ...
		static constexpr uint8_t kCallQueuePrefix[11] = {
			0x68u, 0x00u, 0x00u, 0x04u, 0x00u,
			0x68u, 0x00u, 0x00u, 0x01u, 0x00u,
			0x68u
		};
		uint8_t* pQueue = reinterpret_cast<uint8_t*>(moduleBase + kCallQueueInitRva);
		bool queueOk = false;
		if (std::memcmp(pQueue, kCallQueuePrefix, sizeof(kCallQueuePrefix)) == 0 && pQueue[15] == 0x8Du)
		{
			uint32_t curBudget = 0;
			std::memcpy(&curBudget, pQueue + 11, sizeof(uint32_t));
			if (curBudget == kStereoCallQueueReserve)
				queueOk = true;
			else if (curBudget == kOrigCallQueueReserve)
				queueOk = patchImm32(pQueue + 11, kStereoCallQueueReserve);
		}

		return renderOk && queueOk;
	}

	inline bool EnsureVRMaterialSystemStackBudgets()
	{
		HMODULE hMatSys = GetModuleHandleA("materialsystem.dll");
		if (!hMatSys)
			return false;
		return PatchMaterialSystemStackBudgetsInImage(reinterpret_cast<uintptr_t>(hMatSys), 0u, true);
	}
}
