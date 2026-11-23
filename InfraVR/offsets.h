#pragma once
#include "sigscanner.h"
#include "game.h"


struct Offset
{
    std::string moduleName;
    int offset = 0;
    uintptr_t address = 0;
    std::string signature;
    int sigOffset = 0;

    Offset(std::string moduleName, int currentOffset, std::string signature, int sigOffset = 0)
    {
        this->moduleName = moduleName;
        this->offset = currentOffset;
        this->signature = signature;
        this->sigOffset = sigOffset;

        int newOffset = SigScanner::VerifyOffset(moduleName, currentOffset, signature, sigOffset);
        if (newOffset > 0)
        {
            this->offset = newOffset;
        }
            
        if (newOffset == -1)
        {
            Game::errorMsg(("Signature not found: " + signature).c_str());
            return;
        }

        this->address = (uintptr_t)GetModuleHandle(moduleName.c_str()) + this->offset;
    }
};

class Offsets
{
public:
    Offset RenderView =                  { "client.dll", 0x198E90, "55 8B EC 83 EC 2C 53 56 8B F1 6A 00 8D 8E ? ? ? ? E8 ? ? ? ?" };
    Offset g_pClientMode =               { "client.dll", 0x1F6E03, "8B 0D ? ? ? ? 8B 01 8B 90 04 02 00 00 FF D2 8B 04 85 ? ? ? ? C3", 19 };
    Offset CalcViewModelView =           { "client.dll", 0x1EA9E0, "55 8B EC 83 EC 34 53 8B D9 80 BB" };
    Offset CreateMove =                  { "client.dll", 0xA4910,  "55 8B EC 6A FF E8 ? ? ? ? 83 C4 04 85 C0 75 06 B0 01 5D C2 08 00" };
    Offset WriteUsercmd =                { "client.dll", 0x166B80, "55 8B EC 8B 0D ? ? ? ? 81 F9 ? ? ? ? 75 07 A1 ? ? ? ? EB 07 8B 01 8B 50 34 FF D2 53 8B 5D 0C 56" };

    Offset ReadUserCmd =                 { "server.dll", 0x23A800, "55 8B EC 83 EC 08 53 8B 5D 10 56 57 8B 7D 0C 53" };
    Offset ProcessUsercmds =             { "server.dll", 0x1453C0, "55 8B EC B8 ? ? ? ? E8 ? ? ? ? 0F 57 C0 53 56 57 B9 ? ? ? ? 8D 85 ? ? ? ? 33 DB" };
    Offset CBaseEntity_entindex =        { "server.dll", 0xD6B40,  "8B 41 1C 85 C0 75 01 C3 8B 0D ? ? ? ? 2B 41 58 C1 F8 04 C3 CC" };
    Offset EyePosition =                 { "server.dll", 0x34D7E0, "55 8B EC 83 EC 18 56 8B F1 8B 86 4C 15 00 00 8B 16 F3 0F 7E 86 44 15 00 00" };

    // Aim related
    Offset CHudCrosshair_ShouldDraw =    { "client.dll", 0xF6500,  "57 8B F9 80 BF ? ? ? ? ? 74 04 32 C0 5F C3" };

    // Grabbables & Hand Interaction
    Offset Weapon_ShootPosition =        { "server.dll", 0x34BBC0, "55 8B EC 8B 01 8B 90 ? ? ? ? 56 8B 75 08 56 FF D2 8B C6 5E 5D C2 04 00" };
    Offset UpdateObject =                { "server.dll", 0x3A9820, "53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04 89 6C 24 04 8B EC 81 EC ? ? ? ? 56 57 8B F9 8B 87 ? ? ? ? 89 BD" };
    Offset UpdateObjectVM =              { "server.dll", 0x3AB320, "53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04 89 6C 24 04 8B EC 81 EC ? ? ? ? 56 57 8B F9 8B 87 ? ? ? ? 83 F8" };
    Offset EyeAngles =                   { "server.dll", 0xD78F0,  "55 8B EC 8B 81 ? ? ? ? 83 EC 60 56 57 8B 3D ? ? ? ? 83 F8 FF 74 1D" };

    // FOV
    Offset GetFOV =                      { "client.dll", 0x1E4800, "55 8B EC 51 56 8B F1 E8 ? ? ? ? D9 5D FC 8B 06 8B 90 ? ? ? ? 8B CE FF D2" };
    Offset GetDefaultFOV =               { "client.dll", 0x1B2880, "D9 05 ? ? ? ? C3 CC CC CC CC CC CC CC CC CC 8A 81 84 1E 00 00 C3" };
    Offset GetViewModelFOV =             { "client.dll", 0x1E6400, "55 8B EC 83 EC 0C 56 8B F1 8B 06 8B 90 80 02 00 00 FF D2" };

    // Entity Owner
    Offset GetOwner =                    { "server.dll", 0xAD450,  "8B 81 ? ? ? ? 83 F8 FF 74 23 8B 15 ? ? ? ?" };

    // INFRA-specific VR Mechanics (Flashlight, Camera, +use Hand Interaction, Pause Menu)
    Offset UpdateFlashlight =            { "client.dll", 0x1B3BE0, "55 8B EC 81 EC 8C 00 00 00 53 56 57 8B F1 E8 ? ? ? ? 8B 0D" };
    Offset ClientEyeAngles =             { "client.dll", 0x1E46C0, "55 8B EC 83 EC 2C B8 01 00 00 00 56 8B F1 84 05 ? ? ? ? 75 06" };
    Offset ClientEyePosition =           { "client.dll", 0x2154F0, "55 8B EC 83 EC 18 56 8B F1 8B 86 44 1E 00 00 8B 16" };
    Offset ClientCameraThink =           { "client.dll", 0x1AE7B0, "55 8B EC 83 EC 44 53 56 57 8B F9 8B 0D ? ? ? ? 8B 01 8B 90 04 02 00 00" };
    Offset ShouldUsePhonePause =         { "client.dll", 0x1BDB30, "80 3D ? ? ? ? 00 74 03 32 C0 C3 56 6A FF E8" };
    Offset ClientFindUseEntity =         { "client.dll", 0x1C9430, "53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04 89 6C 24 04 8B EC B8 A8 14 00 00" };
    Offset FindUseEntity =               { "server.dll", 0x2BF390, "53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04 89 6C 24 04 8B EC B8 A8 14 00 00 E8 ? ? ? ? 56 57 8B F9" };
    Offset CameraPrimaryAttack =         { "server.dll", 0x28DAF0, "55 8B EC 51 53 57 8B D9 E8 ? ? ? ? 8B F8 85 FF 0F 84" };
    Offset CameraSecondaryAttack =       { "server.dll", 0x28DE20, "55 8B EC 83 EC 08 56 8B F1 E8 ? ? ? ? 8B 06 8B 90 34 04 00 00" };
    Offset CameraItemPostFrame =         { "server.dll", 0x28E0F0, "56 8B F1 E8 ? ? ? ? 80 BE 0D 05 00 00 00 75 4D 57 8B CE" };
    Offset CameraShouldFlash =           { "client.dll", 0x1ADFF0, "55 8B EC A1 ? ? ? ? 8B 50 34 83 EC 08 56 8B F1 B9 ? ? ? ? FF D2" };
    Offset CameraOnPhotoTaken =          { "client.dll", 0x1ADDA0, "A1 ? ? ? ? D9 40 0C D9 99 C8 0E 00 00 8B 15 ? ? ? ? 8B 42 04 89" };
    Offset CameraFlashlightUpdateLight = { "client.dll", 0x1AEAD0, "55 8B EC 81 EC 24 01 00 00 56 8B F1 80 3E 00 0F 84" };
    Offset FlashlightHideThink =         { "server.dll", 0x29E9E0, "56 57 8B 3D ? ? ? ? 8B 07 8B 50 78 8B F1" };
    Offset ReloadFlashlight =            { "server.dll", 0x2A1B90, "53 8B 1D ? ? ? ? 8B 03 8B 50 78 57 8B F9 8B CB FF D2 8B CF E8" };
    Offset VGuiScreenGetCursorPos =      { "client.dll", 0x8FDD0,  "53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04 89 6C 24 04 8B EC 81 EC 98 00 00 00 89 4D FC" };
    Offset FindVGuiScreenUnderCursor =   { "client.dll", 0x90BA0,  "53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04 89 6C 24 04 8B EC 81 EC D8 00 00 00 56 57 6A FF" };

    // Spatial Audio (Steam Audio 3D Binaural Pipeline in engine.dll)
    Offset S_SpatializeChannel =                 { "engine.dll", 0x014E30, "53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04 89 6C 24 04 8B EC 81 EC 98 02 00 00 56 8B 73 08" };
    Offset AudioDevice_SpatializeChannelOpStack ={ "engine.dll", 0x005950, "55 8B EC 83 EC 38 0F 57 C0 56 8B 75 0C 57 8D 45 D4" };
    Offset AudioDevice_SpatializeChannelLegacy = { "engine.dll", 0x004690, "55 8B EC 83 EC 38 0F 57 C0 53 56 8B 75 0C 57 33" };
    Offset MIX_MixChannelsToPaintbuffer =        { "engine.dll", 0x02F050, "55 8B EC 83 EC 14 B8 44 AC 00 00 99 F7 7D 1C 8B" };
    Offset AudioDevice_Mix8Mono =                { "engine.dll", 0x0042F0, "55 8B EC 83 EC 30 56 E8 04 5A 02 00 8B 4D 08 8B" };
    Offset AudioDevice_Mix16Mono =               { "engine.dll", 0x004490, "55 8B EC 83 EC 30 56 E8 64 58 02 00 8B 4D 08 8B" };
    Offset MIX_ScaleChannelVolume =              { "engine.dll", 0x02D600, "55 8B EC 83 EC 08 8B 45 08 8B 48 04 53 8B 5D 0C" };
    Offset S_FreeChannel =                       { "engine.dll", 0x02EEF0, "55 8B EC 81 EC 04 01 00 00 56 8B 75 08 8A 86 49" };
    Offset CSosOpStack_Execute =                 { "engine.dll", 0x04F0E0, "55 8B EC 8B 45 08 48 56 8B F1 74 1E 83 E8 03 75" };
};
