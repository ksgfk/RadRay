#include <radray/d3d12/device.h>
#include <radray/window/native_window.h>

using namespace radray;

void AAA(const Eigen::Vector2f& i) {
    RADRAY_LOG_DEBUG("aaa {} {}", i.x(), i.y());
}
void BBB(const Eigen::Vector2f& i) {
    RADRAY_LOG_DEBUG("bbb {} {}", i.x(), i.y());
}

int main() {
    window::GlobalInit();
    window::NativeWindow window{"test d3d12", 1280, 720};
    auto t = MakeDelegate([](const Eigen::Vector2f& i) {
        RADRAY_LOG_DEBUG("pos {} {}", i.x(), i.y());
    });
    window.AddCursorPositionCallback(t);
    auto v = MakeDelegate(AAA);
    window.AddCursorPositionCallback(v);
    auto e = BBB;
    auto u = MakeDelegate(e);
    window.AddCursorPositionCallback(u);
    d3d12::Device d{};
    while (!window.ShouldClose()) {
        window::GlobalPollEvents();
    }
    window.Destroy();
    window::GlobalTerminate();
    return 0;
}
