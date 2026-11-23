#include "game.h"
#include <Windows.h>
#include <iostream>
#include "sdk.h"
#include "vr.h"
#include "vr_memstack_patch.h"
#include "hooks.h"
#include "offsets.h"
#include "sigscanner.h"

Game::Game()
{
    auto waitForModule = [](const char* name, uintptr_t& base, DWORD timeoutMs = 120000) -> bool {
        DWORD start = GetTickCount();
        while (!(base = (uintptr_t)GetModuleHandleA(name)))
        {
            if (GetTickCount() - start > timeoutMs)
                return false;
            Sleep(50);
        }
        return true;
    };

    uintptr_t dummy = 0;
    if (!waitForModule("client.dll", m_BaseClient) ||
        !waitForModule("engine.dll", m_BaseEngine) ||
        !waitForModule("materialsystem.dll", m_BaseMaterialSystem) ||
        !waitForModule("server.dll", m_BaseServer) ||
        !waitForModule("vgui2.dll", m_BaseVgui2) ||
        !waitForModule("vguimatsurface.dll", dummy))
    {
        errorMsg("Timed out waiting for required Source Engine modules");
        return;
    }

    InfraVRMemPatch::EnsureVRMaterialSystemStackBudgets();

    m_ClientEntityList = (IClientEntityList *)GetInterface("client.dll", "VClientEntityList003");
    m_EngineTrace = (IEngineTrace *)GetInterface("engine.dll", "EngineTraceClient004");
    m_EngineClient = (IEngineClient *)GetInterface("engine.dll", "VEngineClient015");
    m_MaterialSystem = (IMaterialSystem *)GetInterface("MaterialSystem.dll", "VMaterialSystem080");
    m_EngineViewRender = (IViewRender *)GetInterface("engine.dll", "VEngineRenderView013");
    m_ModelInfo = (IModelInfo *)GetInterface("engine.dll", "VModelInfoClient004");
    m_ModelRender = (IModelRender *)GetInterface("engine.dll", "VEngineModel016");
    m_VguiInput = (IInput *)GetInterface("vgui2.dll", "VGUI_InputInternal001");
    m_VguiSurface = (ISurface *)GetInterface("vguimatsurface.dll", "VGUI_Surface031");

    if (!m_ClientEntityList || !m_EngineTrace || !m_EngineClient || !m_MaterialSystem ||
        !m_EngineViewRender || !m_ModelInfo || !m_ModelRender ||
        !m_VguiInput || !m_VguiSurface)
    {
        char errBuf[256];
        snprintf(errBuf, sizeof(errBuf), "Failed to retrieve one or more Source Engine interfaces (CE:%p, ET:%p, EC:%p, MS:%p, EVR:%p, MI:%p, MR:%p, VI:%p, VS:%p)",
            m_ClientEntityList, m_EngineTrace, m_EngineClient, m_MaterialSystem,
            m_EngineViewRender, m_ModelInfo, m_ModelRender,
            m_VguiInput, m_VguiSurface);
        errorMsg(errBuf);
        return;
    }

    m_Offsets = new Offsets();

    if (m_Offsets->g_pClientMode.address)
    {
        IClientMode **ppClientMode = *(IClientMode ***)(m_Offsets->g_pClientMode.address);
        DWORD start = GetTickCount();
        while (!ppClientMode || !(*ppClientMode))
        {
            if (GetTickCount() - start > 120000)
            {
                delete m_Offsets;
                m_Offsets = nullptr;
                errorMsg("Timed out waiting for ClientMode");
                return;
            }
            Sleep(50);
            ppClientMode = *(IClientMode ***)(m_Offsets->g_pClientMode.address);
        }
        m_ClientMode = *ppClientMode;
    }

    m_VR = new VR(this);
    if (!m_VR || !m_VR->m_IsInitialized)
    {
        if (m_VR)
        {
            delete m_VR;
            m_VR = nullptr;
        }
        delete m_Offsets;
        m_Offsets = nullptr;
        errorMsg("VR subsystem initialization failed");
        return;
    }

    g_Game = this;
    m_Initialized = true;

    m_Hooks = new Hooks(this);
}

Game::~Game()
{
    if (m_Hooks)
    {
        delete m_Hooks;
        m_Hooks = nullptr;
    }
    if (m_VR)
    {
        delete m_VR;
        m_VR = nullptr;
    }
    if (m_Offsets)
    {
        delete m_Offsets;
        m_Offsets = nullptr;
    }
    m_Initialized = false;
    if (g_Game == this)
        g_Game = nullptr;
}

void *Game::GetInterface(const char *dllname, const char *interfacename)
{
    HMODULE hMod = GetModuleHandle(dllname);
    if (!hMod)
        return nullptr;

    tCreateInterface CreateInterface = (tCreateInterface)GetProcAddress(hMod, "CreateInterface");
    if (!CreateInterface)
        return nullptr;

    int returnCode = 0;
    return CreateInterface(interfacename, &returnCode);
}

void Game::errorMsg(const char *msg)
{
    MessageBox(0, msg, "INFRA VR", MB_ICONERROR | MB_OK);
}

CBaseEntity *Game::GetClientEntity(int entityIndex)
{
    return (CBaseEntity *)(m_ClientEntityList->GetClientEntity(entityIndex));
}

char *Game::getNetworkName(uintptr_t *entity)
{
    if (!entity)
        return nullptr;

    uintptr_t entityAddr = (uintptr_t)entity;
    uintptr_t *pNetworkable = *(uintptr_t **)(entityAddr + 0x8);
    if (!pNetworkable)
        return nullptr;

    uintptr_t vtableAddr = (uintptr_t)pNetworkable;
    uintptr_t *pGetClientClassFn = *(uintptr_t **)(vtableAddr + 0x8);
    if (!pGetClientClassFn)
        return nullptr;

    uintptr_t fnAddr = (uintptr_t)pGetClientClassFn;
    uintptr_t *pClientClass = *(uintptr_t **)(fnAddr + 0x1);
    if (!pClientClass)
        return nullptr;

    uintptr_t clientClassAddr = (uintptr_t)pClientClass;
    char *m_pNetworkName = *(char **)(clientClassAddr + 0x8);
#ifdef _DEBUG
    int classID = *(int *)(clientClassAddr + 0x10);
    std::cout << "ClassID: " << classID << std::endl;
#endif
    return m_pNetworkName;
}

void Game::ClientCmd(const char *szCmdString)
{
    m_EngineClient->ClientCmd(szCmdString);
}

void Game::ClientCmd_Unrestricted(const char *szCmdString)
{
    m_EngineClient->ClientCmd_Unrestricted(szCmdString);
}
