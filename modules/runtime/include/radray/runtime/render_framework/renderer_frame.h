#pragma once

#include <radray/runtime/render_framework/material_template.h>
#include <radray/runtime/render_framework/render_output.h>
#include <radray/runtime/vertex_layout.h>

namespace radray {
class FrameBuilder;
struct FramePreparationId {
    RendererFrameId Frame;
    uint32_t Index{UINT32_MAX};
    friend bool operator==(const FramePreparationId&, const FramePreparationId&) = default;
};
/// CPU request owned by the frame. Prepare runs once, after collection and before binding compilation.
class FramePreparation {
public:
    virtual ~FramePreparation() noexcept = default;
    virtual bool Prepare(FrameBuilder& builder) = 0;
};
struct FrameViewId {
    RendererFrameId Frame;
    uint32_t Index{UINT32_MAX};
    friend bool operator==(const FrameViewId&, const FrameViewId&) = default;
};
struct FramePassId {
    RendererFrameId Frame;
    uint32_t Index{UINT32_MAX};
    friend bool operator==(const FramePassId&, const FramePassId&) = default;
};
struct FrameDrawId {
    RendererFrameId Frame;
    uint32_t Index{UINT32_MAX};
    friend bool operator==(const FrameDrawId&, const FrameDrawId&) = default;
};
struct RendererViewDescriptor {
    vector<vector<byte>> Constants;
    vector<uint32_t> VisibleSlots;
    vector<ResourceId> ConstantBuffers;
    std::optional<ResourceId> VisibleBuffer;
};
struct RendererPassDescriptor {
    FrameViewId View;
    TemplateId Template;
    RenderOutputInfo Output;
    vector<vector<byte>> FrameConstants;
    vector<ResourceId> FrameBuffers;
    bool DefaultDepthTest{false};
};
struct RendererInstanceParameter {
    InstanceParameterLayout Layout;
    ResourceId Buffer;
};
/// Owned snapshots. Empty overrides select the registered immutable material version.
struct RendererDrawValues {
    vector<vector<byte>> Constants;
    vector<ResourceId> ConstantBuffers;
    vector<ResourceId> Textures;
    vector<render::SamplerDescriptor> Samplers;
    std::optional<ResourceId> Objects;
    vector<RendererInstanceParameter> Parameters;
};
struct RendererVertexBuffer {
    uint32_t Binding{0};
    ResourceId Buffer;
    uint64_t Offset{0}, Size{0};
    uint32_t Stride{0};
};
struct RendererIndexBuffer {
    ResourceId Buffer;
    uint32_t Offset{0}, Stride{0};
};
struct RendererDrawDescriptor {
    GeometryVertexLayout Geometry;
    vector<RendererVertexBuffer> Vertices;
    std::optional<RendererIndexBuffer> Indices;
    render::PrimitiveTopology Topology{render::PrimitiveTopology::TriangleList};
    uint32_t Count{0}, First{0}, Instances{1}, FirstInstance{0};
    int32_t VertexOffset{0};
    radray::Viewport Viewport;
    radray::Rect Scissor;
    vector<byte> Push;
    bool ReverseCulling{false};
};
}  // namespace radray
