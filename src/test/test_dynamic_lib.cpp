#include <radray/platform.h>

#include <windows.h>
#include <dxcapi.h>

int main() {
    radray::DynamicLibrary dxc{"dxcompiler"};
    auto c = (decltype(DxcCreateInstance)*)dxc.GetSymbol("DxcCreateInstance");
    IDxcUtils* pUtils;
    c(CLSID_DxcUtils, IID_PPV_ARGS(&pUtils));
    if (pUtils != nullptr) {
        pUtils->Release();
    }
    return 0;
}
