#include "../dxvk/dxvk_include.h"

#include "d3d9_vr.h"

#include "d3d9_include.h"
#include "d3d9_surface.h"

#include "d3d9_device.h"

#include "InfraVR/game.h"
#include "InfraVR/vr.h"

#include <fstream>
#include <sstream>
#include <string>
#include <algorithm>
#include <cmath>

std::mutex& GetOpenVRRuntimeMutex()
{
    static std::mutex s_OpenVRRuntimeMutex;
    return s_OpenVRRuntimeMutex;
}

void GetConfiguredVRRenderTargetSize(uint32_t* outWidth, uint32_t* outHeight, float* outScale)
{
    static uint32_t s_cachedWidth = 0;
    static uint32_t s_cachedHeight = 0;
    static float s_cachedScale = 1.0f;

    if (s_cachedWidth == 0 || s_cachedHeight == 0)
    {
        float renderScale = 1.0f;
        std::ifstream fileStream("VR\\config.txt");
        if (!fileStream.is_open())
        {
            char exePath[MAX_PATH] = {};
            if (GetModuleFileNameA(NULL, exePath, MAX_PATH) > 0)
            {
                char* lastSlash = strrchr(exePath, '\\');
                if (lastSlash)
                {
                    *(lastSlash + 1) = '\0';
                    std::string cfgPath = std::string(exePath) + "VR\\config.txt";
                    fileStream.open(cfgPath);
                }
            }
        }
        if (fileStream.is_open())
        {
            std::string line;
            while (std::getline(fileStream, line))
            {
                auto hashPos = line.find('#');
                if (hashPos != std::string::npos)
                    line.erase(hashPos);
                auto eqPos = line.find('=');
                if (eqPos == std::string::npos)
                    continue;
                std::string key = line.substr(0, eqPos);
                std::string value = line.substr(eqPos + 1);
                while (!key.empty() && (key.back() == ' ' || key.back() == '\t' || key.back() == '\r'))
                    key.pop_back();
                while (!key.empty() && (key.front() == ' ' || key.front() == '\t'))
                    key.erase(key.begin());
                if (key == "RenderScale")
                {
                    try { renderScale = std::stof(value); } catch (...) {}
                }
            }
        }
        renderScale = std::clamp(renderScale, 0.4f, 2.0f);
        s_cachedScale = renderScale;

        uint32_t baseWidth = 0, baseHeight = 0;
        if (vr::VRSystem())
        {
            vr::VRSystem()->GetRecommendedRenderTargetSize(&baseWidth, &baseHeight);
        }
        if (baseWidth > 0 && baseHeight > 0)
        {
            uint32_t scaledW = std::max(1280u, ((uint32_t)((float)baseWidth  * renderScale)) & ~1u);
            uint32_t scaledH = std::max(720u,  ((uint32_t)((float)baseHeight * renderScale)) & ~1u);

            // Source Engine's materialsystem.dll caps named render targets (leftEye0, rightEye0, _rt_FullFrameFB)
            // at 4096x4096. Clamp the maximum dimension to 3584 while preserving the exact HMD aspect ratio
            // so Refract (glass) and HDR tonemapping StretchRect blits never exceed _rt_FullFrameFB's extent.
            const uint32_t maxAllowedDim = 3584u;
            uint32_t maxDim = std::max(scaledW, scaledH);
            if (maxDim > maxAllowedDim)
            {
                float clampFactor = (float)maxAllowedDim / (float)maxDim;
                scaledW = std::max(1280u, ((uint32_t)((float)scaledW * clampFactor)) & ~1u);
                scaledH = std::max(720u,  ((uint32_t)((float)scaledH * clampFactor)) & ~1u);
            }

            s_cachedWidth  = scaledW;
            s_cachedHeight = scaledH;
        }
    }

    if (outWidth)  *outWidth  = s_cachedWidth;
    if (outHeight) *outHeight = s_cachedHeight;
    if (outScale)  *outScale  = s_cachedScale;
}

void EnforceVRWindowedMode(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd))
        return;

    RECT clientRect = {};
    GetClientRect(hwnd, &clientRect);
    int clientW = clientRect.right - clientRect.left;
    int clientH = clientRect.bottom - clientRect.top;

    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    LONG exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);

    if ((style & WS_POPUP) != 0 || (style & WS_CAPTION) == 0 || (exStyle & WS_EX_TOPMOST) != 0 || clientW > 1600 || clientH > 900)
    {
        LONG newStyle = (style & ~WS_POPUP) | WS_OVERLAPPEDWINDOW | WS_VISIBLE;
        LONG newExStyle = exStyle & ~WS_EX_TOPMOST;
        SetWindowLongW(hwnd, GWL_STYLE, newStyle);
        SetWindowLongW(hwnd, GWL_EXSTYLE, newExStyle);

        RECT winRect = { 0, 0, 1280, 720 };
        AdjustWindowRectEx(&winRect, newStyle, FALSE, newExStyle);
        int winW = winRect.right - winRect.left;
        int winH = winRect.bottom - winRect.top;

        SetWindowPos(hwnd, HWND_NOTOPMOST, 60, 60, winW, winH,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE);
    }
}

namespace dxvk {

    class D3D9VR final : public ComObjectClamp<IDirect3DVR9>
    {
    public:

        D3D9VR(IDirect3DDevice9 *pDevice)
            : m_device(static_cast<D3D9DeviceEx *>(pDevice))
        {}

        HRESULT STDMETHODCALLTYPE QueryInterface(
            REFIID riid,
            void **ppvObject)
        {
            if (ppvObject == nullptr)
                return E_POINTER;

            *ppvObject = nullptr;

            if (riid == __uuidof(IUnknown) ||
                riid == __uuidof(IDirect3DVR9)) {
                *ppvObject = ref(this);
                return S_OK;
            }

            Logger::warn("D3D9VR::QueryInterface: Unknown interface query");
            Logger::warn(str::format(riid));
            return E_NOINTERFACE;
        }

        HRESULT STDMETHODCALLTYPE GetVRDesc(
            IDirect3DSurface9 *pSurface,
            D3D9_TEXTURE_VR_DESC *pDesc)
        {
            if (unlikely(pSurface == nullptr || pDesc == nullptr))
                return D3DERR_INVALIDCALL;

            D3D9Surface *surface = static_cast<D3D9Surface *>(pSurface);

            const auto *tex = surface->GetCommonTexture();
            if (unlikely(tex == nullptr))
                return D3DERR_INVALIDCALL;

            const auto &desc = tex->Desc();
            const auto &image = desc->MultiSample != D3DMULTISAMPLE_NONE ? const_cast<D3D9CommonTexture*>(tex)->GetResolveImage() : tex->GetImage();
            if (unlikely(image == nullptr))
                return D3DERR_INVALIDCALL;
            const auto &device = tex->Device()->GetDXVKDevice();

            // I don't know why the image randomly is a uint64_t in OpenVR.
            pDesc->Image = uint64_t(image->handle());
            pDesc->Device = device->handle();
            pDesc->PhysicalDevice = device->adapter()->handle();
            pDesc->Instance = device->instance()->handle();
            pDesc->Queue = device->queues().graphics.queueHandle;
            pDesc->QueueFamilyIndex = device->queues().graphics.queueFamily;

            pDesc->Width = desc->Width;
            pDesc->Height = desc->Height;
            pDesc->Format = tex->GetFormatMapping().FormatColor;
            pDesc->SampleCount = uint32_t(image->info().sampleCount);

            return D3D_OK;
        }

        HRESULT STDMETHODCALLTYPE TransferSurface(
            IDirect3DSurface9 *pSurface,
            BOOL waitResourceIdle)
        {
            if (unlikely(pSurface == nullptr))
                return D3DERR_INVALIDCALL;

            auto *tex = static_cast<D3D9Surface *>(pSurface)->GetCommonTexture();
            if (unlikely(tex == nullptr))
                return D3DERR_INVALIDCALL;

            const auto &image = tex->GetImage();
            if (unlikely(image == nullptr))
                return D3DERR_INVALIDCALL;

            VkImageSubresourceRange subresources = {
              VK_IMAGE_ASPECT_COLOR_BIT,
              0, image->info().mipLevels,
              0, image->info().numLayers
            };

            m_device->TransformImage(
                tex, &subresources,
                image->info().layout,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

            // This wait may need to be on all Faces and Mip Levels (2 loops).
            if (waitResourceIdle)
                m_device->WaitForResource(image, tex->GetMappingBufferSequenceNumber(0u), D3DLOCK_READONLY);

            return D3D_OK;
        }

        HRESULT STDMETHODCALLTYPE LockDevice()
        {
            m_lock = m_device->LockDevice();
            return D3D_OK;
        }

        HRESULT STDMETHODCALLTYPE UnlockDevice()
        {
            m_lock = D3D9DeviceLock();
            return D3D_OK;
        }

        HRESULT STDMETHODCALLTYPE WaitDeviceIdle()
        {
            m_device->Flush();
            // Not clear if we need all here, perhaps...
            m_device->SynchronizeCsThread(DxvkCsThread::SynchronizeAll);
            m_device->GetDXVKDevice()->waitForIdle();
            return D3D_OK;
        }

        HRESULT STDMETHODCALLTYPE BeginVRSubmit()
        {
            m_device->Flush();
            m_device->SynchronizeCsThread(DxvkCsThread::SynchronizeAll);
            m_device->GetDXVKDevice()->lockSubmission();
            return D3D_OK;
        }

        HRESULT STDMETHODCALLTYPE EndVRSubmit()
        {
            m_device->GetDXVKDevice()->unlockSubmission();
            return D3D_OK;
        }

        HRESULT STDMETHODCALLTYPE GetBackBufferData(SharedTextureHolder* backBufferData)
        {
            if (unlikely(backBufferData == nullptr))
                return D3DERR_INVALIDCALL;

            IDirect3DSurface9 *backBufferSurface = nullptr;

            HRESULT res = m_device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBufferSurface);
            if (FAILED(res) || !backBufferSurface)
                return res;

            D3D9_TEXTURE_VR_DESC textureDesc = {};
            HRESULT vrRes = GetVRDesc(backBufferSurface, &textureDesc);
            backBufferSurface->Release();
            if (FAILED(vrRes))
                return vrRes;

            memcpy(&backBufferData->m_VulkanData, &textureDesc, sizeof(vr::VRVulkanTextureData_t));
            backBufferData->m_VRTexture.handle = &backBufferData->m_VulkanData;
            backBufferData->m_VRTexture.eColorSpace = vr::ColorSpace_Auto;
            backBufferData->m_VRTexture.eType = vr::TextureType_Vulkan;
        
            return res;
        }

    private:
        D3D9DeviceEx *m_device;
        D3D9DeviceLock m_lock;
    };

}

HRESULT __stdcall Direct3DCreateVRImpl(IDirect3DDevice9 *pDevice, IDirect3DVR9 **pInterface) {
    if (pInterface == nullptr)
        return D3DERR_INVALIDCALL;

    *pInterface = new dxvk::D3D9VR(pDevice);

    return D3D_OK;
}