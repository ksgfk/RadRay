#include <radray/utility.h>

int main() {
    {
        radray::string a{"cajsijsaoi  aiosf jais fja pfjap s"};
        auto opt = radray::ToWideChar(a);
        radray::wstring wa = opt.value();
        radray::wstring ta{L"cajsijsaoi  aiosf jais fja pfjap s"};
        if (wa != ta) {
            throw std::exception();
        }
    }
    {
        radray::wstring a{L"abcdef"};
        auto opt = radray::ToMultiByte(a);
        radray::string wa = opt.value();
        radray::string ta{"abcdef"};
        if (wa != ta) {
            throw std::exception();
        }
    }
    return 0;
}
