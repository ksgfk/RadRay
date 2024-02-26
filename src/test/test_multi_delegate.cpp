#include <radray/logger.h>
#include <radray/multi_delegate.h>

using namespace radray;

int g = 114514;

void Test1(int i) {
    RADRAY_LOG_DEBUG("b {}", i);
}

int main() {
    auto md = std::make_shared<MultiDelegate<void(int)>>();
    DelegateHandle<void(int)> a{
        [&](int i) {
            RADRAY_LOG_DEBUG("a {} {}", i, g);
        },
        md};
    DelegateHandle<void(int)> b = std::move(a);
    b = {};
    {
        DelegateHandle<void(void)> e;
    }
    md->Invoke(114514);
    return 0;
}
