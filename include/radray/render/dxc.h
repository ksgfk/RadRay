#pragma once

#include <array>

#include <radray/render/common.h>
#include <radray/render/sampler.h>

namespace radray::render {

class DxilReflection {
public:
    class Variable {
    public:
        radray::string Name;
        uint32_t Start;
        uint32_t Size;
    };

    class CBuffer {
    public:
        radray::string Name;
        radray::vector<Variable> Vars;
        uint32_t Size;
    };

    class BindResource {
    public:
        radray::string Name;
        ShaderResourceType Type;
        TextureDimension Dim;
        uint32_t Space;
        uint32_t BindPoint;
        uint32_t BindCount;
    };

    class StaticSampler : public SamplerDescriptor {
    public:
        radray::string Name;
    };

    class VertexInput {
    public:
        VertexSemantic Semantic;
        uint32_t SemanticIndex;
        VertexFormat Format;
    };

public:
    radray::vector<CBuffer> CBuffers;
    radray::vector<BindResource> Binds;
    radray::vector<VertexInput> VertexInputs;
    radray::vector<StaticSampler> StaticSamplers;
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
    radray::vector<byte> data;
    radray::vector<byte> refl;
    ShaderBlobCategory category;
};

class Dxc : public RenderBase, public radray::enable_shared_from_this<Dxc> {
public:
    class Impl {
    public:
        virtual ~Impl() noexcept = default;
    };

    explicit Dxc(radray::unique_ptr<Impl> impl) noexcept : _impl(std::move(impl)) {}
    ~Dxc() noexcept override = default;

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
    radray::unique_ptr<Impl> _impl;
};

Nullable<radray::shared_ptr<Dxc>> CreateDxc() noexcept;

}  // namespace radray::render

#endif
