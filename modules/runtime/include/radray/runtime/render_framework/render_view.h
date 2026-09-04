#pragma once

#include <radray/runtime/render_framework/render_output.h>

namespace radray {

struct ViewStateId {
    uint64_t Value{0};
    bool IsValid() const noexcept { return Value != 0; }
    friend bool operator==(const ViewStateId&, const ViewStateId&) = default;
};
struct ViewStateIdHash {
    size_t operator()(ViewStateId id) const noexcept { return std::hash<uint64_t>{}(id.Value); }
};
ViewStateId AllocateViewStateId() noexcept;

struct RenderExtent {
    uint32_t Width{0}, Height{0};
    friend bool operator==(const RenderExtent&, const RenderExtent&) = default;
};
struct NormalizedRect {
    float X{0}, Y{0}, Width{1}, Height{1};
};
struct PerspectiveProjectionDesc {
    float VerticalFovRadians{1.04719755f}, NearPlane{0.1f}, FarPlane{1000.0f};
};
struct OrthographicProjectionDesc {
    float VerticalSize{1}, NearPlane{0}, FarPlane{1000};
};
struct ExplicitProjectionDesc {
    Eigen::Matrix4f Matrix{Eigen::Matrix4f::Identity()};
};
using RenderProjectionDesc = std::variant<PerspectiveProjectionDesc, OrthographicProjectionDesc, ExplicitProjectionDesc>;

struct RenderViewDesc {
    string Name;
    ViewStateId StateId;
    Eigen::Matrix4f WorldToView{Eigen::Matrix4f::Identity()};
    Eigen::Vector3f WorldPosition{Eigen::Vector3f::Zero()};
    RenderProjectionDesc Projection;
    NormalizedRect ViewRect{}, ScissorRect{};
    Eigen::Vector2f JitterPixels{Eigen::Vector2f::Zero()};
    uint32_t LayerMask{0xffffffffu};
    float LodBias{1};
    bool CameraCut{false};
};

struct RenderViewFamilyDesc {
    string Name;
    RenderOutputId Output;
    float RenderScale{1};
    vector<RenderViewDesc> Views;
};

struct ResolvedRenderView {
    std::string_view Name;
    ViewStateId StateId;
    Eigen::Matrix4f View, Projection, ViewProjection, PreviousViewProjection;
    Eigen::Vector3f WorldPosition;
    Rect ViewRect{}, ScissorRect{};
    Eigen::Vector2f JitterNdc{Eigen::Vector2f::Zero()};
    uint32_t LayerMask{0xffffffffu};
    float LodBias{1};
    bool PreviousViewValid{false};
    bool CameraCut{false};
};

struct ResolvedRenderViewFamily {
    uint32_t FrameLocalIndex{0};
    std::string_view Name;
    RenderOutputId OutputId;
    RenderExtent OutputSize, RenderSize;
    render::TextureFormat OutputFormat{render::TextureFormat::UNKNOWN};
    uint32_t SampleCount{1};
    vector<ResolvedRenderView> Views;
    bool OutputAvailable{false};
};

bool ValidateRenderView(const RenderViewDesc& view, string& reason);
std::optional<ResolvedRenderViewFamily> ResolveRenderViewFamily(
    const RenderViewFamilyDesc& family, const RenderOutputInfo& output, uint32_t index,
    uint32_t maxDimension, string& reason);

enum class RenderExtentMode : uint8_t { Absolute,
                                        RelativeToFamilyRenderExtent,
                                        RelativeToFamilyOutputExtent };
struct RenderExtentSpec {
    RenderExtentMode Mode{RenderExtentMode::Absolute};
    uint32_t Width{1}, Height{1};
    float ScaleX{1}, ScaleY{1};
    uint32_t AlignX{1}, AlignY{1}, MinWidth{1}, MinHeight{1};
};
struct RuntimeTextureDesc {
    RenderExtentSpec Extent;
    render::TextureDimension Dimension{render::TextureDimension::Dim2D};
    uint32_t DepthOrArraySize{1}, MipLevels{1}, SampleCount{1};
    render::TextureFormat Format{render::TextureFormat::UNKNOWN};
    render::TextureUses Usage{render::TextureUse::UNKNOWN};
    render::ResourceHints Hints{render::ResourceHint::None};
};
std::optional<render::TextureDescriptor> ResolveRuntimeTextureDesc(
    const RuntimeTextureDesc& desc, const ResolvedRenderViewFamily& family, const render::Device& device, string& reason);

std::optional<render::TextureFormat> SelectFirstSupportedFormat(
    const render::Device& device, std::span<const render::TextureFormat> candidates,
    render::TextureDimension dimension, render::TextureUses usage, uint32_t sampleCount);

}  // namespace radray
