// dllmain.cpp : Defines the entry point for the DLL application.
#include <Windows.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <regex>
#include "game.h"
#include "hooks.h"
#include "vr.h"
#include "vr_memstack_patch.h"
#include "sdk.h"

static void EnsureVRWindowedVideoConfig(bool patchDisplaySettings)
{
    int winW = 1280;
    int winH = 720;

    std::ifstream vrCfg(VR::ResolveGameRelativePath("VR\\config.txt"));
    if (vrCfg.is_open())
    {
        std::string line;
        while (std::getline(vrCfg, line))
        {
            auto hashPos = line.find('#');
            if (hashPos != std::string::npos)
                line = line.substr(0, hashPos);
            auto eqPos = line.find('=');
            if (eqPos == std::string::npos)
                continue;
            std::string key = line.substr(0, eqPos);
            std::string val = line.substr(eqPos + 1);
            try
            {
                if (key.find("DesktopWindowWidth") != std::string::npos)
                {
                    int w = std::stoi(val);
                    if (w >= 640 && w <= 3840) winW = w;
                }
                else if (key.find("DesktopWindowHeight") != std::string::npos)
                {
                    int h = std::stoi(val);
                    if (h >= 480 && h <= 2160) winH = h;
                }
            }
            catch (...) {}
        }
    }

    const std::string resolvedPaths[] = {
        VR::ResolveGameRelativePath("infra\\cfg\\video.txt"),
        VR::ResolveGameRelativePath("infra\\cfg\\videodefaults.txt")
    };

    for (const std::string& path : resolvedPaths)
    {
        std::ifstream in(path);
        if (!in.is_open())
            continue;
        std::ostringstream ss;
        ss << in.rdbuf();
        std::string txt = ss.str();
        in.close();

        if (txt.empty())
            continue;

        if (path.find("video.txt") != std::string::npos)
        {
            txt = std::regex_replace(txt, std::regex("^\\s*\"config\""), "\"VideoConfig\"");
        }
        txt = std::regex_replace(txt, std::regex("(\"setting\\.fullscreen\"\\s+)\"\\d+\""), "$1\"0\"");
        txt = std::regex_replace(txt, std::regex("(\"setting\\.nowindowborder\"\\s+)\"\\d+\""), "$1\"0\"");
        txt = std::regex_replace(txt, std::regex("(\"setting\\.mat_vsync\"\\s+)\"\\d+\""), "$1\"0\"");
        txt = std::regex_replace(txt, std::regex("(\"setting\\.mat_queue_mode\"\\s+)\"-?\\d+\""), "$1\"0\"");
        txt = std::regex_replace(txt, std::regex("(\"setting\\.defaultres\"\\s+)\"\\d+\""), "$1\"" + std::to_string(winW) + "\"");
        txt = std::regex_replace(txt, std::regex("(\"setting\\.defaultresheight\"\\s+)\"\\d+\""), "$1\"" + std::to_string(winH) + "\"");

        std::ofstream out(path, std::ios::trunc);
        if (out.is_open())
            out << txt;
    }

    if (!patchDisplaySettings)
        return;

    // Block engine.dll (RVA 0x1E1EFF) and Win32 display mode changes from ever altering the physical desktop monitor resolution.
    // Use GetModuleHandleA only (never LoadLibrary inside DllMain under the loader lock).
    HMODULE hUser32 = GetModuleHandleA("user32.dll");
    if (hUser32)
    {
        auto patchStdcallRetZero = [&](const char* funcName, uint16_t stackBytes) {
            uint8_t* fn = (uint8_t*)GetProcAddress(hUser32, funcName);
            if (!fn)
                return;
            DWORD oldProt = 0;
            if (VirtualProtect(fn, 8, PAGE_EXECUTE_READWRITE, &oldProt))
            {
                // mov eax, 0 (DISP_CHANGE_SUCCESSFUL); ret stackBytes
                fn[0] = 0xB8; fn[1] = 0x00; fn[2] = 0x00; fn[3] = 0x00; fn[4] = 0x00;
                fn[5] = 0xC2; fn[6] = (uint8_t)(stackBytes & 0xFF); fn[7] = (uint8_t)((stackBytes >> 8) & 0xFF);
                VirtualProtect(fn, 8, oldProt, &oldProt);
                FlushInstructionCache(GetCurrentProcess(), fn, 8);
            }
        };
        patchStdcallRetZero("ChangeDisplaySettingsA", 8);
        patchStdcallRetZero("ChangeDisplaySettingsW", 8);
        patchStdcallRetZero("ChangeDisplaySettingsExA", 20);
        patchStdcallRetZero("ChangeDisplaySettingsExW", 20);
    }
}

DWORD WINAPI InitInfraVR(HMODULE hModule)
{
#ifdef _DEBUG
    AllocConsole();
    FILE *fp;
    freopen_s(&fp, "CONOUT$", "w", stdout);
#endif

    g_Game = new Game();

    return 0;
}



BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
                     )
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        EnsureVRWindowedVideoConfig(true);
        InfraVRMemPatch::EnsureVRMaterialSystemStackBudgets();
        {
            HANDLE hThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)InitInfraVR, hModule, 0, NULL);
            if (hThread)
            {
                CloseHandle(hThread);
            }
        }
        break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
        break;
    case DLL_PROCESS_DETACH:
        // Only perform full teardown when dynamically unloaded (lpReserved == nullptr).
        // During process termination (lpReserved != nullptr), worker threads have already
        // been terminated by the OS kernel under the loader lock, so joining threads or
        // shutting down OpenVR/MinHook would deadlock on exit.
        if (lpReserved == nullptr && g_Game)
        {
            delete g_Game;
            g_Game = nullptr;
        }
        break;
    }
    return TRUE;
}


