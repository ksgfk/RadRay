#pragma once

#include <array>

#include <radray/render/common.h>
#include <radray/render/sampler.h>

namespace radray::render {

class DxilReflection {
public:
    class Variable {
    public:
        string Name;
        uint32_t Start;
        uint32_t Size;
    };

    class CBuffer {
    public:
        string Name;
        vector<Variable> Vars;
        uint32_t Size;
    };

    class BindResource {
    public:
        string Name;
        ShaderResourceType Type;
        TextureViewDimension Dim;
        uint32_t Space;
        uint32_t BindPoint;
        uint32_t BindCount;
    };

    class StaticSampler : public SamplerDescriptor {
    public:
        string Name;
    };

    class VertexInput {
    public:
        string Semantic;
        uint32_t SemanticIndex;
        VertexFormat Format;
    };

public:
    vector<CBuffer> CBuffers;
    vector<BindResource> Binds;
    vector<VertexInput> VertexInputs;
    vector<StaticSampler> StaticSamplers;
    std::array<uint32_t, 3> GroupSize;
};

bool operator==(const DxilReflection::Variable& lhs, const DxilReflection::Variable& rhs) noexcept;
bool operator!=(const DxilReflection::Variable& lhs, const DxilReflection::Variable& rhs) noexcept;
bool operator==(const DxilReflection::CBuffer& lhs, const DxilReflection::CBuffer& rhs) noexcept;
bool operator!=(const DxilReflection::CBuffer& lhs, const DxilReflection::CBuffer& rhs) noexcept;
bool operator==(const DxilReflection::StaticSampler& lhs, const DxilReflection::StaticSampler& rhs) noexcept;
bool operator!=(const DxilReflection::StaticSampler& lhs, const DxilReflection::StaticSampler& rhs) noexcept;

}  // namespace radray::render

#ifdef RADRAY_ENABLE_DXC

#include <span>

namespace radray::render {

enum class HlslShaderModel {
    SM60,
    SM61,
    SM62,
    SM63,
    SM64,
    SM65,
    SM66
};

class DxcOutput {
public:
    vector<byte> Data;
    vector<byte> Refl;
    ShaderBlobCategory Category;
};

class Dxc : public RenderBase, public enable_shared_from_this<Dxc> {
public:
    class Impl {
    public:
        virtual ~Impl() noexcept = default;
    };

    explicit Dxc(unique_ptr<Impl> impl) noexcept : _impl(std::move(impl)) {}
    ~Dxc() noexcept override = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::UNKNOWN; }
    bool IsValid() const noexcept override { return _impl != nullptr; }
    void Destroy() noexcept override;

    std::optional<DxcOutput> Compile(std::string_view code, std::span<std::string_view> args) noexcept;

    std::optional<DxcOutput> Compile(
        std::string_view code,
        std::string_view entryPoint,
        ShaderStage stage,
        HlslShaderModel sm,
        bool isOptimize,
        std::span<std::string_view> defines = {},
        std::span<std::string_view> includes = {},
        bool isSpirv = false) noexcept;

    std::optional<DxilReflection> GetDxilReflection(ShaderStage stage, std::span<const byte> refl) noexcept;

private:
    unique_ptr<Impl> _impl;
};

Nullable<shared_ptr<Dxc>> CreateDxc() noexcept;

}  // namespace radray::render

#endif
