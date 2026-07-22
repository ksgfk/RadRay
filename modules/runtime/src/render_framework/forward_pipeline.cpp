#include <radray/runtime/render_framework/forward_pipeline.h>
#include <radray/runtime/shader_binding_policy.h>

namespace radray {
namespace {

render::ShaderValueTypeDesc VectorType(
    render::ShaderScalarType scalar,
    uint32_t columns,
    uint32_t byteSize,
    uint32_t arrayCount = 1,
    uint32_t arrayStride = 0) {
    return render::ShaderValueTypeDesc{
        .Scalar = scalar,
        .Rows = 1,
        .Columns = columns,
        .ArrayCount = arrayCount,
        .ArrayStride = arrayStride,
        .ByteSize = byteSize};
}

render::ShaderInterfaceFieldDesc Float4Field(
    string name,
    uint32_t offset,
    uint32_t arrayCount = 1) {
    const uint32_t size = 16 * arrayCount;
    return render::ShaderInterfaceFieldDesc{
        .Name = std::move(name),
        .Offset = offset,
        .Size = size,
        .Type = VectorType(
            render::ShaderScalarType::Float32,
            4,
            size,
            arrayCount,
            arrayCount > 1 ? 16 : 0),
        .Members = {}};
}

render::ShaderInterfaceFieldDesc UInt4Field(string name, uint32_t offset) {
    return render::ShaderInterfaceFieldDesc{
        .Name = std::move(name),
        .Offset = offset,
        .Size = 16,
        .Type = VectorType(render::ShaderScalarType::UInt32, 4, 16),
        .Members = {}};
}

render::ShaderInterfaceFieldDesc MatrixField(
    string name,
    uint32_t offset,
    uint32_t arrayCount = 1) {
    const uint32_t size = 64 * arrayCount;
    return render::ShaderInterfaceFieldDesc{
        .Name = std::move(name),
        .Offset = offset,
        .Size = size,
        .Type = render::ShaderValueTypeDesc{
            .Scalar = render::ShaderScalarType::Float32,
            .Rows = 4,
            .Columns = 4,
            .ArrayCount = arrayCount,
            .ArrayStride = arrayCount > 1 ? 64u : 0u,
            .MatrixStride = 16,
            .ByteSize = size},
        .Members = {}};
}

render::ShaderInterfaceFieldDesc StructField(
    string name,
    uint32_t offset,
    uint32_t size,
    vector<render::ShaderInterfaceFieldDesc> members,
    uint32_t arrayCount = 1,
    uint32_t arrayStride = 0) {
    return render::ShaderInterfaceFieldDesc{
        .Name = std::move(name),
        .Offset = offset,
        .Size = size,
        .Type = render::ShaderValueTypeDesc{
            .Rows = 1,
            .Columns = 1,
            .ArrayCount = arrayCount,
            .ArrayStride = arrayStride,
            .ByteSize = size},
        .Members = std::move(members)};
}

render::ShaderBindingDesc ConstantBinding(
    uint32_t bindingIndex,
    uint32_t byteSize,
    vector<render::ShaderInterfaceFieldDesc> fields) {
    return render::ShaderBindingDesc{
        .Name = "ForwardConstants",
        .BindingIndex = bindingIndex,
        .Kind = render::ShaderBindingKind::ConstantBuffer,
        .Access = render::ShaderResourceAccess::ReadOnly,
        .Count = 1,
        .Buffer = render::ShaderBufferInterfaceDesc{
            .ByteSize = byteSize,
            .Fields = std::move(fields)}};
}

render::ShaderBindingDesc MakeObjectConstants() {
    return ConstantBinding(1, 64, {MatrixField("ObjectToWorld", 0)});
}

render::ShaderBindingDesc MakeForwardViewConstants() {
    return ConstantBinding(
        0,
        1424,
        {
            MatrixField("ViewProj", 0),
            Float4Field("CameraPosition", 64),
            UInt4Field("LightCounts", 80),
            StructField(
                "PointLights",
                96,
                256,
                {Float4Field("Position", 96), Float4Field("Intensity", 112)},
                8,
                32),
            StructField(
                "DirectionalLights",
                352,
                256,
                {Float4Field("Direction", 352), Float4Field("Irradiance", 368)},
                8,
                32),
            StructField(
                "PointShadow",
                608,
                416,
                {
                    MatrixField("ViewProj", 608, 6),
                    Float4Field("LightPosInvRadius", 992),
                    Float4Field("Params", 1008),
                }),
            StructField(
                "DirectionalShadow",
                1024,
                400,
                {
                    MatrixField("WorldToShadow", 1024, 4),
                    Float4Field("CascadeSphere", 1280, 4),
                    Float4Field("CascadeBias", 1344, 4),
                    Float4Field("Params", 1408),
                }),
        });
}

render::ShaderBindingDesc MakeShadowViewConstants() {
    return ConstantBinding(0, 384, {MatrixField("ViewProj", 0, 6)});
}

render::ShaderBindingDesc TextureBinding(
    uint32_t bindingIndex,
    render::ShaderTextureDimension dimension,
    bool arrayed) {
    return render::ShaderBindingDesc{
        .Name = "ForwardShadowTexture",
        .BindingIndex = bindingIndex,
        .Kind = render::ShaderBindingKind::SampledTexture,
        .Access = render::ShaderResourceAccess::ReadOnly,
        .Count = 1,
        .Texture = render::ShaderTextureInterfaceDesc{
            .Dimension = dimension,
            .SampleType = render::ShaderSampleType::Float,
            .Arrayed = arrayed}};
}

render::ShaderBindingDesc SamplerBinding(uint32_t bindingIndex) {
    return render::ShaderBindingDesc{
        .Name = "ForwardShadowSampler",
        .BindingIndex = bindingIndex,
        .Kind = render::ShaderBindingKind::Sampler,
        .Access = render::ShaderResourceAccess::ReadOnly,
        .Count = 1};
}

shared_ptr<const IShaderBindingProvider> MakeObjectProvider() {
    return make_shared<ShaderBindingSchemaProvider>(
        "ForwardObject",
        vector<ShaderBindingProviderSchemaEntry>{ShaderBindingProviderSchemaEntry{
            .AcceptedBindings = {MakeObjectConstants()}}});
}

shared_ptr<const IShaderBindingProvider> MakePipelineProvider() {
    return make_shared<ShaderBindingSchemaProvider>(
        "ForwardPipeline",
        vector<ShaderBindingProviderSchemaEntry>{
            ShaderBindingProviderSchemaEntry{
                .AcceptedBindings = {MakeForwardViewConstants(), MakeShadowViewConstants()}},
            ShaderBindingProviderSchemaEntry{
                .AcceptedBindings = {
                    TextureBinding(1, render::ShaderTextureDimension::Cube, false)}},
            ShaderBindingProviderSchemaEntry{.AcceptedBindings = {TextureBinding(2, render::ShaderTextureDimension::Dim2D, true)}},
            ShaderBindingProviderSchemaEntry{.AcceptedBindings = {SamplerBinding(3)}},
        });
}

}  // namespace

ForwardPipeline::~ForwardPipeline() noexcept = default;

const PipelineBindingPolicy& ForwardPipeline::GetShaderBindingPolicy() const noexcept {
    static const PipelineBindingPolicy policy{{PipelineBindingReservation{
                                                   .GroupIndex = kObjectBindingGroup,
                                                   .Provider = MakeObjectProvider()},
                                               PipelineBindingReservation{
                                                   .GroupIndex = kPipelineBindingGroup,
                                                   .Provider = MakePipelineProvider()}}};
    return policy;
}

}  // namespace radray
