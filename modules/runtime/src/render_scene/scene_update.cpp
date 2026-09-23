#include <radray/runtime/render_scene/scene_update.h>

namespace radray {

void SceneUpdateBatch::Clear() noexcept {
    RemoveTransforms.clear();
    CreateTransforms.clear();
    TransformParents.clear();
    LocalTransforms.clear();
    RemoveShapes.clear();
    CreateShapes.clear();
    MeshStates.clear();
    Transforms.clear();
    LightsChanged = false;
    Lights.Clear();
}

}  // namespace radray
