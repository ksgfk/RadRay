#include "test_scene_apply_changes.h"

namespace radray::test {

TEST_F(SceneApplyChanges, FinalForestChangesSharedTransformsAndReparent) {
    auto& scene = Scene;
    auto& changes = Changes;
    auto& batch = Batch;
    const TransformId parent{0, 0}, child{1, 0}, other{2, 0};
    const ShapeId a{0, 0}, b{1, 0}, c{2, 0}, bare{3, 0};
    batch.CreateTransforms = {{parent, {}, {}}, {child, parent, {}}, {other, {}, {}}};
    batch.CreateShapes = {a, b, c, bare};
    batch.MeshStates = {{.Id = a, .Transform = parent}, {.Id = b, .Transform = child}, {.Id = c, .Transform = child}};
    scene.Apply(batch, &changes);
    EXPECT_EQ(changes.Updated, (vector<ShapeId>{a, b, c}));
    EXPECT_TRUE(changes.Removed.empty());
    batch.Clear();
    LocalTransform moved;
    moved.Translation[0] = 4;
    batch.LocalTransforms = {{parent, moved}, {child, moved}};
    scene.Apply(batch, &changes);
    EXPECT_EQ(changes.Updated, (vector<ShapeId>{a, b, c}));
    EXPECT_FLOAT_EQ(scene.GetStaticMesh(b)->LocalToWorld(0, 3), 8);
    batch.LocalTransforms = {{parent, {}}};
    scene.Apply(batch, &changes);
    EXPECT_EQ(changes.Updated, (vector<ShapeId>{a, b, c}));
    batch.Clear();
    batch.TransformParents = {{child, other}};
    scene.Apply(batch, &changes);
    EXPECT_EQ(changes.Updated, (vector<ShapeId>{b, c}));
    batch.Clear();
    batch.MeshStates = {{.Id = b, .Transform = child}};
    scene.Apply(batch, &changes);
    EXPECT_EQ(changes.Updated, (vector<ShapeId>{b}));
    batch.Clear();
    batch.RemoveShapes = {a};
    scene.Apply(batch, &changes);
    EXPECT_TRUE(changes.Updated.empty());
    EXPECT_EQ(changes.Removed, (vector<ShapeId>{a}));
    EXPECT_EQ(scene.GetStaticMeshes()[0], c);
    batch.Clear();
    scene.Apply(batch, &changes);
    EXPECT_TRUE(changes.Updated.empty());
    EXPECT_TRUE(changes.Removed.empty());
}

TEST_F(SceneApplyChanges, BareExplicitWorldAndGenerationReuse) {
    auto& scene = Scene;
    auto& changes = Changes;
    auto& batch = Batch;
    const ShapeId old{9, 0}, replacement{9, 1};
    batch.CreateShapes = {old};
    scene.Apply(batch, &changes);
    EXPECT_TRUE(changes.Updated.empty());
    batch.Clear();
    batch.MeshStates = {{.Id = old}};
    scene.Apply(batch, &changes);
    EXPECT_EQ(changes.Updated, (vector<ShapeId>{old}));
    batch.Clear();
    Eigen::Matrix4f matrix = Eigen::Matrix4f::Identity();
    matrix(2, 3) = 17;
    batch.Transforms = {{old, matrix}};
    scene.Apply(batch, &changes);
    EXPECT_EQ(changes.Updated, (vector<ShapeId>{old}));
    EXPECT_FLOAT_EQ(scene.GetStaticMesh(old)->LocalToWorld(2, 3), 17);
    batch.Clear();
    batch.RemoveShapes = {old};
    batch.CreateShapes = {replacement};
    batch.MeshStates = {{.Id = replacement}};
    scene.Apply(batch, &changes);
    EXPECT_EQ(changes.Removed, (vector<ShapeId>{old}));
    EXPECT_EQ(changes.Updated, (vector<ShapeId>{replacement}));
    EXPECT_FALSE(scene.GetStaticMesh(old));
}

}  // namespace radray::test
