#pragma once

#include <gtest/gtest.h>
#include <radray/runtime/render_scene/render_scene.h>

namespace radray::test {

class SceneApplyChanges : public testing::Test {
protected:
    RenderScene Scene;
    radray::SceneApplyChanges Changes;
    SceneUpdateBatch Batch;
};

}  // namespace radray::test
