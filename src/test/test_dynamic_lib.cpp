#include <radray/platform.h>
#include <radray/logger.h>

#ifdef RADRAY_PLATFORM_WINDOWS
#include <windows.h>
#endif
#ifdef RADRAY_PLATFORM_MACOS
#define __EMULATE_UUID 1
#endif
#define DXC_API_IMPORT
#include <dxcapi.h>

int main() {
    radray::DynamicLibrary dxc{"dxcompiler"};
    if (!dxc.IsValid()) {
        RADRAY_ERR_LOG("cannot load dynamic lib dxcompiler");
        return -1;
    }
    auto pFunc = dxc.GetFunction<decltype(DxcCreateInstance)>("DxcCreateInstance");
    if (pFunc == nullptr) {
        RADRAY_ERR_LOG("cannot load function DxcCreateInstance");
        return -1;
    }
    IDxcCompiler3* pDxc;
    pFunc(CLSID_DxcCompiler, IID_PPV_ARGS(&pDxc));
    if (pDxc != nullptr) {
        pDxc->Release();
    }
    return 0;
}
