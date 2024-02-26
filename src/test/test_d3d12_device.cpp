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
    DelegateHandle<window::CursorPositionCallback> t{
        [](const Eigen::Vector2f& i) {
            RADRAY_LOG_DEBUG("pos {} {}", i.x(), i.y());
        },
        window.EventCursorPosition()};
    DelegateHandle<window::CursorPositionCallback> v{AAA, window.EventCursorPosition()};
    auto e = BBB;
    DelegateHandle<window::CursorPositionCallback> u{e, window.EventCursorPosition()};
    d3d12::Device d{};
    while (!window.ShouldClose()) {
        window::GlobalPollEvents();
    }
    window.Destroy();
    window::GlobalTerminate();
    return 0;
}
