#include <radray/render/dear_imgui.h>

using namespace radray;
using namespace radray::render;

int main() {
    GlobalInitDearImGui();
    GlobalTerminateDearImGui();
    return 0;
}
