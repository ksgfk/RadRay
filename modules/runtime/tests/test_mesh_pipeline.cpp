#include <radray/runtime/scene.h>
#include <array>
#include <cstring>
#include <iostream>
#include <gtest/gtest.h>

namespace radray::test {
void Check(bool value, const char* reason) { EXPECT_TRUE(value) << reason; }
struct Instance {
    int origin[3];
    float visibility;
};
struct Processor final : MeshPassProcessor {
    uint64_t revision{1};
    uint64_t Version() const override { return revision; }
    bool Validate(const MeshBatch&, const MaterialDescriptor&) const override { return true; }
};
struct Fixture {
    Processor processor;
    ViewSubmission view;

    Scene scene;
    array<uint32_t, 6> indices{0, 1, 2, 2, 3, 0};
    array<byte, 112> vertices{};
    IndexUpdate index{1, indices.data(), 6, 0};
    DrawRange range{0, 6, 0, 0};
    MeshUpdate mesh{1, vertices.data(), 1, &range, 4, 28, 7, 1};
    MaterialDescriptor material{1, 42, {0, 1}, {byte{1}}};
    PrimitiveUpdate primitive{1, 1, 1};
    Instance instance{{0, 0, 0}, 1};
    VisibleItem visible{1, 0, 0, 0, 1};
    SceneUpdate frame{};
    Fixture() {
        frame.epoch = 1;
        frame.indices = &index;
        frame.index_count = 1;
        frame.meshes = &mesh;
        frame.mesh_count = 1;
        frame.materials = &material;
        frame.material_count = 1;
        frame.primitives = &primitive;
        frame.primitive_count = 1;
        view = {{&visible, 1}, std::as_bytes(std::span{&instance, 1}), sizeof(instance)};
    }
    Scene::Update Prepare(uint64_t textureVersion = 1) {
        Scene::Update update;
        EXPECT_TRUE(scene.Prepare(frame, view, processor, textureVersion, update));
        return update;
    }
    void NoChanges() { frame.index_count = frame.mesh_count = frame.material_count = frame.primitive_count = 0; }
};
void CacheAndTransactions() {
    Fixture f;
    auto first = f.Prepare();
    auto cached = first.visible[0].cached;
    Check(first.builds == 1, "initial cache build");
    f.scene.Commit(std::move(first));
    f.NoChanges();
    for (int i = 0; i < 4; i++) {
        f.instance.origin[0] += 100;
        f.instance.visibility = .5f;
        f.visible.depth_key++;
        auto next = f.Prepare();
        Check(next.builds == 0 && next.visible[0].cached == cached, "view/instance must reuse stable command");
        f.scene.Commit(std::move(next));
    }
    f.frame.mesh_count = 1;
    auto replacement = f.Prepare();
    Check(replacement.builds == 1, "geometry invalidation");
    // Drop the prepared transaction as a resource preparation failure would.
    f.NoChanges();
    auto unchanged = f.Prepare();
    Check(unchanged.visible[0].cached == cached, "failed preparation published scene");
    f.frame.mesh_count = 1;
    f.scene.Commit(f.Prepare());
    f.NoChanges();
    f.frame.material_count = 1;
    f.material.parameters[0] = byte{4};
    Check(f.Prepare().builds == 1, "material invalidation");
    f.scene.Commit(f.Prepare());
    f.NoChanges();
    f.frame.index_count = 1;
    Check(f.Prepare().builds == 1, "index invalidation");
    f.scene.Commit(f.Prepare());
    f.NoChanges();
    Check(f.Prepare(2).builds == 1, "texture resource invalidation");
    f.scene.Commit(f.Prepare(2));
    Check(f.Prepare(2).builds == 0, "resource binding failed to stabilize");
    f.processor.revision++;
    Check(f.Prepare(2).builds == 1, "processor identity did not invalidate cached commands");
    f.scene.Commit(f.Prepare(2));
    uint64_t removed = 1;
    f.frame.removed_meshes = &removed;
    f.frame.removed_mesh_count = 1;
    Scene::Update invalid;
    Check(!f.scene.Prepare(f.frame, f.view, f.processor, 2, invalid), "dangling primitive accepted");
    f.frame.removed_mesh_count = 0;
    Check(f.Prepare(2).builds == 0, "invalid removal changed cache");
    f.frame.epoch = 2;
    f.view.visible = {};
    auto reset = f.Prepare(2);
    Check(reset.reset, "epoch did not clear scene");
    f.scene.Commit(std::move(reset));
    f.view.visible = {&f.visible, 1};
    invalid = {};
    Check(!f.scene.Prepare(f.frame, f.view, f.processor, 2, invalid), "epoch retained primitive");
    f.frame.epoch = 1;
    invalid = {};
    Check(!f.scene.Prepare(f.frame, f.view, f.processor, 2, invalid), "stale epoch accepted");
}
DrawList DrawListForTest(vector<ResolvedDraw> visible, std::span<const byte> instances, uint32_t stride) {
    DrawList result;
    EXPECT_TRUE(BuildDrawList(std::move(visible), instances, stride, result));
    return result;
}
void CommandLists() {
    MeshDrawCommand a{};
    a.mesh = 1;
    a.material = 1;
    a.program = 42;
    a.range = {0, 6, 0, 0};
    auto b = a;
    b.range.first = 6;
    array<Instance, 2> instances{{{{1, 2, 3}, 1}, {{4, 5, 6}, .5f}}};
    auto contiguous = DrawListForTest({{&b, 1, 1, 10, 0, 0, 1}, {&a, 1, 1, 10, 0, 0, 1}}, std::as_bytes(std::span{instances}), sizeof(Instance));
    Check(contiguous.packets.size() == 1 && contiguous.packets[0].count == 12, "contiguous submeshes not merged");
    auto repeated = DrawListForTest({{&a, 1, 1, 10, 0, 0, 1}, {&a, 1, 1, 10, 0, 1, 1}}, std::as_bytes(std::span{instances}), sizeof(Instance));
    Check(repeated.packets.size() == 1 && repeated.packets[0].instances == 2, "compatible instances not merged");
    auto other = a;
    other.mesh = 2;
    auto separate = DrawListForTest({{&a, 1, 1, 10, 0, 0, 1}, {&other, 1, 1, 10, 0, 1, 1}}, std::as_bytes(std::span{instances}), sizeof(Instance));
    Check(separate.packets.size() == 2, "different geometry was instanced");
    auto offset = DrawListForTest({{&a, 1, 1, 10, 0, 0, 1}, {&a, 1, 1, 20, 0, 1, 1}}, std::as_bytes(std::span{instances}), sizeof(Instance));
    Check(offset.packets.size() == 2, "different ranges were instanced");
    auto binding = a;
    binding.textureVersion++;
    auto incompatible = DrawListForTest({{&a, 1, 1, 10, 0, 0, 1}, {&binding, 1, 1, 10, 0, 1, 1}}, std::as_bytes(std::span{instances}), sizeof(Instance));
    Check(incompatible.packets.size() == 2, "different binding identities were merged");
    binding = a;
    binding.layout++;
    incompatible = DrawListForTest({{&a, 1, 1, 10, 0, 0, 1}, {&binding, 1, 1, 10, 0, 1, 1}}, std::as_bytes(std::span{instances}), sizeof(Instance));
    Check(incompatible.packets.size() == 2, "different vertex layouts were merged");
    for (const auto& p : separate.packets) {
        array<uint32_t, 5> indirect{};
        std::memcpy(indirect.data(), separate.indirect.data() + p.indirectOffset, 20);
        Check(indirect == array<uint32_t, 5>{p.count, p.instances, p.first, uint32_t(p.baseVertex), p.firstInstance}, "indexed direct/indirect differ");
    }
    a.program = 19;
    a.range = {0, 3, 0, 0};
    auto procedural = DrawListForTest({{&a, 0, 0, 0, 0, 0, 1}}, std::as_bytes(std::span{instances}), sizeof(Instance));
    array<uint32_t, 4> args{};
    std::memcpy(args.data(), procedural.indirect.data(), 16);
    Check(args == array<uint32_t, 4>{3, 1, 0, 0}, "nonindexed indirect differs");
}
void ProceduralCache() {
    Fixture f;
    f.frame.index_count = 0;
    f.mesh = {1, nullptr, 0, &f.range, 3, 0, 0, 1};
    f.range.count = 3;
    f.material.program = 19;
    f.material.resources.clear();
    auto initial = f.Prepare();
    auto cached = initial.visible[0].cached;
    f.scene.Commit(std::move(initial));
    f.NoChanges();
    auto changedTextures = f.Prepare(9);
    Check(changedTextures.builds == 0 && changedTextures.visible[0].cached == cached, "UV depends on unrelated textures");
}
TEST(RuntimeScene, CacheAndTransactions) { CacheAndTransactions(); }
TEST(RuntimeScene, StrictDrawPackets) { CommandLists(); }
TEST(RuntimeScene, ProceduralCache) { ProceduralCache(); }

}  // namespace radray::test
