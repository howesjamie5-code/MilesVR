// Which GPU the OpenXR runtime wants for D3D12 (xrGetD3D12GraphicsRequirementsKHR), and every
// DXGI adapter with its LUID, so a "Game GPU does not match OpenXR GPU" can be read. Never starts
// a session.
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#define XR_USE_GRAPHICS_API_D3D12
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <cstdio>

int main() {
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 d{};
            adapter->GetDesc1(&d);
            std::printf("DXGI adapter %u: %ls  LUID %08lx:%08lx%s\n", i, d.Description, d.AdapterLuid.HighPart,
                        d.AdapterLuid.LowPart, (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? " (software)" : "");
        }
    }
    const char* extensions[] = {XR_KHR_D3D12_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
    std::snprintf(create.applicationInfo.applicationName, sizeof(create.applicationInfo.applicationName),
                  "MilesVR adapter probe");
    create.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    create.enabledExtensionCount = 1;
    create.enabledExtensionNames = extensions;
    XrInstance instance{};
    if (XR_FAILED(xrCreateInstance(&create, &instance))) {
        std::printf("OpenXR instance unavailable\n");
        return 2;
    }
    XrSystemGetInfo info{XR_TYPE_SYSTEM_GET_INFO};
    info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId system{};
    int status = 3;
    PFN_xrGetD3D12GraphicsRequirementsKHR get{};
    if (XR_SUCCEEDED(xrGetSystem(instance, &info, &system)) &&
        XR_SUCCEEDED(xrGetInstanceProcAddr(instance, "xrGetD3D12GraphicsRequirementsKHR",
                                           reinterpret_cast<PFN_xrVoidFunction*>(&get)))) {
        XrGraphicsRequirementsD3D12KHR r{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
        if (XR_SUCCEEDED(get(instance, system, &r))) {
            std::printf("OpenXR wants LUID %08lx:%08lx\n", r.adapterLuid.HighPart, r.adapterLuid.LowPart);
            status = 0;
        }
    }
    xrDestroyInstance(instance);
    return status;
}
