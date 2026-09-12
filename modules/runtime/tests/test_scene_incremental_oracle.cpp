#include "foundation_graph_fixture.h"
#include "upload_test_support.h"
#include "forward_pipeline/forward_frame.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/render_framework/frame_draw_resources.h>
#include <radray/runtime/render_framework/mesh_pass_processor.h>
#include <radray/runtime/render_framework/scene.h>
#include <radray/runtime/render_framework/view_state.h>

namespace radray {
namespace {

constexpr uint32_t kOracleSlots = 24, kOracleMaterials = 6;
constexpr uint64_t kOracleSeed = 0x58c0ffee1234abcdull;
constexpr uint32_t kOracleEpochs = 384;

class OracleRandom {
public:
    uint32_t Next(uint32_t limit) {
        State ^= State << 13;
        State ^= State >> 7;
        State ^= State << 17;
        return static_cast<uint32_t>(State % limit);
    }
    uint64_t State{kOracleSeed};
};

struct AuthoringMaterial {
    uint32_t Schema{0};
    uint64_t Generation{0};
    array<float, 8> Values{};
    MaterialPipelineState State;
    RenderQueue Queue{RenderQueue::Geometry};
};

struct AuthoringPrimitive {
    bool Active{false};
    uint32_t AuthoringSlot{0};
    SceneObjectId Id;
    uint64_t ProxyGeneration{0};
    uint64_t MotionEpoch{0};
    uint32_t Material{0}, Geometry{0}, FirstIndex{0}, IndexCount{3};
    int32_t VertexOffset{0};
    uint32_t Layer{1};
    bool DisableCulling{false};
    bool GeometryReady{true};
    Nullable<const array<GpuMesh::DrawData, 2>*> GeometryOwner{nullptr};
    Eigen::Matrix4f Transform{Eigen::Matrix4f::Identity()};
    AxisAlignedBounds LocalBounds{Eigen::Vector3f::Constant(-.025f), Eigen::Vector3f::Constant(.025f)};
};

struct ReferencePrimitive {
    AuthoringPrimitive Source;
    AuthoringMaterial Material;
    AxisAlignedBounds Bounds;
    Eigen::Matrix4f Normal{Eigen::Matrix4f::Zero()};
    bool Mirrored{false};
    array<Forward_ObjectData, 2> Temporal;
};

// The reference accepts only test-owned authoring facts, including identities returned at
// registration. It must not inspect a published snapshot, catalog, object cache, list or Ready.
vector<ReferencePrimitive> RebuildReference(const array<AuthoringPrimitive, kOracleSlots>& primitives,
                                            const array<AuthoringMaterial, kOracleMaterials>& materials) {
    vector<ReferencePrimitive> result;
    for (const auto& input : primitives) {
        if (!input.Active) continue;
        auto& value = result.emplace_back();
        value.Source = input;
        value.Material = materials[input.Material];
        Eigen::Vector3d low = Eigen::Vector3d::Constant(std::numeric_limits<double>::infinity());
        Eigen::Vector3d high = -low;
        for (uint32_t corner = 0; corner < 8; ++corner) {
            Eigen::Vector4d local;
            for (uint32_t axis = 0; axis < 3; ++axis)
                local[axis] = corner & (1u << axis) ? input.LocalBounds.Max[axis] : input.LocalBounds.Min[axis];
            local[3] = 1;
            const Eigen::Vector3d world = (input.Transform.cast<double>() * local).head<3>();
            low = low.cwiseMin(world);
            high = high.cwiseMax(world);
        }
        value.Bounds = {low.cast<float>(), high.cast<float>()};
        const Eigen::Matrix3d linear = input.Transform.block<3, 3>(0, 0).cast<double>();
        value.Mirrored = linear.determinant() < 0;
        Eigen::Matrix3d normal = linear.inverse().transpose();
        normal /= normal.cwiseAbs().maxCoeff();
        value.Normal.block<3, 3>(0, 0) = normal.cast<float>();
    }
    return result;
}

vector<byte> ReferenceBytes(const AuthoringMaterial& material) {
    const auto bytes = std::as_bytes(std::span{material.Values.data(), material.Schema ? size_t{8} : size_t{4}});
    return {bytes.begin(), bytes.end()};
}

MaterialPipelineState ReferenceState(const ReferencePrimitive& primitive, uint32_t policy, uint64_t revision, bool mirrored) {
    auto state = primitive.Material.State;
    if (policy == 1) state.Primitive.Cull = revision % 2 ? render::CullMode::Front : render::CullMode::None;
    if (mirrored)
        state.Primitive.FaceClockwise = state.Primitive.FaceClockwise == render::FrontFace::CW ? render::FrontFace::CCW : render::FrontFace::CW;
    return state;
}

class OracleGeometryAsset final : public Asset {
public:
    OracleGeometryAsset(const array<GpuMesh::DrawData, 2>& geometry, shared_ptr<uint32_t> destroyed)
        : Geometry(geometry), Destroyed(std::move(destroyed)) {}
    ~OracleGeometryAsset() noexcept override { ++*Destroyed; }
    void OnUnload(AssetManager&) override {}
    array<GpuMesh::DrawData, 2> Geometry;
    shared_ptr<uint32_t> Destroyed;
};

class OracleLoadGate {
public:
    ~OracleLoadGate() { Resume(); }
    void Resume() {
        const auto pending = Pending;
        Pending = {};
        if (pending) pending.resume();
    }
    bool IsPending() const noexcept { return bool(Pending); }
    struct Awaiter {
        OracleLoadGate* Gate;
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> continuation) const noexcept { Gate->Pending = continuation; }
        void await_resume() const noexcept {}
    };
    Awaiter Wait() noexcept { return {this}; }

private:
    std::coroutine_handle<> Pending{};
};

class OraclePrimitive final : public PrimitiveSceneProxy {
public:
    OraclePrimitive(const AuthoringPrimitive& value, const GpuMesh::DrawData* geometry, Material* material)
        : Value(value), Geometry(geometry), DrawMaterial(material) { SetLocalToWorld(value.Transform); }
    bool UsesRenderChangeNotifications() const noexcept override { return true; }
    uint64_t GetRenderDataRevision() const noexcept override { return Revision * 2 + (Stream.IsValid() ? Stream.IsReady() : 1); }
    bool HasPendingRenderResources() const noexcept override { return Stream.IsValid() && !Stream.IsCompleted(); }
    void CollectAssetReferences(vector<StreamingAssetRefAny>& out) const override {
        if (Stream.IsValid()) out.push_back(Stream.AsAny());
    }
    uint64_t GetTransformRevision() const noexcept override { return GetLocalToWorldRevision(); }
    uint32_t GetSectionCount() const noexcept override { return Stream.IsValid() && !Stream.IsReady() ? 0 : 1; }
    AxisAlignedBounds GetLocalBounds() const noexcept override { return Value.LocalBounds; }
    uint32_t GetLayerMask() const noexcept override { return Value.Layer; }
    bool IsFrustumCullingDisabled() const noexcept override { return Value.DisableCulling; }
    MeshDrawArgs GetDrawArgs(uint32_t) const noexcept override {
        const auto asset = Stream.Get();
        return {asset ? &asset->Geometry[Value.Geometry] : Geometry, Value.FirstIndex, Value.IndexCount, Value.VertexOffset};
    }
    Nullable<Material*> GetMaterial(uint32_t) const noexcept override { return DrawMaterial; }
    void Apply(const AuthoringPrimitive& value, const GpuMesh::DrawData* geometry, Material* material, PrimitiveDirtyFlags dirty) {
        Value = value;
        Geometry = geometry;
        DrawMaterial = material;
        if (dirty.HasFlag(PrimitiveDirtyKind::Structure)) ++Revision;
        SetLocalToWorld(value.Transform);
        MarkRenderDirty(dirty);
    }
    void SetStream(StreamingAssetRef<OracleGeometryAsset> stream) {
        Stream = std::move(stream);
        ++Revision;
        MarkRenderDirty(PrimitiveDirtyKind::Structure);
    }

private:
    AuthoringPrimitive Value;
    const GpuMesh::DrawData* Geometry;
    Material* DrawMaterial;
    StreamingAssetRef<OracleGeometryAsset> Stream;
    uint64_t Revision{1};
};

bool CompileOracleBase(const StaticPassCompileInput& input, StaticPassCompileResult& output) {
    output.NormalState = input.Pass.PipelineState;
    output.MirroredState = output.NormalState;
    output.MirroredState.Primitive.FaceClockwise = OppositeFrontFace(output.NormalState.Primitive.FaceClockwise);
    output.Bindings = input.Bindings ? *input.Bindings : StaticBindingRecipe{};
    output.Bindings.Buffers[1] = 0;
    output.Bindings.Groups[1] = 1;
    output.Bindings.Buffers[2] = 1;
    output.Bindings.Groups[2] = 2;
    output.Bindings.GroupOrder[0] = 1;
    output.Bindings.GroupOrder[1] = 2;
    output.Bindings.GroupCount = 2;
    output.Bindings.Valid = true;
    return true;
}
bool CompileOracleFront(const StaticPassCompileInput& input, StaticPassCompileResult& output) {
    CompileOracleBase(input, output);
    output.NormalState.Primitive.Cull = output.MirroredState.Primitive.Cull = render::CullMode::Front;
    return true;
}
bool CompileOracleTwoSided(const StaticPassCompileInput& input, StaticPassCompileResult& output) {
    CompileOracleBase(input, output);
    output.NormalState.Primitive.Cull = output.MirroredState.Primitive.Cull = render::CullMode::None;
    return true;
}

class OracleCaptureSet final : public render::ShaderParameterSet {
public:
    bool IsValid() const noexcept override { return Flushed; }
    void Destroy() noexcept override {}
    bool Set(render::BindingHandle binding, uint32_t element, render::ShaderParameterValue value) noexcept override {
        const auto* buffer = std::get_if<render::ShaderBufferBinding>(&value);
        if (!binding.IsValid() || element != 0 || !buffer || !buffer->Target || Buffer) return false;
        Binding = binding;
        Buffer = *buffer;
        return true;
    }
    bool FlushWrites() noexcept override {
        Flushed = Buffer.has_value();
        return Flushed;
    }
    render::BindingHandle Binding;
    std::optional<render::ShaderBufferBinding> Buffer;
    bool Flushed{false};
};

struct OraclePipeline {
    render::GraphicsPipelineState* Native;
    render::PipelineLayout* Layout;
    render::FrontFace Face;
    render::CullMode Cull;
    render::ColorWrites WriteMask;
    uint32_t VertexStride;
};

// Real reflection/shaders/PSOs, CPU-readable upload buffers and captured descriptor writes.
// Captured sets are consumed only by the fake encoder, never submitted to a native encoder.
class OracleCaptureDevice final : public test::GraphCompileDevice {
public:
    explicit OracleCaptureDevice(render::Device& native) : Native(native) {}
    render::RenderBackend GetBackend() noexcept override { return Native.GetBackend(); }
    render::DeviceDetail GetDetail() const noexcept override { return Native.GetDetail(); }
    const render::RenderDeviceCapabilities& GetCapabilities() const noexcept override { return Native.GetCapabilities(); }
    render::TextureSupport QueryTextureSupport(const render::TextureSupportQuery& query) const noexcept override { return Native.QueryTextureSupport(query); }
    Nullable<unique_ptr<render::Texture>> CreateTexture(const render::TextureDescriptor& desc) noexcept override { return Native.CreateTexture(desc); }
    Nullable<unique_ptr<render::TextureView>> CreateTextureView(const render::TextureViewDescriptor& desc) noexcept override { return Native.CreateTextureView(desc); }
    Nullable<unique_ptr<render::RenderPass>> CreateRenderPass(const render::RenderPassDescriptor& desc) noexcept override { return Native.CreateRenderPass(desc); }
    Nullable<unique_ptr<render::Framebuffer>> CreateFramebuffer(const render::FramebufferDescriptor& desc) noexcept override { return Native.CreateFramebuffer(desc); }
    Nullable<unique_ptr<render::Shader>> CreateShader(const render::ShaderDescriptor& desc) noexcept override { return Native.CreateShader(desc); }
    Nullable<unique_ptr<render::Buffer>> CreateBuffer(const render::BufferDescriptor& desc) noexcept override {
        return unique_ptr<render::Buffer>{new test::UploadTestBuffer(this, desc, LiveBuffers)};
    }
    Nullable<unique_ptr<render::ShaderParameterSet>> CreateShaderParameterSet(const render::ShaderParameterSetDescriptor&) noexcept override {
        return unique_ptr<render::ShaderParameterSet>{new OracleCaptureSet};
    }
    Nullable<unique_ptr<render::GraphicsPipelineState>> CreateGraphicsPipelineState(const render::GraphicsPipelineStateDescriptor& desc) noexcept override {
        auto pipeline = Native.CreateGraphicsPipelineState(desc);
        if (pipeline && desc.ColorTargets.size() == 1 && desc.VertexInput.Buffers.size() == 1)
            Pipelines.push_back({pipeline.Get(), desc.PipelineLayout, desc.Primitive.FaceClockwise, desc.Primitive.Cull, desc.ColorTargets[0].WriteMask, desc.VertexInput.Buffers[0].ArrayStride});
        return pipeline;
    }
    vector<OraclePipeline> Pipelines;

private:
    render::Device& Native;
    int LiveBuffers{0};
};

vector<byte> DecodeMaterial(render::ShaderParameterSet* source, std::span<const render::ShaderParameterDynamicOffset> offsets) {
    const auto& set = *static_cast<OracleCaptureSet*>(source);
    if (!set.Flushed || !set.Buffer || offsets.size() != 1 || offsets[0].Binding != set.Binding) return {};
    const auto& binding = *set.Buffer;
    const auto offset = binding.Range.Offset + offsets[0].Offset;
    if (offset > binding.Target->GetDesc().Size || binding.Range.Size > binding.Target->GetDesc().Size - offset) return {};
    auto* mapped = static_cast<const byte*>(binding.Target->Map(offset, binding.Range.Size));
    if (!mapped) return {};
    vector<byte> result(mapped, mapped + binding.Range.Size);
    binding.Target->Unmap();
    return result;
}

class OracleProcessor final : public MeshPassProcessor {
public:
    OracleProcessor(FrameDrawResources& groups, const PackedCBufferTable& objects, ViewStateRegistry& history)
        : Groups(groups), Objects(objects), History(history) {}
    void AddMeshBatch(const RendererListDesc&, const RenderSceneSnapshot&, const MeshBatch&, MeshPassDrawListContext& out) override {
        ++DynamicFallbacks;
        out.Reject(MeshPassRejectReason::ProcessorRejected);
    }
    void PrepareRecord(const RendererListDesc& desc, const RenderSceneSnapshot& scene, const DrawRecord& record, MeshPassDrawListContext& out) override {
        const auto& material = scene.Materials[record.Material];
        const auto& pass = material.Passes[record.PassIndex];
        const auto group = Groups.PrepareGroupId(*pass.Program, 1,
                                                 {.Source = &scene, .WireType = &_wire, .Generation = material.Generation, .Revision = material.ValuesRevision}, record.Material, pass.NumericBytes);
        if (!group.IsValid()) {
            out.Reject(MeshPassRejectReason::PrepareResourceFailed);
            return;
        }
        auto object = *AsCBuffer<Forward_ObjectData>(Objects.Row(record.Primitive));
        const auto motion = History.GetPrimitiveMotion(desc.View->StateId, scene.Primitives[record.Primitive]);
        object.PreviousLocalToWorld = motion.PreviousLocalToWorld;
        object.MotionValid = motion.Valid && desc.View->PreviousViewValid ? 1u : 0u;
        const auto stamp = History.GetPrimitiveHistoryStamp(desc.View->StateId);
        const auto objectGroup = Groups.PrepareGroupId(*pass.Program, 2,
                                                       {.Source = &Objects, .WireType = &_objectWire, .Context = desc.View->StateId.Value, .Generation = stamp.CommittedSerial, .Revision = stamp.InvalidationRevision, .HistoryRevision = desc.View->PreviousViewValid ? 1u : 0u, .HistoryOwner = &History},
                                                       record.Primitive, AsCBufferBytes(object));
        if (!objectGroup.IsValid()) {
            out.Reject(MeshPassRejectReason::PrepareResourceFailed);
            return;
        }
        const array<FrameShaderGroupId, 2> groups{group, objectGroup};
        out.AddRecord(Groups, Groups.InternBinding(groups));
    }
    uint32_t DynamicFallbacks{0};

private:
    FrameDrawResources& Groups;
    const PackedCBufferTable& Objects;
    ViewStateRegistry& History;
    inline static const byte _wire{};
    inline static const byte _objectWire{};
};

struct OracleTraceDraw {
    Nullable<render::GraphicsPipelineState*> Pipeline{nullptr};
    Nullable<render::Buffer*> Vertex{nullptr}, Index{nullptr};
    uint64_t VertexOffset{0}, VertexSize{0};
    uint32_t IndexOffset{0}, IndexStride{0};
    array<int64_t, 5> Arguments{};
    vector<byte> Material;
    vector<byte> Object;
};

class OracleEncoder final : public render::GraphicsCommandEncoder {
public:
    explicit OracleEncoder(render::CommandBuffer& command) : Command(command) {}
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    render::CommandBuffer* GetCommandBuffer() const noexcept override { return &Command; }
    void BindGraphicsPipelineState(render::GraphicsPipelineState* pipeline) noexcept override {
        State.Pipeline = pipeline;
        State.Material.clear();
        State.Object.clear();
    }
    void BindShaderParameterSet(uint32_t group, render::ShaderParameterSet* set, std::span<const render::ShaderParameterDynamicOffset> offsets) noexcept override {
        if (group == 1)
            State.Material = DecodeMaterial(set, offsets);
        else if (group == 2)
            State.Object = DecodeMaterial(set, offsets);
        else
            Unexpected = true;
    }
    void BindVertexBuffers(std::span<const render::VertexBufferBinding> bindings) noexcept override {
        EXPECT_EQ(bindings.size(), 1u);
        if (bindings.size() != 1) return;
        EXPECT_EQ(bindings[0].Binding, 0u);
        State.Vertex = bindings[0].View.Target;
        State.VertexOffset = bindings[0].View.Offset;
        State.VertexSize = bindings[0].View.Size;
    }
    void BindIndexBuffer(render::IndexBufferView view) noexcept override {
        State.Index = view.Target;
        State.IndexOffset = view.Offset;
        State.IndexStride = view.Stride;
    }
    void DrawIndexed(uint32_t count, uint32_t instances, uint32_t first, int32_t offset, uint32_t firstInstance) noexcept override {
        State.Arguments = {count, instances, first, offset, firstInstance};
        Draws.push_back(State);
    }
    void SetViewport(Viewport) noexcept override {}
    void SetScissor(Rect) noexcept override {}
    bool SetPushConstants(render::BindingHandle, std::span<const byte>) noexcept override {
        Unexpected = true;
        return false;
    }
    void Draw(uint32_t, uint32_t, uint32_t, uint32_t) noexcept override { Unexpected = true; }
    void DrawIndirect(render::Buffer*, uint64_t, uint32_t) noexcept override { Unexpected = true; }
    void DrawIndexedIndirect(render::Buffer*, uint64_t, uint32_t) noexcept override { Unexpected = true; }
    vector<OracleTraceDraw> Draws;
    bool Unexpected{false};

private:
    render::CommandBuffer& Command;
    OracleTraceDraw State;
};

struct OracleFlight {
    shared_ptr<const RenderSceneSnapshot> Snapshot;
    vector<ReferencePrimitive> Reference;
    vector<const ReferencePrimitive*> LastExpected;
    vector<StreamingAssetRefAny> Owners;
    forward_detail::ForwardObjectDataCache Objects;
    unique_ptr<FrameDrawResources> Groups;
    HostWriteBatch Writes;
    array<RendererList, 4> Lists;
    array<ResolvedRenderView, 2> Views;
    uint64_t Epoch{0}, PolicyRevision{1};
};

class OracleWorld {
public:
    explicit OracleWorld(render::Device& native) : Native(native), Capture(native), HistoryRegistry(&native), History(native, HistoryRegistry, 3) {}
    void Initialize() {
        render::ShaderProgramLayoutRecipe recipe;
        const render::ShaderLayoutSelector selector{.DeclarationName = "MaterialValues", .ExpectedLogicalResourceKind = shader::ShaderBindingKind::CBuffer};
        recipe.D3D12.BufferPlacements.push_back({selector, render::D3D12BufferPlacement::RootDescriptor});
        recipe.Vulkan.BufferDescriptors.push_back({selector, render::VulkanBufferDescriptorPlacement::Dynamic});
        const render::ShaderLayoutSelector objectSelector{.DeclarationName = "OracleObject", .ExpectedLogicalResourceKind = shader::ShaderBindingKind::CBuffer};
        recipe.D3D12.BufferPlacements.push_back({objectSelector, render::D3D12BufferPlacement::RootDescriptor});
        recipe.Vulkan.BufferDescriptors.push_back({objectSelector, render::VulkanBufferDescriptorPlacement::Dynamic});
        for (uint32_t schema = 0; schema < Programs.size(); ++schema) {
            const string source = "#include <core/platform.hlsli>\n#include <pipelines/forward/cbuffers.hlsli>\nstruct Values { float4 BaseColor; " + string{schema ? "float4 Extra;" : ""} +
                                  " };\n"
                                  "VK_BINDING(0, 1) ConstantBuffer<Values> MaterialValues : register(b0, space1);\n"
                                  "VK_BINDING(0, 2) ConstantBuffer<Forward_ObjectData> OracleObject : register(b0, space2);\n"
                                  "[shader(\"vertex\")] float4 VSMain(float3 p : POSITION) : SV_Position { return mul(OracleObject.LocalToWorld, float4(p, 1)) + mul(OracleObject.PreviousLocalToWorld, float4(p, 1)) * OracleObject.MotionValid * .0001; }\n"
                                  "[shader(\"pixel\")] float4 PSMain() : SV_Target0 { return MaterialValues.BaseColor" +
                                  string{schema ? " + MaterialValues.Extra * .01" : ""} + "; }\n";
            auto program = test::CompileFoundationGraphics(Native, source, recipe, &Capture);
            ASSERT_TRUE(program);
            Programs[schema] = program.Release();
            auto technique = MaterialTechnique::Create({{"Oracle", Programs[schema].get(), "MaterialValues", {}}}, "Oracle");
            ASSERT_TRUE(technique);
            Techniques[schema] = technique.Release();
        }
        for (uint32_t index = 0; index < Materials.size(); ++index) {
            auto& model = MaterialModel[index];
            model.Schema = index % 2;
            model.Values = {float(index + 1), .25f, .5f, 1, .75f, 1, 1.25f, 1.5f};
            model.State.DepthStencil.DepthTestEnable = model.State.DepthStencil.DepthWriteEnable = false;
            auto material = Material::Create(Techniques[model.Schema].get());
            ASSERT_TRUE(material);
            Materials[index] = material.Release();
            model.Generation = Materials[index]->GetGeneration();
            ASSERT_TRUE(Materials[index]->SetNumericData(ReferenceBytes(model)));
            ASSERT_TRUE(Materials[index]->SetPassPipelineState("Oracle", model.State));
        }
        const auto copyUses = render::BufferUse::CopySource | render::BufferUse::CopyDestination;
        auto vb = Native.CreateBuffer({256, render::MemoryType::Device, copyUses | render::BufferUse::Vertex, {}});
        auto ib = Native.CreateBuffer({96, render::MemoryType::Device, copyUses | render::BufferUse::Index, {}});
        ASSERT_TRUE(vb && ib);
        Vertices = vb.Release();
        Indices = ib.Release();
        for (uint32_t variant = 0; variant < Geometry.size(); ++variant) {
            Geometry[variant].VertexBuffers = {{0, {Vertices.get(), variant * 16u, 192}}};
            Geometry[variant].Ibv = {Indices.get(), variant * 4u, 4};
            Geometry[variant].VertexLayout.Buffers = {{0, variant ? 16u : 12u, render::VertexStepMode::Vertex}};
            Geometry[variant].VertexLayout.Attributes = {{"POSITION", 0, 0, 0, render::VertexFormat::FLOAT32X3}};
        }
        for (uint32_t slot = 0; slot < Models.size(); ++slot) {
            Models[slot].AuthoringSlot = slot;
            Models[slot].Material = slot % kOracleMaterials;
            Models[slot].Transform(0, 3) = float(slot % 4) * .3f - .45f;
            Models[slot].Transform(1, 3) = float(slot / 4) * .2f - .5f;
            Models[slot].Transform(2, 3) = slot < kOracleMaterials ? .02f + float(slot / 2) * .01f : .1f + float(slot) * .025f;
            Register(slot);
        }
        for (auto& flight : Flights) flight.Groups = make_unique<FrameDrawResources>(&Capture);
        Initialized = true;
    }

    void Register(uint32_t slot) {
        auto& model = Models[slot];
        const auto previous = model.Id;
        auto proxy = make_unique<OraclePrimitive>(model, &Geometry[model.Geometry], Materials[model.Material].get());
        Proxies[slot] = proxy.get();
        model.ProxyGeneration = proxy->GetGeneration();
        ASSERT_TRUE(Source.AddPrimitive(std::move(proxy)));
        model.Id = Source.GetPrimitiveId(Proxies[slot].Get());
        ASSERT_TRUE(model.Id.IsValid());
        if (previous.IsValid()) EXPECT_NE(model.Id, previous);
        model.Active = true;
        model.MotionEpoch = 0;
    }

    void Mutate(uint32_t epoch) {
        // One permanent primitive per material anchors its first-use rank, both program
        // ranks, and equal-depth tie pairs independently of snapshot packing and caches.
        const uint32_t slot = kOracleMaterials + Random.Next(kOracleSlots - kOracleMaterials - 1);
        auto& model = Models[slot];
        const uint32_t operation = epoch % 12;
        ++Operations[operation];
        if (operation == 0) {
            if (model.Active) {
                Source.RemovePrimitive(Proxies[slot].Get());
                Proxies[slot] = nullptr;
                model.Active = false;
            } else
                Register(slot);
            return;
        }
        if (!model.Active) Register(slot);
        PrimitiveDirtyFlags dirty{};
        switch (operation) {
            case 1: {
                const float sign = Random.Next(2) ? -1.f : 1.f;
                model.Transform(0, 0) = sign * (.5f + .25f * Random.Next(7));
                model.Transform(1, 1) = .5f + .25f * Random.Next(7);
                model.Transform(2, 2) = .75f + .25f * Random.Next(4);
                model.Transform(0, 1) = float(int(Random.Next(5)) - 2) * .125f;
                model.Transform(0, 3) = float(int(Random.Next(11)) - 5) * .3f;
                model.Transform(2, 3) = .1f + float(slot) * .025f + float(Random.Next(4)) * .001f;
                dirty = PrimitiveDirtyKind::TransformOrBounds;
                break;
            }
            case 2: {
                const auto material = Random.Next(kOracleMaterials);
                MaterialModel[material].Values[0] = float(epoch) * .125f;
                MaterialModel[material].Values[1] = float(slot) * .25f;
                ASSERT_TRUE(Materials[material]->SetNumericData(ReferenceBytes(MaterialModel[material])));
                return;
            }
            case 3:
                model.Material = (model.Material + 1 + 2 * Random.Next(kOracleMaterials / 2)) % kOracleMaterials;
                dirty = PrimitiveDirtyKind::MaterialAssignment;
                break;
            case 4:
                model.Geometry ^= 1;
                model.FirstIndex = Random.Next(4);
                model.IndexCount = Random.Next(2) ? 6 : 3;
                model.VertexOffset = Random.Next(2) ? -1 : 0;
                dirty = PrimitiveDirtyKind::Structure;
                break;
            case 5: {
                const auto material = Random.Next(kOracleMaterials);
                auto& state = MaterialModel[material].State;
                state.Primitive.FaceClockwise = Random.Next(2) ? render::FrontFace::CW : render::FrontFace::CCW;
                state.Primitive.Cull = Random.Next(2) ? render::CullMode::Back : render::CullMode::None;
                state.WriteMask = Random.Next(2) ? render::ColorWrite::All : render::ColorWrite::Red;
                ASSERT_TRUE(Materials[material]->SetPassPipelineState("Oracle", state));
                return;
            }
            case 6:
                model.Layer = Random.Next(2) ? 1 : 2;
                model.DisableCulling = Random.Next(2) != 0;
                dirty = PrimitiveDirtyKind::Filter;
                break;
            case 7:
                model.LocalBounds.Min = Eigen::Vector3f{-.02f, -.03f, -.02f};
                model.LocalBounds.Max = Eigen::Vector3f{.02f + Random.Next(5) * .01f, .04f, .02f};
                dirty = PrimitiveDirtyKind::Structure | PrimitiveDirtyKind::TransformOrBounds;
                break;
            case 8: {
                const auto material = Random.Next(kOracleMaterials);
                MaterialModel[material].Queue = Random.Next(2) ? RenderQueue::Geometry : RenderQueue::Transparent;
                Materials[material]->SetRenderQueue(MaterialModel[material].Queue);
                return;
            }
            case 9: ++PolicyRevision; return;
            case 10: {
                const auto old = model.Id;
                Source.RemovePrimitive(Proxies[slot].Get());
                Proxies[slot] = nullptr;
                model.Active = false;
                Register(slot);
                EXPECT_EQ(model.Id.Slot, old.Slot);
                EXPECT_GT(model.Id.Generation, old.Generation);
                return;
            }
            default:
                Proxies[slot]->ResetMotion();
                ++model.MotionEpoch;
                return;
        }
        Proxies[slot]->Apply(model, &Geometry[model.Geometry], Materials[model.Material].get(), dirty);
    }

    void AdvanceStreaming(uint32_t iteration) {
        constexpr uint32_t slot = kOracleSlots - 1;
        auto& model = Models[slot];
        const auto phase = iteration % 24, cycle = iteration / 24;
        if (phase == 0) {
            ASSERT_FALSE(LoadGate.IsPending());
            ASSERT_TRUE(model.Active);
            auto asset = make_unique<OracleGeometryAsset>(Geometry, DestroyedGeometry);
            model.GeometryOwner = &asset->Geometry;
            model.GeometryReady = false;
            const AssetId id{0x58000000u + cycle, 0x1122, 0x3344, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc};
            Stream = Assets.Load<OracleGeometryAsset>({id, [](OracleLoadGate* gate, unique_ptr<OracleGeometryAsset> value, bool fail) -> task<AssetLoadResult> {
                                                           co_await gate->Wait();
                                                           if (fail) co_return AssetLoadResult::Failure("oracle scheduled load failure");
                                                           co_return AssetLoadResult::Success(std::move(value));
                                                       }(&LoadGate, std::move(asset), cycle % 3 == 1),
                                                       "oracle streamed geometry"});
            Proxies[slot]->SetStream(Stream);
            ASSERT_TRUE(LoadGate.IsPending());
            ++StreamingStarts;
        } else if (phase == 2 && cycle % 3 == 2) {
            Source.RemovePrimitive(Proxies[slot].Get());
            Proxies[slot] = nullptr;
            model.Active = false;
            ++StreamingRemovedPending;
        } else if (phase == 3) {
            LoadGate.Resume();
            Assets.Pump();
            model.GeometryReady = cycle % 3 != 1;
            EXPECT_EQ(Stream.IsReady(), model.GeometryReady);
            EXPECT_EQ(Stream.IsFaulted(), !model.GeometryReady);
            if (model.GeometryReady)
                ++StreamingReady;
            else
                ++StreamingFailed;
            // No proxy setter or dirty notification accompanies completion.
        } else if (phase == 4 && !model.Active) {
            model.GeometryReady = true;
            model.GeometryOwner = nullptr;
            Stream.Reset();
            Register(slot);
        }
        Assets.Pump();
    }

    void Publish(uint64_t epoch, uint32_t flightIndex) {
        auto& flight = Flights[flightIndex];
        for (auto& list : flight.Lists) list.ResetForReuse();
        flight.LastExpected.clear();
        flight.Owners.clear();
        const array<PassPolicy, 2> policies{{{{5801}, 1, "Oracle", CompileOracleBase},
                                             {{5802}, PolicyRevision, "Oracle", PolicyRevision % 2 ? CompileOracleFront : CompileOracleTwoSided}}};
        ASSERT_TRUE(Source.GetDrawStore().SetActivePolicies(epoch, policies));
        flight.Reference = RebuildReference(Models, MaterialModel);
        auto snapshot = Source.GetRenderState().PrepareShared(Source, epoch, flightIndex, flight.Owners, RenderValidationMode::Full);
        ASSERT_TRUE(snapshot);
        flight.Snapshot = snapshot.Release();
        flight.Epoch = epoch;
        flight.PolicyRevision = PolicyRevision;
        flight.Objects.Update(*flight.Snapshot);
        EXPECT_EQ(flight.Objects.Update(*flight.Snapshot), 0u);
        flight.Writes.Reset();
        ASSERT_TRUE(flight.Groups->BeginFrame(flight.Writes));
    }

    void CheckSnapshot(const OracleFlight& flight) const {
        if (!flight.Snapshot) return;
        const auto& snapshot = *flight.Snapshot;
        ASSERT_TRUE(snapshot.Valid);
        EXPECT_EQ(snapshot.SceneEpoch, flight.Epoch);
        ASSERT_EQ(snapshot.Primitives.size(), flight.Reference.size());
        const auto readyCount = std::count_if(flight.Reference.begin(), flight.Reference.end(), [](const auto& value) { return value.Source.GeometryReady; });
        ASSERT_EQ(snapshot.MeshBatches.size(), readyCount);
        ASSERT_EQ(snapshot.DrawRecords.size(), readyCount * 2);
        ASSERT_EQ(snapshot.PrimitiveDrawBegin.size(), flight.Reference.size() + 1);
        ASSERT_EQ(flight.Objects.Rows().RowCount(), flight.Reference.size());
        vector<uint64_t> uniqueMaterials;
        for (const auto& expected : flight.Reference)
            if (std::find(uniqueMaterials.begin(), uniqueMaterials.end(), expected.Material.Generation) == uniqueMaterials.end()) uniqueMaterials.push_back(expected.Material.Generation);
        EXPECT_EQ(snapshot.Materials.size(), uniqueMaterials.size());
        for (const auto& expected : flight.Reference) {
            const auto found = std::find_if(snapshot.Primitives.begin(), snapshot.Primitives.end(), [&](const auto& actual) { return actual.Id == expected.Source.Id; });
            ASSERT_NE(found, snapshot.Primitives.end());
            const auto index = static_cast<uint32_t>(found - snapshot.Primitives.begin());
            const auto& actual = *found;
            EXPECT_EQ(actual.Generation, expected.Source.ProxyGeneration);
            EXPECT_TRUE(actual.LocalToWorld.isApprox(expected.Source.Transform, 1e-6f));
            EXPECT_TRUE(actual.WorldBounds.Min.isApprox(expected.Bounds.Min, 1e-5f));
            EXPECT_TRUE(actual.WorldBounds.Max.isApprox(expected.Bounds.Max, 1e-5f));
            EXPECT_EQ(actual.LayerMask, expected.Source.Layer);
            EXPECT_EQ(actual.DisableFrustumCulling, expected.Source.DisableCulling);
            const auto* object = AsCBuffer<Forward_ObjectData>(flight.Objects.Rows().Row(index));
            EXPECT_TRUE(static_cast<Eigen::Matrix4f>(object->LocalToWorld).isApprox(expected.Source.Transform, 1e-6f));
            EXPECT_TRUE(static_cast<Eigen::Matrix4f>(object->NormalToWorld).isApprox(expected.Normal, 2e-5f));
            EXPECT_EQ(object->MotionValid, 0u);
            if (!expected.Source.GeometryReady) {
                EXPECT_EQ(actual.MeshBatchCount, 0u);
                EXPECT_EQ(snapshot.PrimitiveDrawBegin[index], snapshot.PrimitiveDrawBegin[index + 1]);
                continue;
            }
            ASSERT_LT(actual.FirstMeshBatch, snapshot.MeshBatches.size());
            const auto& batch = snapshot.MeshBatches[actual.FirstMeshBatch];
            EXPECT_EQ(batch.Primitive, index);
            EXPECT_EQ(actual.MeshBatchCount, 1u);
            EXPECT_EQ(batch.Geometry.Get(), ExpectedGeometry(expected));
            EXPECT_EQ(batch.FirstIndex, expected.Source.FirstIndex);
            EXPECT_EQ(batch.IndexCount, expected.Source.IndexCount);
            EXPECT_EQ(batch.VertexOffset, expected.Source.VertexOffset);
            EXPECT_EQ(batch.SectionIndex, 0u);
            ASSERT_LT(batch.Material, snapshot.Materials.size());
            const auto& material = snapshot.Materials[batch.Material];
            ASSERT_EQ(material.Passes.size(), 1u);
            EXPECT_EQ(material.Generation, expected.Material.Generation);
            EXPECT_EQ(material.Queue, expected.Material.Queue);
            EXPECT_TRUE(material.Passes[0].Valid);
            EXPECT_EQ(material.Passes[0].Program.Get(), Programs[expected.Material.Schema].get());
            EXPECT_EQ(material.Passes[0].NumericBytes, ReferenceBytes(expected.Material));
            EXPECT_EQ(material.Passes[0].PipelineState, expected.Material.State);
            const uint32_t begin = snapshot.PrimitiveDrawBegin[index], end = snapshot.PrimitiveDrawBegin[index + 1];
            ASSERT_LE(begin, end);
            ASSERT_LE(end, snapshot.DrawRecords.size());
            ASSERT_EQ(end - begin, 2u);
            array<bool, 2> seen{};
            for (uint32_t row = begin; row < end; ++row) {
                const auto& record = snapshot.DrawRecords[row];
                ASSERT_GE(record.Policy.Value, 5801u);
                ASSERT_LE(record.Policy.Value, 5802u);
                const auto policy = static_cast<uint32_t>(record.Policy.Value - 5801);
                EXPECT_FALSE(seen[policy]);
                seen[policy] = true;
                EXPECT_EQ(record.PrimitiveId, expected.Source.Id);
                EXPECT_EQ(record.Primitive, index);
                EXPECT_EQ(record.Batch, actual.FirstMeshBatch);
                EXPECT_EQ(record.Material, batch.Material);
                EXPECT_EQ(record.Queue, expected.Material.Queue);
                EXPECT_EQ(record.LayerMask, expected.Source.Layer);
                EXPECT_EQ(record.SectionIndex, 0u);
                EXPECT_NE(record.NormalStateId, 0u);
                EXPECT_NE(record.MirroredStateId, 0u);
                EXPECT_EQ(record.Status, DrawRecordStatus::Ready);
                EXPECT_EQ(record.Mirrored, expected.Mirrored);
                EXPECT_EQ(record.PolicyRevision, policy ? flight.PolicyRevision : 1);
                CheckDescription(record.Description, expected);
                EXPECT_EQ(record.Description.PipelineState, ReferenceState(expected, policy, flight.PolicyRevision, false));
                EXPECT_EQ(record.MirroredState, ReferenceState(expected, policy, flight.PolicyRevision, true));
                ASSERT_LT(record.BindingRecipe, snapshot.BindingRecipes.size());
                const auto& binding = snapshot.BindingRecipes[record.BindingRecipe];
                EXPECT_TRUE(binding.Valid);
                EXPECT_EQ(binding.GroupCount, 2u);
                EXPECT_EQ(binding.Groups[1], 1u);
                EXPECT_EQ(binding.Buffers[1], 0u);
                EXPECT_EQ(binding.GroupOrder[0], 1u);
                EXPECT_EQ(binding.Groups[2], 2u);
                EXPECT_EQ(binding.Buffers[2], 1u);
                EXPECT_EQ(binding.GroupOrder[1], 2u);
                ASSERT_LT(record.GeometryBindingPlan, snapshot.GeometryBindingPlans.size());
                EXPECT_EQ(snapshot.GeometryBindingPlans[record.GeometryBindingPlan].Runs, (InlineVector<CpuVertexBindingRun, 4>{{0, 1}}));
            }
        }
        for (uint32_t policy = 0; policy < flight.Lists.size() && !flight.LastExpected.empty(); ++policy) {
            const auto& list = flight.Lists[policy];
            ASSERT_TRUE(list.IsCurrent());
            ASSERT_EQ(list.GetDrawCount(), flight.LastExpected.size());
            for (uint32_t draw = 0; draw < flight.LastExpected.size(); ++draw) {
                const auto& expected = *flight.LastExpected[draw];
                CheckDescription(list.GetDescription(draw), expected);
                EXPECT_EQ(list.GetPipelineState(draw), ReferenceState(expected, policy % 2, flight.PolicyRevision, expected.Mirrored));
                const auto groups = list.GetGroups(draw);
                ASSERT_EQ(groups.size(), 2u);
                EXPECT_EQ(DecodeMaterial(groups[0].Set.Get(), groups[0].DynamicOffsets), ReferenceBytes(expected.Material));
                CheckObject(DecodeMaterial(groups[1].Set.Get(), groups[1].DynamicOffsets), expected, policy / 2);
            }
        }
    }

    void PrepareTemporal(uint32_t flightIndex, uint32_t iteration) {
        auto& flight = Flights[flightIndex];
        History.BeginFlight(flightIndex, flight.Epoch);
        ResolvedRenderViewFamily family{};
        family.OutputAvailable = true;
        family.RenderSize = {8, 8};
        family.OutputFormat = render::TextureFormat::RGBA8_UNORM;
        family.SampleCount = 1;
        for (uint32_t index = 0; index < flight.Views.size(); ++index) {
            auto& view = flight.Views[index];
            view = {};
            view.StateId = ViewIds[index];
            view.View = view.Projection = view.ViewProjection = Eigen::Matrix4f::Identity();
            view.CameraCut = iteration != 0 && iteration % (index == 0 ? 17 : 23) == 0;
            if (view.CameraCut) {
                HistoryValid[index] = false;
                ++CameraCuts;
            }
            History.Resolve(view, family);
            EXPECT_EQ(view.PreviousViewValid, HistoryValid[index]);
            ASSERT_TRUE(History.PreparePrimitiveHistory(view, *flight.Snapshot));
            for (auto& reference : flight.Reference) {
                auto& object = reference.Temporal[index];
                object = {};
                object.LocalToWorld = reference.Source.Transform;
                object.NormalToWorld = reference.Normal;
                object.PreviousLocalToWorld = reference.Source.Transform;
                if (!HistoryValid[index]) continue;
                const auto previous = std::find_if(HistoryModel[index].begin(), HistoryModel[index].end(), [&](const auto& old) {
                    return old.ProxyGeneration == reference.Source.ProxyGeneration && old.MotionEpoch == reference.Source.MotionEpoch;
                });
                if (previous == HistoryModel[index].end()) continue;
                object.PreviousLocalToWorld = previous->Transform;
                object.MotionValid = 1;
            }
        }
        for (const auto& reference : flight.Reference) {
            const auto& a = reference.Temporal[0];
            const auto& b = reference.Temporal[1];
            DivergentHistories += a.MotionValid != b.MotionValid ||
                                  !static_cast<Eigen::Matrix4f>(a.PreviousLocalToWorld).isApprox(static_cast<Eigen::Matrix4f>(b.PreviousLocalToWorld));
        }
    }

    void CommitTemporal(const OracleFlight& flight) {
        for (uint32_t index = 0; index < flight.Views.size(); ++index) {
            const auto before = History.GetPrimitiveCommittedSerial(ViewIds[index]);
            if (flight.Epoch % (index == 0 ? 7 : 3) == 0) {
                ++WithheldCommits;
                EXPECT_EQ(History.GetPrimitiveCommittedSerial(ViewIds[index]), before);
                continue;
            }
            ASSERT_TRUE(History.CommitViewWithHistory(ViewIds[index], {}));
            HistoryModel[index].clear();
            for (const auto& reference : flight.Reference) HistoryModel[index].push_back(reference.Source);
            HistoryValid[index] = true;
            EXPECT_EQ(History.GetPrimitiveCommittedSerial(ViewIds[index]), flight.Epoch);
        }
    }

    void CheckObject(std::span<const byte> bytes, const ReferencePrimitive& reference, uint32_t viewIndex) const {
        ASSERT_EQ(bytes.size(), sizeof(Forward_ObjectData));
        Forward_ObjectData actual;
        std::memcpy(&actual, bytes.data(), sizeof(actual));
        const auto& expected = reference.Temporal[viewIndex];
        EXPECT_EQ(actual.MotionValid, expected.MotionValid);
        EXPECT_TRUE(static_cast<Eigen::Matrix4f>(actual.LocalToWorld).isApprox(reference.Source.Transform, 1e-6f));
        EXPECT_TRUE(static_cast<Eigen::Matrix4f>(actual.NormalToWorld).isApprox(reference.Normal, 2e-5f));
        EXPECT_TRUE(static_cast<Eigen::Matrix4f>(actual.PreviousLocalToWorld).isApprox(static_cast<Eigen::Matrix4f>(expected.PreviousLocalToWorld), 1e-6f));
    }

    void CheckDescription(const MeshDrawDescription& description, const ReferencePrimitive& expected) const {
        EXPECT_EQ(description.Program.Get(), Programs[expected.Material.Schema].get());
        EXPECT_EQ(description.Geometry.Get(), ExpectedGeometry(expected));
        ASSERT_TRUE(description.Geometry);
        // Re-read old flight geometry after Pump as well as current geometry. The expected
        // ranges come from authoring, so pointer equality alone cannot hide a freed asset.
        ASSERT_EQ(description.Geometry->VertexBuffers.size(), 1u);
        const auto& vertex = description.Geometry->VertexBuffers[0];
        EXPECT_EQ(vertex.Binding, 0u);
        EXPECT_EQ(vertex.View.Target, Vertices.get());
        EXPECT_EQ(vertex.View.Offset, expected.Source.Geometry * 16u);
        EXPECT_EQ(vertex.View.Size, 192u);
        EXPECT_EQ(description.Geometry->Ibv.Target, Indices.get());
        EXPECT_EQ(description.Geometry->Ibv.Offset, expected.Source.Geometry * 4u);
        EXPECT_EQ(description.Geometry->Ibv.Stride, 4u);
        EXPECT_EQ(description.FirstIndex, expected.Source.FirstIndex);
        EXPECT_EQ(description.IndexCount, expected.Source.IndexCount);
        EXPECT_EQ(description.VertexOffset, expected.Source.VertexOffset);
        EXPECT_TRUE(description.LayoutId.IsValid());
    }

    const GpuMesh::DrawData* ExpectedGeometry(const ReferencePrimitive& expected) const {
        return &(expected.Source.GeometryOwner ? *expected.Source.GeometryOwner.Get() : Geometry)[expected.Source.Geometry];
    }

    vector<const ReferencePrimitive*> VisibleReference(const OracleFlight& flight, const ResolvedRenderView& view, RendererListSorting sorting, bool requireGeometry = true) const {
        vector<const ReferencePrimitive*> result;
        for (const auto& expected : flight.Reference) {
            if (requireGeometry && !expected.Source.GeometryReady) continue;
            if (!(expected.Source.Layer & view.LayerMask)) continue;
            const auto& bounds = expected.Bounds;
            const bool inside = bounds.Max.x() >= -1 && bounds.Min.x() <= 1 && bounds.Max.y() >= -1 && bounds.Min.y() <= 1 && bounds.Max.z() >= 0 && bounds.Min.z() <= 1;
            if (!expected.Source.DisableCulling && !inside) continue;
            result.push_back(&expected);
        }
        std::sort(result.begin(), result.end(), [&](const auto* a, const auto* b) {
            if (a == b) return false;
            if (a->Material.Queue != b->Material.Queue) return a->Material.Queue < b->Material.Queue;
            if (sorting == RendererListSorting::StateThenFrontToBack) {
                if (a->Material.Schema != b->Material.Schema) return a->Material.Schema < b->Material.Schema;
                if (a->Source.Material != b->Source.Material) return a->Source.Material < b->Source.Material;
            }
            const float az = (a->Bounds.Min.z() + a->Bounds.Max.z()) * .5f;
            const float bz = (b->Bounds.Min.z() + b->Bounds.Max.z()) * .5f;
            if (az != bz) return sorting == RendererListSorting::BackToFront ? az > bz : az < bz;
            // Equal depths are deliberately confined to permanent anchors; their registration
            // order never changes and is known without reading the incremental snapshot.
            EXPECT_LT(a->Source.AuthoringSlot, kOracleMaterials);
            EXPECT_LT(b->Source.AuthoringSlot, kOracleMaterials);
            return a->Source.AuthoringSlot < b->Source.AuthoringSlot;
        });
        return result;
    }

    void CheckTrace(const OracleEncoder& trace, std::span<const ReferencePrimitive* const> expected, uint32_t policy, uint64_t revision) const {
        EXPECT_FALSE(trace.Unexpected);
        ASSERT_EQ(trace.Draws.size(), expected.size());
        for (size_t index = 0; index < expected.size(); ++index) {
            const auto& draw = trace.Draws[index];
            const auto& reference = *expected[index];
            EXPECT_EQ(draw.Vertex.Get(), Vertices.get());
            EXPECT_EQ(draw.VertexOffset, reference.Source.Geometry * 16u);
            EXPECT_EQ(draw.VertexSize, 192u);
            EXPECT_EQ(draw.Index.Get(), Indices.get());
            EXPECT_EQ(draw.IndexOffset, reference.Source.Geometry * 4u);
            EXPECT_EQ(draw.IndexStride, 4u);
            EXPECT_EQ(draw.Arguments, (array<int64_t, 5>{reference.Source.IndexCount, 1, reference.Source.FirstIndex, reference.Source.VertexOffset, 0}));
            EXPECT_EQ(draw.Material, ReferenceBytes(reference.Material));
            const auto pso = std::find_if(Capture.Pipelines.begin(), Capture.Pipelines.end(), [&](const auto& value) { return value.Native == draw.Pipeline.Get(); });
            ASSERT_NE(pso, Capture.Pipelines.end());
            const auto state = ReferenceState(reference, policy % 2, revision, reference.Mirrored);
            CheckObject(draw.Object, reference, policy / 2);
            EXPECT_EQ(pso->Face, state.Primitive.FaceClockwise);
            EXPECT_EQ(pso->Cull, state.Primitive.Cull);
            EXPECT_EQ(pso->WriteMask, state.WriteMask);
            EXPECT_EQ(pso->VertexStride, reference.Source.Geometry ? 16u : 12u);
            EXPECT_EQ(pso->Layout, Programs[reference.Material.Schema]->GetPipelineLayout());
        }
    }

    render::Device& Native;
    OracleCaptureDevice Capture;
    render::RenderPassRegistry HistoryRegistry;
    ViewStateRegistry History;
    array<ViewStateId, 2> ViewIds{AllocateViewStateId(), AllocateViewStateId()};
    array<vector<AuthoringPrimitive>, 2> HistoryModel;
    array<bool, 2> HistoryValid{};
    uint64_t CameraCuts{0}, WithheldCommits{0}, DivergentHistories{0};
    array<unique_ptr<ShaderProgram>, 2> Programs;
    array<unique_ptr<MaterialTechnique>, 2> Techniques;
    array<unique_ptr<Material>, kOracleMaterials> Materials;
    unique_ptr<render::Buffer> Vertices, Indices;
    array<GpuMesh::DrawData, 2> Geometry;
    array<AuthoringMaterial, kOracleMaterials> MaterialModel;
    array<AuthoringPrimitive, kOracleSlots> Models;
    array<Nullable<OraclePrimitive*>, kOracleSlots> Proxies;
    // Pending coroutine resumes before its manager is destroyed; proxies and flight owners
    // release before the manager, including on an early assertion return.
    AssetManager Assets;
    OracleLoadGate LoadGate;
    StreamingAssetRef<OracleGeometryAsset> Stream;
    shared_ptr<uint32_t> DestroyedGeometry{make_shared<uint32_t>(0)};
    Scene Source;
    array<OracleFlight, 3> Flights;
    OracleRandom Random;
    array<uint32_t, 12> Operations{};
    uint64_t PolicyRevision{1};
    uint32_t StreamingStarts{0}, StreamingReady{0}, StreamingFailed{0}, StreamingRemovedPending{0};
    bool Initialized{false};
};

class SceneIncrementalOracleTest : public test::FoundationGraphGpuTest {};

TEST_P(SceneIncrementalOracleTest, RandomAuthoringRebuildMatchesThreeFlightsCullSortReadyAndEncoder) {
    OracleWorld world{*Context.Device};
    world.Initialize();
    ASSERT_TRUE(world.Initialized);
    // Graph preparation and shader programs share the wrapper identity. Graph attachments
    // remain native; only material uploads and parameter sets go to the trace encoder.
    render::RenderPassRegistry oracleRegistry{&world.Capture};
    RenderGraphFrameResources oracleResources{world.Capture, oracleRegistry};
    for (const auto& program : world.Programs) ASSERT_EQ(program->GetDevice(), &world.Capture);
    array<byte, 352> geometryBytes{};
    auto upload = render::test::MakeUploadBuffer(*Context.Device, geometryBytes, render::BufferUse::CopySource);
    auto readback = Context.Device->CreateBuffer({geometryBytes.size(), render::MemoryType::ReadBack, render::BufferUse::CopyDestination | render::BufferUse::MapRead, {}});
    ASSERT_TRUE(upload && readback);
    // External state survives graph instances; every successful run below waits for submission.
    RenderExternalBuffer vertexExternal{world.Vertices.get(), world.Vertices->GetDesc(), render::BufferState::Undefined};
    RenderExternalBuffer indexExternal{world.Indices.get(), world.Indices->GetDesc(), render::BufferState::Undefined};
    RenderExternalBuffer readbackExternal{readback.Get(), readback->GetDesc(), render::BufferState::CopyDestination};
    uint64_t checkedDraws = 0;
    array<uint32_t, 3> flightVisits{};
    array<bool, 2> schemasSeen{}, geometrySeen{}, mirroredSeen{}, queuesSeen{};
    uint64_t culledPrimitives = 0;
    array<uint32_t, 3> sortVisits{};
    uint64_t tiedDepthPairs = 0;
    for (uint32_t iteration = 0; iteration < kOracleEpochs; ++iteration) {
        SCOPED_TRACE(fmt::format("seed={} epoch={} rng={}", kOracleSeed, iteration + 1, world.Random.State));
        world.Mutate(iteration);
        world.AdvanceStreaming(iteration);
        ASSERT_FALSE(HasFatalFailure());
        // Irregular reuse leaves other publications frozen across multiple authoring epochs.
        const uint32_t flightIndex = iteration < 3 ? iteration : world.Random.Next(3);
        ++flightVisits[flightIndex];
        world.Publish(iteration + 1, flightIndex);
        ASSERT_FALSE(HasFatalFailure());
        EXPECT_LE(world.Assets.GetAssetCount(), 4u);
        if (iteration % 24 >= 5) EXPECT_EQ(world.Source.GetCommitStats().PendingResourcesObserved, 0u);
        world.PrepareTemporal(flightIndex, iteration);
        ASSERT_FALSE(HasFatalFailure());
        for (const auto& previous : world.Flights) world.CheckSnapshot(previous);
        ASSERT_FALSE(HasFatalFailure());
        auto& flight = world.Flights[flightIndex];
        ResolvedRenderView view{};
        view.View = view.Projection = view.ViewProjection = view.PreviousViewProjection = Eigen::Matrix4f::Identity();
        view.WorldPosition = Eigen::Vector3f::Zero();
        view.LayerMask = iteration % 3 ? 1u : 3u;
        CullingResults culling;
        ASSERT_TRUE(Cull({flight.Snapshot.get(), &view}, culling));
        const auto sorting = static_cast<RendererListSorting>(iteration % 3);
        ++sortVisits[iteration % 3];
        auto expected = world.VisibleReference(flight, view, sorting);
        for (size_t index = 1; index < expected.size(); ++index)
            tiedDepthPairs += expected[index - 1]->Material.Queue == expected[index]->Material.Queue &&
                              (expected[index - 1]->Bounds.Min.z() + expected[index - 1]->Bounds.Max.z()) ==
                                  (expected[index]->Bounds.Min.z() + expected[index]->Bounds.Max.z());
        ASSERT_FALSE(expected.empty());
        flight.LastExpected = expected;
        culledPrimitives += flight.Reference.size() - expected.size();
        for (const auto* value : expected) {
            schemasSeen[value->Material.Schema] = true;
            geometrySeen[value->Source.Geometry] = true;
            mirroredSeen[value->Mirrored ? 1 : 0] = true;
            queuesSeen[value->Material.Queue == RenderQueue::Transparent ? 1 : 0] = true;
        }
        const auto expectedVisible = world.VisibleReference(flight, view, sorting, false);
        ASSERT_EQ(culling.Primitives.size(), expectedVisible.size());
        for (const auto* value : expectedVisible) {
            const auto visible = std::find_if(culling.Primitives.begin(), culling.Primitives.end(), [&](const auto& actual) { return flight.Snapshot->Primitives[actual.Primitive].Id == value->Source.Id; });
            ASSERT_NE(visible, culling.Primitives.end());
            EXPECT_NEAR(visible->ViewDepth, (value->Bounds.Min.z() + value->Bounds.Max.z()) * .5f, 1e-6f);
        }
        OracleProcessor processor{*flight.Groups, flight.Objects.Rows(), world.History};
        for (uint32_t policy = 0; policy < flight.Lists.size(); ++policy) {
            auto viewCulling = culling;
            viewCulling.View = &flight.Views[policy / 2];
            RendererListDesc desc{"oracle", "Oracle", &viewCulling, &flight.Views[policy / 2]};
            desc.Policy = {5801u + policy % 2};
            desc.Sorting = sorting;
            desc.RequireMaterialPass = true;
            ASSERT_TRUE(BuildRendererList(desc, processor, flight.Lists[policy]));
            const auto& list = flight.Lists[policy];
            ASSERT_TRUE(list.Stats.ContentSucceeded());
            EXPECT_TRUE(list.Commands.empty());
            ASSERT_EQ(list.GetDrawCount(), expected.size());
            for (uint32_t draw = 0; draw < expected.size(); ++draw) {
                world.CheckDescription(list.GetDescription(draw), *expected[draw]);
                EXPECT_EQ(flight.Snapshot->Primitives[list.Items[draw].SortData.Primitive].Id, expected[draw]->Source.Id);
                EXPECT_EQ(list.GetPipelineState(draw), ReferenceState(*expected[draw], policy % 2, flight.PolicyRevision, expected[draw]->Mirrored));
                const auto groups = list.GetGroups(draw);
                ASSERT_EQ(groups.size(), 2u);
                EXPECT_EQ(DecodeMaterial(groups[0].Set.Get(), groups[0].DynamicOffsets), ReferenceBytes(expected[draw]->Material));
                world.CheckObject(DecodeMaterial(groups[1].Set.Get(), groups[1].DynamicOffsets), *expected[draw], policy / 2);
            }
        }
        EXPECT_EQ(processor.DynamicFallbacks, 0u);
        flight.Writes.Flush(world.Capture);
        for (uint32_t index = 0; index < geometryBytes.size(); ++index)
            geometryBytes[index] = static_cast<byte>((iteration * 17 + index * 3) & 255);
        auto* mapped = upload->Map(0, geometryBytes.size());
        ASSERT_NE(mapped, nullptr);
        std::memcpy(mapped, geometryBytes.data(), geometryBytes.size());
        upload->FlushMappedRange({0, geometryBytes.size()});
        upload->Unmap();
        auto expectedGeometry = geometryBytes;
        std::copy_n(geometryBytes.begin() + 64, 32, expectedGeometry.begin() + 16);
        for (uint32_t accessCase = 0; accessCase < (iteration + 1 == kOracleEpochs ? 3u : 1u); ++accessCase) {
            Writes.Reset();
            oracleResources.BeginFlight(uint64_t{iteration} * 3 + accessCase + 1, Writes);
            RenderExternalBuffer uploadExternal{upload.Get(), upload->GetDesc(), render::BufferState::HostWrite, true};
            RenderGraph graph{world.Capture, oracleResources, oracleRegistry, "incremental oracle Ready"};
            auto vertices = graph.NextVersion(graph.ImportBuffer(vertexExternal, "oracle geometry vertices", RenderGraphExternalAccess::ReadWrite));
            const auto indices = graph.NextVersion(graph.ImportBuffer(indexExternal, "oracle geometry indices", RenderGraphExternalAccess::ReadWrite));
            const auto source = graph.ImportBuffer(uploadExternal, "oracle geometry upload", RenderGraphExternalAccess::ReadOnly);
            graph.AddCopyBufferPass("write vertices", source, vertices, 256);
            graph.AddCopyBufferPass("write indices", source, indices, 96, 256);
            vertices = graph.NextVersion(vertices);
            graph.AddCopyBufferPass("patch overlapping vertices", source, vertices, 32, 64, 16);
            auto color = graph.CreateTexture({render::TextureDimension::Dim2D, 8, 8, 1, 1, 1, render::TextureFormat::RGBA8_UNORM, render::MemoryType::Device, render::TextureUse::RenderTarget, {}}, "oracle color");
            struct Payload {
                OracleWorld* World;
                OracleFlight* Flight;
                vector<const ReferencePrimitive*> Expected;
                uint32_t Policy;
                std::optional<PreparedRendererList> Ready;
            };
            for (uint32_t policy = 0; policy < flight.Lists.size(); ++policy) {
                if (policy != 0) color = graph.NextVersion(color);
                graph.AddRasterPass<Payload>("oracle Ready trace", [&](Payload& data, RenderGraphRasterBuilder& builder) {
                data.World = &world; data.Flight = &flight; data.Expected = expected; data.Policy = policy;
                // Policy one retains both imports even when policy zero omits a declaration,
                // so the negative case cannot pass through the culled-import exemption.
                if (policy != 0 || accessCase != 1) builder.ReadBuffer(vertices, RgBufferAccess::Vertex);
                if (policy != 0 || accessCase != 2) builder.ReadBuffer(indices, RgBufferAccess::Index);
                builder.SetColorAttachment(0, color); builder.SetSideEffect(); }, +[](Payload& data, RenderGraphPrepareContext& context) {
                data.Ready = PrepareRendererList(data.Flight->Lists[data.Policy], context);
                return data.Ready.has_value(); }, +[](const Payload& data, RenderGraphRasterContext& context) {
                OracleEncoder trace{*RenderGraphTestDriver::NativeEncoder(context).GetCommandBuffer()};
                DrawExecutionStats stats;
                RenderGraphTestDriver::WithEncoder(context, trace, [&](RenderGraphRasterContext& traced) { RecordRendererList(*data.Ready, traced, stats); });
                EXPECT_TRUE(stats.Succeeded());
                EXPECT_EQ(stats.Draws, data.Expected.size());
                data.World->CheckTrace(trace, data.Expected, data.Policy, data.Flight->PolicyRevision); });
            }
            auto host = graph.NextVersion(graph.ImportBuffer(readbackExternal, "oracle geometry readback", RenderGraphExternalAccess::ObservableOutput));
            graph.AddCopyBufferPass("read vertices", vertices, host, 256);
            host = graph.NextVersion(host);
            graph.AddCopyBufferPass("read indices", indices, host, 96, 0, 256);
            HostRead(graph, host);
            if (accessCase != 0) {
                EXPECT_FALSE(Run(graph));
                EXPECT_EQ(graph.GetFirstErrorCode(), "UndeclaredGeometryRead");
                EXPECT_EQ(graph.GetReport().CommandCalls.DrawIndexed, 0u);
                continue;
            }
            ASSERT_TRUE(Run(graph)) << graph.GetReport().ToText();
            EXPECT_EQ(graph.GetReport().CommandCalls.DrawIndexed, expected.size() * flight.Lists.size());
            EXPECT_EQ(graph.GetReport().GeometryValidationCalls, 2 * flight.Lists.size());
            if (iteration > 1) EXPECT_TRUE(graph.GetReport().CompilePlanReused);
            const auto actualGeometry = Read(*readback);
            EXPECT_EQ(actualGeometry, (vector<byte>{expectedGeometry.begin(), expectedGeometry.end()}));
            for (const auto [resource, access] : {std::pair{vertices.Index, render::BufferState::Vertex},
                                                  std::pair{indices.Index, render::BufferState::Index}}) {
                EXPECT_TRUE(std::any_of(graph.GetReport().Barriers.begin(), graph.GetReport().Barriers.end(), [&](const auto& barrier) {
                    return barrier.Resource == resource && (barrier.Before & uint32_t(render::BufferState::CopyDestination)) != 0 &&
                           (barrier.After & uint32_t(access)) != 0;
                })) << graph.GetReport().ToText();
            }
            checkedDraws += expected.size() * flight.Lists.size();
        }
        world.CommitTemporal(flight);
    }
    EXPECT_GT(checkedDraws, kOracleEpochs * 2u);
    EXPECT_GT(culledPrimitives, 0u);
    for (const auto& coverage : {schemasSeen, geometrySeen, mirroredSeen, queuesSeen})
        for (const bool seen : coverage) EXPECT_TRUE(seen);
    for (const auto visits : flightVisits) EXPECT_GT(visits, 50u);
    for (const auto visits : sortVisits) EXPECT_EQ(visits, kOracleEpochs / sortVisits.size());
    EXPECT_GT(tiedDepthPairs, 0u);
    EXPECT_GT(world.CameraCuts, 0u);
    EXPECT_GT(world.WithheldCommits, 0u);
    EXPECT_GT(world.DivergentHistories, 0u);
    EXPECT_EQ(world.StreamingStarts, 16u);
    EXPECT_EQ(world.StreamingReady, 11u);
    EXPECT_EQ(world.StreamingFailed, 5u);
    EXPECT_EQ(world.StreamingRemovedPending, 5u);
    world.Source.RemovePrimitive(world.Proxies.back().Get());
    world.Proxies.back() = nullptr;
    world.Stream.Reset();
    for (auto& flight : world.Flights) {
        for (auto& list : flight.Lists) list.ResetForReuse();
        flight.LastExpected.clear();
        flight.Reference.clear();
        flight.Snapshot.reset();
        flight.Owners.clear();
    }
    world.Assets.Pump();
    EXPECT_EQ(world.Assets.GetAssetCount(), 0u);
    EXPECT_EQ(*world.DestroyedGeometry, world.StreamingStarts);
    for (const auto operations : world.Operations) EXPECT_EQ(operations, kOracleEpochs / world.Operations.size());
    fmt::print("SCENE_ORACLE seed={} epochs={} flights=3 views=2 checked_draws={} validation=Full reference=authoring_rebuild sort=all_three tied_depth_pairs={} streaming=ready_faulted_remove_pending ready_loads={} failed_loads={} removed_pending={} history=ready_and_encoder camera_cuts={} withheld_history_commits={} divergent_history_rows={} graph_geometry_access=both_policies_and_missing_vertex_index graph_geometry_writers=native_copy_overlap_readback_and_barriers sanitizer=external_run_required\n", kOracleSeed, kOracleEpochs, checkedDraws, tiedDepthPairs, world.StreamingReady, world.StreamingFailed, world.StreamingRemovedPending, world.CameraCuts, world.WithheldCommits, world.DivergentHistories);
}

INSTANTIATE_TEST_SUITE_P(Backends, SceneIncrementalOracleTest, testing::Values(render::RenderBackend::D3D12, render::RenderBackend::Vulkan));

}  // namespace
}  // namespace radray
