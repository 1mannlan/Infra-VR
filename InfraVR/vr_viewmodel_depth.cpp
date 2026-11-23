#include "vr_viewmodel_depth.h"
#include "sigscanner.h"

#include <Windows.h>
#include <psapi.h>

#include <atomic>
#include <cstring>
#include <mutex>

namespace
{
	// Verified client.dll RVAs (ImageBase = 0x10000000) in CViewRender::DrawViewModels (+0x198820):
	// Pass 1 (+0x1989DE) and Pass 2 (+0x198D79) load 0.1f from .rdata (+0x48031C) into xmm0
	// for IMatRenderContext::DepthRange(0.0f, 0.1f), then Restore 1 (+0x198CDF) and Restore 2
	// (+0x198DEC) load 1.0f from .rdata (+0x47EF68) for DepthRange(0.0f, 1.0f).
	// Patching only the two 4-byte movss disp32 operands inside DrawViewModels preserves all
	// other users of shared 0.1f .rdata and leaves vehicle mode (ClearBuffers) untouched.
	constexpr int kPass1ImmOffset    = 0x1989E2;
	constexpr int kRestore1ImmOffset = 0x198CE3;
	constexpr int kPass2ImmOffset    = 0x198D7D;
	constexpr int kRestore2ImmOffset = 0x198DF0;

	constexpr uint32_t kFloat01Bits = 0x3DCCCCCDu; // 0.1f IEEE-754
	constexpr uint32_t kFloat10Bits = 0x3F800000u; // 1.0f IEEE-754

	// Each pattern uniquely matches a single site in client.dll.
	constexpr const char* kPass1Sig =
		"84 C0 0F 94 C0 88 45 FE 84 C0 8B 03 8B CB 75 0D 8B 50 30 6A 00 6A 01 6A 00 FF D2 EB 1E "
		"F3 0F 10 05 ?? ?? ?? ?? 8B 50 2C 83 EC 08 F3 0F 11 44 24 04 0F 57 C0 F3 0F 11 04 24 FF D2";
	constexpr int kPass1SigDispOffset = 0x21;

	constexpr const char* kRestore1Sig =
		"8D 95 C8 FE FF FF 52 8B CF E8 ?? ?? ?? ?? 80 7D FE 00 74 22 "
		"F3 0F 10 05 ?? ?? ?? ?? 8B 03 8B 50 2C 83 EC 08 F3 0F 11 44 24 04 0F 57 C0 8B CB F3 0F 11 04 24 FF D2";
	constexpr int kRestore1SigDispOffset = 0x18;

	constexpr const char* kPass2Sig =
		"53 FF D2 80 7D FE 00 74 22 "
		"F3 0F 10 05 ?? ?? ?? ?? 8B 03 8B 50 2C 83 EC 08 F3 0F 11 44 24 04 0F 57 C0 8B CB F3 0F 11 04 24 FF D2";
	constexpr int kPass2SigDispOffset = 0x0D;

	constexpr const char* kRestore2Sig =
		"80 7D FE 00 8B 7D F4 74 22 "
		"F3 0F 10 05 ?? ?? ?? ?? 8B 17 8B 42 2C 83 EC 08 F3 0F 11 44 24 04 0F 57 C0 8B CF F3 0F 11 04 24 FF D0";
	constexpr int kRestore2SigDispOffset = 0x0D;

	std::mutex g_Mutex;
	bool g_Resolved = false;
	bool g_Enabled = false;
	uintptr_t g_ClientBase = 0;
	uintptr_t g_Pass1ImmAddr = 0;
	uintptr_t g_Pass2ImmAddr = 0;
	uint32_t g_Float01Va = 0;
	uint32_t g_Float10Va = 0;

	void ResetStateLocked()
	{
		g_Resolved = false;
		g_Enabled = false;
		g_ClientBase = 0;
		g_Pass1ImmAddr = 0;
		g_Pass2ImmAddr = 0;
		g_Float01Va = 0;
		g_Float10Va = 0;
	}

	bool IsInModuleRange(uintptr_t addr, size_t size, uintptr_t moduleBase, uintptr_t moduleEnd)
	{
		return addr >= moduleBase && addr <= moduleEnd && (moduleEnd - addr) >= size;
	}

	bool ReadU32(uintptr_t addr, uint32_t& outVal)
	{
		std::memcpy(&outVal, reinterpret_cast<const void*>(addr), sizeof(outVal));
		return true;
	}

	int ResolveVerifiedRva(int expectedRva, const char* sig, int sigOffset)
	{
		const int res = SigScanner::VerifyOffset("client.dll", expectedRva, sig, sigOffset);
		if (res == 0)
			return expectedRva;
		if (res > 0)
			return res;
		return -1;
	}

	bool WriteCodeU32(uintptr_t addr, uint32_t value)
	{
		uint32_t current = 0;
		ReadU32(addr, current);
		if (current == value)
			return true;

		void* target = reinterpret_cast<void*>(addr);
		MEMORY_BASIC_INFORMATION mbi{};
		bool alreadyWritable = false;
		if (VirtualQuery(target, &mbi, sizeof(mbi)) == sizeof(mbi))
		{
			const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
			const DWORD writableFlags = PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY;
			if (addr + sizeof(value) <= regionEnd &&
			    (mbi.Protect & writableFlags) != 0 &&
			    (mbi.Protect & PAGE_GUARD) == 0)
			{
				alreadyWritable = true;
			}
		}

		if (alreadyWritable)
		{
			std::memcpy(target, &value, sizeof(value));
			std::atomic_thread_fence(std::memory_order_seq_cst);
			FlushInstructionCache(GetCurrentProcess(), target, sizeof(value));
			return true;
		}

		DWORD oldProt = 0;
		if (!VirtualProtect(target, sizeof(value), PAGE_EXECUTE_READWRITE, &oldProt))
			return false;

		std::memcpy(target, &value, sizeof(value));
		std::atomic_thread_fence(std::memory_order_seq_cst);

		DWORD dummyProt = 0;
		VirtualProtect(target, sizeof(value), oldProt, &dummyProt);
		FlushInstructionCache(GetCurrentProcess(), target, sizeof(value));
		return true;
	}

	bool EnsureResolvedLocked(uintptr_t requestedBase)
	{
		HMODULE hClient = GetModuleHandleA("client.dll");
		if (!hClient)
		{
			ResetStateLocked();
			return false;
		}

		const uintptr_t actualBase = reinterpret_cast<uintptr_t>(hClient);
		if (requestedBase != 0 && requestedBase != actualBase)
			return false;

		if (g_Resolved && g_ClientBase == actualBase)
			return true;

		if (g_Resolved && g_ClientBase != actualBase)
			ResetStateLocked();

		MODULEINFO modInfo{};
		if (!GetModuleInformation(GetCurrentProcess(), hClient, &modInfo, sizeof(modInfo)) ||
		    reinterpret_cast<uintptr_t>(modInfo.lpBaseOfDll) != actualBase ||
		    modInfo.SizeOfImage == 0)
		{
			return false;
		}

		const uintptr_t moduleEnd = actualBase + static_cast<uintptr_t>(modInfo.SizeOfImage);

		const int pass1Rva    = ResolveVerifiedRva(kPass1ImmOffset, kPass1Sig, kPass1SigDispOffset);
		const int restore1Rva = ResolveVerifiedRva(kRestore1ImmOffset, kRestore1Sig, kRestore1SigDispOffset);
		const int pass2Rva    = ResolveVerifiedRva(kPass2ImmOffset, kPass2Sig, kPass2SigDispOffset);
		const int restore2Rva = ResolveVerifiedRva(kRestore2ImmOffset, kRestore2Sig, kRestore2SigDispOffset);
		if (pass1Rva <= 0 || restore1Rva <= 0 || pass2Rva <= 0 || restore2Rva <= 0)
			return false;

		const uintptr_t pass1Addr    = actualBase + static_cast<uintptr_t>(pass1Rva);
		const uintptr_t restore1Addr = actualBase + static_cast<uintptr_t>(restore1Rva);
		const uintptr_t pass2Addr    = actualBase + static_cast<uintptr_t>(pass2Rva);
		const uintptr_t restore2Addr = actualBase + static_cast<uintptr_t>(restore2Rva);

		if (!IsInModuleRange(pass1Addr, sizeof(uint32_t), actualBase, moduleEnd) ||
		    !IsInModuleRange(restore1Addr, sizeof(uint32_t), actualBase, moduleEnd) ||
		    !IsInModuleRange(pass2Addr, sizeof(uint32_t), actualBase, moduleEnd) ||
		    !IsInModuleRange(restore2Addr, sizeof(uint32_t), actualBase, moduleEnd))
		{
			return false;
		}

		if (pass1Addr == pass2Addr)
			return false;

		uint32_t pass1Ptr = 0;
		uint32_t restore1Ptr = 0;
		uint32_t pass2Ptr = 0;
		uint32_t restore2Ptr = 0;
		ReadU32(pass1Addr, pass1Ptr);
		ReadU32(restore1Addr, restore1Ptr);
		ReadU32(pass2Addr, pass2Ptr);
		ReadU32(restore2Addr, restore2Ptr);

		if (restore1Ptr == 0 || restore1Ptr != restore2Ptr ||
		    !IsInModuleRange(static_cast<uintptr_t>(restore1Ptr), sizeof(uint32_t), actualBase, moduleEnd))
		{
			return false;
		}

		if (pass1Ptr == 0 || pass1Ptr != pass2Ptr || pass1Ptr == restore1Ptr ||
		    !IsInModuleRange(static_cast<uintptr_t>(pass1Ptr), sizeof(uint32_t), actualBase, moduleEnd))
		{
			return false;
		}

		uint32_t restoreFloatBits = 0;
		uint32_t passFloatBits = 0;
		ReadU32(static_cast<uintptr_t>(restore1Ptr), restoreFloatBits);
		ReadU32(static_cast<uintptr_t>(pass1Ptr), passFloatBits);
		if (restoreFloatBits != kFloat10Bits || passFloatBits != kFloat01Bits)
			return false;

		g_ClientBase = actualBase;
		g_Pass1ImmAddr = pass1Addr;
		g_Pass2ImmAddr = pass2Addr;
		g_Float01Va = pass1Ptr;
		g_Float10Va = restore1Ptr;
		g_Resolved = true;
		g_Enabled = false;
		return true;
	}

	bool ApplyStateLocked(bool enabled)
	{
		if (!g_Resolved)
			return false;

		if (GetModuleHandleA("client.dll") != reinterpret_cast<HMODULE>(g_ClientBase))
		{
			ResetStateLocked();
			return false;
		}

		uint32_t cur1 = 0;
		uint32_t cur2 = 0;
		ReadU32(g_Pass1ImmAddr, cur1);
		ReadU32(g_Pass2ImmAddr, cur2);

		if ((cur1 != g_Float01Va && cur1 != g_Float10Va) ||
		    (cur2 != g_Float01Va && cur2 != g_Float10Va))
		{
			return false;
		}

		const uint32_t desiredVal = enabled ? g_Float10Va : g_Float01Va;
		if (cur1 == desiredVal && cur2 == desiredVal)
		{
			g_Enabled = enabled;
			return true;
		}

		if (!WriteCodeU32(g_Pass1ImmAddr, desiredVal))
			return false;

		if (!WriteCodeU32(g_Pass2ImmAddr, desiredVal))
		{
			WriteCodeU32(g_Pass1ImmAddr, cur1);
			return false;
		}

		g_Enabled = enabled;
		return true;
	}
}

namespace VRViewModelDepth
{
	bool SetEnabled(uintptr_t clientBase, bool enabled)
	{
		std::lock_guard<std::mutex> lock(g_Mutex);
		if (!EnsureResolvedLocked(clientBase))
			return false;

		if (g_Enabled == enabled)
		{
			if (GetModuleHandleA("client.dll") != reinterpret_cast<HMODULE>(g_ClientBase))
			{
				ResetStateLocked();
				return false;
			}
			return true;
		}

		return ApplyStateLocked(enabled);
	}

	bool SetEnabled(bool enabled)
	{
		return SetEnabled(0, enabled);
	}

	bool IsEnabled()
	{
		std::lock_guard<std::mutex> lock(g_Mutex);
		return g_Resolved && g_Enabled;
	}

	bool IsResolved()
	{
		std::lock_guard<std::mutex> lock(g_Mutex);
		return g_Resolved;
	}

	void Shutdown()
	{
		std::lock_guard<std::mutex> lock(g_Mutex);
		if (g_Resolved && g_Enabled)
		{
			ApplyStateLocked(false);
		}
		ResetStateLocked();
	}
}
