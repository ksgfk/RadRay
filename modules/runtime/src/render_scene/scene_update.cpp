#include <radray/runtime/render_scene/scene_update.h>

namespace radray {

void SceneUpdateBatch::Clear() noexcept {
    RemovePrimitives.clear();
    CreatePrimitives.clear();
    MeshStates.clear();
    Transforms.clear();
}

}  // namespace radray
