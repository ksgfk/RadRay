#pragma once

#include <radray/runtime/asset_bundle.h>
#include <radray/shader/shader_compiler_contract.h>

namespace radray {

/// XML/Binary source 共享的最小 Image 描述。payload 仍由 locator 指向的文件提供。
class ImageAssetDescriptor final : public AssetDescriptor {
public:
    explicit ImageAssetDescriptor(bool convertToRgba8 = true) noexcept
        : ConvertToRgba8(convertToRgba8) {}

    RuntimeTypeId GetTypeId() const noexcept override;
    unique_ptr<const AssetDescriptor> Clone() const override;

    bool ConvertToRgba8{true};
};

/// GPU Texture 描述只保留采样颜色空间这一项；上传服务由 AssetManager 装配。
class TextureAssetDescriptor final : public AssetDescriptor {
public:
    explicit TextureAssetDescriptor(bool srgb = false) noexcept : Srgb(srgb) {}

    RuntimeTypeId GetTypeId() const noexcept override;
    unique_ptr<const AssetDescriptor> Clone() const override;

    bool Srgb{false};
};

/// StaticMesh 的第一版描述只包含主 payload locator；网格格式由 loader 解释。
class StaticMeshAssetDescriptor final : public AssetDescriptor {
public:
    RuntimeTypeId GetTypeId() const noexcept override;
    unique_ptr<const AssetDescriptor> Clone() const override;
};

enum class ShaderAssetRepresentation : uint8_t {
    JitSource,
    AotArtifact,
};

/// ShaderAsset 最小 descriptor：一个 entry 只描述一个 source unit 或一个 AOT artifact。
class ShaderAssetDescriptor final : public AssetDescriptor {
public:
    ShaderAssetDescriptor(
        ShaderAssetRepresentation representation,
        shader::ShaderTarget target) noexcept
        : Representation(representation), Target(target) {}

    RuntimeTypeId GetTypeId() const noexcept override;
    unique_ptr<const AssetDescriptor> Clone() const override;

    ShaderAssetRepresentation Representation{ShaderAssetRepresentation::JitSource};
    shader::ShaderTarget Target{shader::ShaderTarget::DXIL};
};

}  // namespace radray
