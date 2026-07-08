#include <gtest/gtest.h>

#include <cstring>

#include <radray/runtime/material_asset.h>
#include <radray/runtime/render_framework/material_property_block.h>
#include <radray/runtime/render_framework/material_render_snapshot.h>

using namespace radray;

namespace {

// 从快照的 Constants 里按名字取一个 float (用于校验合并结果)。
std::optional<float> GetConstantFloat(const MaterialRenderSnapshot& snap, std::string_view name) {
    for (const auto& c : snap.Constants) {
        if (c.Name == name && c.Bytes.size() == sizeof(float)) {
            float v;
            std::memcpy(&v, c.Bytes.data(), sizeof(float));
            return v;
        }
    }
    return std::nullopt;
}

// 统计快照 Constants 里某名字出现的次数 (校验无重复 entry)。
uint32_t CountConstant(const MaterialRenderSnapshot& snap, std::string_view name) {
    uint32_t n = 0;
    for (const auto& c : snap.Constants) {
        if (c.Name == name) {
            ++n;
        }
    }
    return n;
}

}  // namespace

TEST(MaterialPropertyBlockTest, EmptyByDefault) {
    MaterialPropertyBlock block;
    EXPECT_TRUE(block.IsEmpty());
    EXPECT_EQ(block.GetVersion(), 0u);
}

TEST(MaterialPropertyBlockTest, VersionIncrementsOnWrite) {
    MaterialPropertyBlock block;
    const uint64_t v0 = block.GetVersion();
    block.SetFloat("_A", 1.0f);
    const uint64_t v1 = block.GetVersion();
    EXPECT_GT(v1, v0);
    block.SetVector("_B", Eigen::Vector4f{1, 2, 3, 4});
    EXPECT_GT(block.GetVersion(), v1);
}

TEST(MaterialPropertyBlockTest, ClearPropertyDecrementsAndBumpsVersion) {
    MaterialPropertyBlock block;
    block.SetFloat("_A", 1.0f);
    const uint64_t v1 = block.GetVersion();
    block.ClearProperty("_A");
    EXPECT_TRUE(block.IsEmpty());
    EXPECT_GT(block.GetVersion(), v1);
    // 清一个不存在的 property 不改版本。
    const uint64_t v2 = block.GetVersion();
    block.ClearProperty("_Missing");
    EXPECT_EQ(block.GetVersion(), v2);
}

TEST(MaterialPropertyBlockTest, GetOverride) {
    MaterialPropertyBlock block;
    block.SetFloat("_A", 2.5f);
    auto ov = block.GetOverride("_A");
    ASSERT_TRUE(ov.has_value());
    ASSERT_TRUE(std::holds_alternative<float>(ov.value()));
    EXPECT_FLOAT_EQ(std::get<float>(ov.value()), 2.5f);
    EXPECT_FALSE(block.GetOverride("_Missing").has_value());
}

// ─── 快照合并 ───

TEST(MaterialSnapshotMergeTest, NullBlockEqualsTemplate) {
    MaterialAsset mat;
    mat.SetFloat("_Metallic", 0.5f);
    auto snap = mat.CreateSnapshot(nullptr);
    ASSERT_NE(snap, nullptr);
    auto v = GetConstantFloat(*snap, "_Metallic");
    ASSERT_TRUE(v.has_value());
    EXPECT_FLOAT_EQ(v.value(), 0.5f);
}

TEST(MaterialSnapshotMergeTest, EmptyBlockEqualsTemplate) {
    MaterialAsset mat;
    mat.SetFloat("_Metallic", 0.5f);
    MaterialPropertyBlock block;  // 空
    auto snap = mat.CreateSnapshot(&block);
    ASSERT_NE(snap, nullptr);
    EXPECT_FLOAT_EQ(GetConstantFloat(*snap, "_Metallic").value(), 0.5f);
}

TEST(MaterialSnapshotMergeTest, OverrideReplacesTemplateValue) {
    MaterialAsset mat;
    mat.SetFloat("_Metallic", 0.5f);
    MaterialPropertyBlock block;
    block.SetFloat("_Metallic", 0.9f);
    auto snap = mat.CreateSnapshot(&block);
    ASSERT_NE(snap, nullptr);
    // 覆盖生效, 且不产生重复 entry。
    EXPECT_EQ(CountConstant(*snap, "_Metallic"), 1u);
    EXPECT_FLOAT_EQ(GetConstantFloat(*snap, "_Metallic").value(), 0.9f);
}

TEST(MaterialSnapshotMergeTest, OverrideAddsNewProperty) {
    MaterialAsset mat;
    mat.SetFloat("_Metallic", 0.5f);
    MaterialPropertyBlock block;
    block.SetFloat("_Roughness", 0.2f);
    auto snap = mat.CreateSnapshot(&block);
    ASSERT_NE(snap, nullptr);
    // 模板值保留, 覆盖新增值也在。
    EXPECT_FLOAT_EQ(GetConstantFloat(*snap, "_Metallic").value(), 0.5f);
    EXPECT_FLOAT_EQ(GetConstantFloat(*snap, "_Roughness").value(), 0.2f);
}

TEST(MaterialSnapshotMergeTest, TemplateUnmodifiedByOverride) {
    MaterialAsset mat;
    mat.SetFloat("_Metallic", 0.5f);
    MaterialPropertyBlock block;
    block.SetFloat("_Metallic", 0.9f);
    (void)mat.CreateSnapshot(&block);
    // 共享材质模板不被覆盖污染。
    EXPECT_FLOAT_EQ(mat.GetFloat("_Metallic").value(), 0.5f);
}

TEST(MaterialSnapshotMergeTest, OverrideCanChangePropertyType) {
    // 模板里 _P 是 float; 覆盖成 vector。合并后应只有 vector (常量 16 字节), 无重复。
    MaterialAsset mat;
    mat.SetFloat("_P", 1.0f);
    MaterialPropertyBlock block;
    block.SetVector("_P", Eigen::Vector4f{1, 2, 3, 4});
    auto snap = mat.CreateSnapshot(&block);
    ASSERT_NE(snap, nullptr);
    EXPECT_EQ(CountConstant(*snap, "_P"), 1u);
    // 16 字节 (vector) 而非 4 字节 (float)。
    for (const auto& c : snap->Constants) {
        if (c.Name == "_P") {
            EXPECT_EQ(c.Bytes.size(), sizeof(float) * 4);
        }
    }
}

TEST(MaterialSnapshotMergeTest, KeywordAndRenderQueueFromTemplate) {
    MaterialAsset mat;
    mat.EnableKeyword("_TINT");
    mat.SetRenderQueue(RenderQueue::Transparent);
    MaterialPropertyBlock block;
    block.SetFloat("_X", 1.0f);
    auto snap = mat.CreateSnapshot(&block);
    ASSERT_NE(snap, nullptr);
    // PropertyBlock 不改变体 / 队列: 仍取模板值。
    ASSERT_EQ(snap->EnabledKeywords.size(), 1u);
    EXPECT_EQ(snap->EnabledKeywords[0], "_TINT");
    EXPECT_EQ(snap->RenderQueue, static_cast<int32_t>(RenderQueue::Transparent));
}
