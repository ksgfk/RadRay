#include <cstdint>
#include <utility>

#include <gtest/gtest.h>

#include <radray/render/common.h>

namespace radray::render {
namespace {

class TestShaderBindingLayout final : public ShaderBindingLayout {
public:
    explicit TestShaderBindingLayout(vector<ShaderParameterInfo> parameters) noexcept
        : _shaderParameters(std::move(parameters)) {}

    bool IsValid() const noexcept override { return _valid; }
    void Destroy() noexcept override { _valid = false; }
    void SetDebugName(std::string_view name) noexcept override { _name = string{name}; }

    vector<ShaderParameterInfo> GetParameters() const noexcept override { return _shaderParameters; }

    std::optional<ShaderParameterId> FindParameterId(std::string_view name) const noexcept override {
        for (const auto& parameter : _shaderParameters) {
            if (parameter.Name == name) {
                return parameter.Id;
            }
        }
        return std::nullopt;
    }

    Nullable<const ShaderParameterInfo*> FindParameter(ShaderParameterId id) const noexcept override {
        for (const auto& parameter : _shaderParameters) {
            if (parameter.Id == id) {
                return &parameter;
            }
        }
        return nullptr;
    }

private:
    vector<ShaderParameterInfo> _shaderParameters{};
    bool _valid{true};
    string _name{};
};

class FakeShaderParameterTable final : public ShaderParameterTable {
public:
    using ShaderParameterTable::SetBindlessArray;
    using ShaderParameterTable::SetBytes;
    using ShaderParameterTable::SetResource;
    using ShaderParameterTable::SetSampler;

    explicit FakeShaderParameterTable(ShaderBindingLayout* layout) noexcept
        : _layout(layout) {}

    bool IsValid() const noexcept override { return _valid; }

    void Destroy() noexcept override { _valid = false; }

    void Reset() noexcept override {
        LastResourceId.reset();
        LastResourceView = nullptr;
        LastResourceKind = LastWrittenResourceKind::None;
        LastBufferBinding.reset();
        LastResourceArrayIndex = 0;
        LastSamplerId.reset();
        LastSampler = nullptr;
        LastSamplerArrayIndex = 0;
        LastBytesId.reset();
        LastBytes.clear();
        LastBindlessId.reset();
        LastBindlessArray = nullptr;
    }

    void SetDebugName(std::string_view name) noexcept override { _name = string{name}; }

    ShaderBindingLayout* GetShaderBindingLayout() const noexcept override { return _layout; }

    bool SetResource(ShaderParameterId id, ResourceView* view, uint32_t arrayIndex = 0) noexcept override {
        LastResourceId = id;
        LastResourceView = view;
        LastBufferBinding.reset();
        LastResourceKind = LastWrittenResourceKind::View;
        LastResourceArrayIndex = arrayIndex;
        return true;
    }

    bool SetResource(ShaderParameterId id, const BufferBindingDescriptor& desc, uint32_t arrayIndex = 0) noexcept override {
        LastResourceId = id;
        LastResourceView = nullptr;
        LastBufferBinding = desc;
        LastResourceKind = LastWrittenResourceKind::Buffer;
        LastResourceArrayIndex = arrayIndex;
        return true;
    }

    bool SetSampler(ShaderParameterId id, Sampler* sampler, uint32_t arrayIndex = 0) noexcept override {
        LastSamplerId = id;
        LastSampler = sampler;
        LastSamplerArrayIndex = arrayIndex;
        return true;
    }

    bool SetBytes(ShaderParameterId id, const void* data, uint32_t size) noexcept override {
        LastBytesId = id;
        LastBytes.assign(static_cast<const byte*>(data), static_cast<const byte*>(data) + size);
        return true;
    }

    bool SetBindlessArray(ShaderParameterId id, BindlessArray* array) noexcept override {
        LastBindlessId = id;
        LastBindlessArray = array;
        return true;
    }

    enum class LastWrittenResourceKind : uint8_t {
        None,
        View,
        Buffer
    };

    std::optional<ShaderParameterId> LastResourceId{};
    ResourceView* LastResourceView{nullptr};
    LastWrittenResourceKind LastResourceKind{LastWrittenResourceKind::None};
    std::optional<BufferBindingDescriptor> LastBufferBinding{};
    uint32_t LastResourceArrayIndex{0};

    std::optional<ShaderParameterId> LastSamplerId{};
    Sampler* LastSampler{nullptr};
    uint32_t LastSamplerArrayIndex{0};

    std::optional<ShaderParameterId> LastBytesId{};
    vector<byte> LastBytes{};

    std::optional<ShaderParameterId> LastBindlessId{};
    BindlessArray* LastBindlessArray{nullptr};

private:
    bool _valid{true};
    ShaderBindingLayout* _layout{nullptr};
    string _name{};
};

class DummyTextureView final : public TextureView {
public:
    bool IsValid() const noexcept override { return _valid; }
    void Destroy() noexcept override { _valid = false; }
    void SetDebugName(std::string_view name) noexcept override { _name = string{name}; }

private:
    bool _valid{true};
    string _name{};
};

class DummySampler final : public Sampler {
public:
    bool IsValid() const noexcept override { return _valid; }
    void Destroy() noexcept override { _valid = false; }
    void SetDebugName(std::string_view name) noexcept override { _name = string{name}; }

private:
    bool _valid{true};
    string _name{};
};

class DummyBindlessArray final : public BindlessArray {
public:
    bool IsValid() const noexcept override { return _valid; }
    void Destroy() noexcept override { _valid = false; }
    void SetDebugName(std::string_view name) noexcept override { _name = string{name}; }
    void SetBuffer(uint32_t, const BufferBindingDescriptor&) noexcept override {}
    void SetTexture(uint32_t, TextureView*, Sampler*) noexcept override {}

private:
    bool _valid{true};
    string _name{};
};

TestShaderBindingLayout MakeShaderBindingLayoutForHelpers() {
    vector<ShaderParameterInfo> parameters{};
    parameters.push_back(ShaderParameterInfo{
        .Name = "Tex",
        .Id = ShaderParameterId{0},
        .Kind = ShaderParameterKind::Resource,
        .Stages = ShaderStage::Pixel,
        .Type = ResourceBindType::Texture,
        .Count = 1,
        .IsReadOnly = true,
    });
    parameters.push_back(ShaderParameterInfo{
        .Name = "Linear",
        .Id = ShaderParameterId{1},
        .Kind = ShaderParameterKind::Sampler,
        .Stages = ShaderStage::Pixel,
        .Type = ResourceBindType::Sampler,
        .Count = 1,
        .IsReadOnly = true,
    });
    parameters.push_back(ShaderParameterInfo{
        .Name = "Pc",
        .Id = ShaderParameterId{2},
        .Kind = ShaderParameterKind::Constant,
        .Stages = ShaderStage::Pixel,
        .ByteSize = sizeof(uint32_t),
    });
    parameters.push_back(ShaderParameterInfo{
        .Name = "Bindless",
        .Id = ShaderParameterId{3},
        .Kind = ShaderParameterKind::BindlessArray,
        .Stages = ShaderStage::Pixel,
        .Type = ResourceBindType::Buffer,
        .Count = 0,
        .IsReadOnly = true,
        .IsBindless = true,
    });
    return TestShaderBindingLayout{std::move(parameters)};
}

}  // namespace

TEST(ShaderParameterTableTest, NameHelpersResolveThroughShaderBindingLayout) {
    TestShaderBindingLayout layout = MakeShaderBindingLayoutForHelpers();
    FakeShaderParameterTable table{&layout};
    DummyTextureView texView{};
    DummySampler sampler{};
    DummyBindlessArray bindless{};

    EXPECT_TRUE(table.SetResource("Tex", &texView, 2));
    ASSERT_TRUE(table.LastResourceId.has_value());
    EXPECT_EQ(table.LastResourceId.value(), ShaderParameterId{0});
    EXPECT_EQ(table.LastResourceKind, FakeShaderParameterTable::LastWrittenResourceKind::View);
    EXPECT_EQ(table.LastResourceView, &texView);
    EXPECT_EQ(table.LastResourceArrayIndex, 2u);

    EXPECT_TRUE(table.SetSampler("Linear", &sampler, 1));
    ASSERT_TRUE(table.LastSamplerId.has_value());
    EXPECT_EQ(table.LastSamplerId.value(), ShaderParameterId{1});
    EXPECT_EQ(table.LastSampler, &sampler);
    EXPECT_EQ(table.LastSamplerArrayIndex, 1u);

    uint32_t pc = 42;
    EXPECT_TRUE(table.SetBytes("Pc", &pc, sizeof(pc)));
    ASSERT_TRUE(table.LastBytesId.has_value());
    EXPECT_EQ(table.LastBytesId.value(), ShaderParameterId{2});
    EXPECT_EQ(table.LastBytes.size(), sizeof(pc));

    EXPECT_TRUE(table.SetBindlessArray("Bindless", &bindless));
    ASSERT_TRUE(table.LastBindlessId.has_value());
    EXPECT_EQ(table.LastBindlessId.value(), ShaderParameterId{3});
    EXPECT_EQ(table.LastBindlessArray, &bindless);

    EXPECT_FALSE(table.SetResource("Missing", &texView));
    BufferBindingDescriptor bufferDesc{};
    bufferDesc.Target = reinterpret_cast<Buffer*>(0x1);
    bufferDesc.Range = BufferRange{16, 32};
    bufferDesc.Stride = 16;
    bufferDesc.Usage = BufferViewUsage::ReadOnlyStorage;
    EXPECT_TRUE(table.SetResource(ShaderParameterId{0}, bufferDesc, 3));
    ASSERT_TRUE(table.LastBufferBinding.has_value());
    EXPECT_EQ(table.LastResourceKind, FakeShaderParameterTable::LastWrittenResourceKind::Buffer);
    EXPECT_EQ(table.LastBufferBinding->Target, bufferDesc.Target);
    EXPECT_EQ(table.LastBufferBinding->Range.Offset, bufferDesc.Range.Offset);
    EXPECT_EQ(table.LastBufferBinding->Range.Size, bufferDesc.Range.Size);
    EXPECT_EQ(table.LastBufferBinding->Stride, bufferDesc.Stride);
    EXPECT_EQ(table.LastBufferBinding->Usage, bufferDesc.Usage);
    EXPECT_FALSE(table.SetSampler("Missing", &sampler));

    table.Reset();
    EXPECT_FALSE(table.LastResourceId.has_value());
    EXPECT_EQ(table.LastResourceKind, FakeShaderParameterTable::LastWrittenResourceKind::None);
    EXPECT_FALSE(table.LastSamplerId.has_value());
    EXPECT_FALSE(table.LastBytesId.has_value());
    EXPECT_FALSE(table.LastBindlessId.has_value());
}

TEST(ShaderBindingLayoutTest, ExposesPublicShaderAbiOnly) {
    TestShaderBindingLayout layout = MakeShaderBindingLayoutForHelpers();

    ASSERT_EQ(layout.GetParameters().size(), 4u);
    auto texId = layout.FindParameterId("Tex");
    ASSERT_TRUE(texId.has_value());
    EXPECT_EQ(texId.value(), ShaderParameterId{0});

    auto pc = layout.FindParameter(ShaderParameterId{2});
    ASSERT_TRUE(pc.HasValue());
    EXPECT_EQ(pc.Get()->Name, "Pc");
    EXPECT_EQ(pc.Get()->Kind, ShaderParameterKind::Constant);
    EXPECT_EQ(pc.Get()->ByteSize, sizeof(uint32_t));

    auto bindless = layout.FindParameter(ShaderParameterId{3});
    ASSERT_TRUE(bindless.HasValue());
    EXPECT_EQ(bindless.Get()->Kind, ShaderParameterKind::BindlessArray);
    EXPECT_TRUE(bindless.Get()->IsBindless);
    EXPECT_FALSE(layout.FindParameterId("Missing").has_value());
    EXPECT_FALSE(layout.FindParameter(ShaderParameterId{99}).HasValue());
}

}  // namespace radray::render
