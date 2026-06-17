#include <radray/runtime/application.h>
#include <radray/runtime/imgui_system.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/window_manager.h>
#include <radray/runtime/renderer/render_context.h>
#include <radray/runtime/renderer/render_pass.h>
#include <radray/runtime/renderer/render_pipeline.h>
#include <radray/runtime/renderer/render_resource_pool.h>
#include <radray/render/common.h>
#include <radray/render/gpu_resource.h>
#include <radray/basic_math.h>
#include <radray/camera_control.h>
#include <radray/hash.h>
#include <radray/image_data.h>
#include <radray/logger.h>
#include <radray/scope_guard.h>
#include <radray/types.h>
#include <radray/window/native_window.h>

#ifdef RADRAY_PLATFORM_WINDOWS
#include <radray/platform/win32_headers.h>
#endif

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numbers>
#include <mutex>
#include <sstream>
#include <span>
#include <string_view>

#ifndef RADRAY_GLTF_VIEWER_ASSET_DIR
#define RADRAY_GLTF_VIEWER_ASSET_DIR "."
#endif

using namespace radray;

namespace {

enum class GltfAlphaMode : uint32_t {
    Opaque = 0,
    Mask = 1,
    Blend = 2,
    Transmission = 3,
};

enum class GltfDebugMode : uint32_t {
    Shaded = 0,
    GeometryNormal = 1,
    UV = 2,
    White = 3,
    NormalTexture = 4,
    ShadingNormal = 5,
};

struct GltfVertex {
    Eigen::Vector3f Position{Eigen::Vector3f::Zero()};
    Eigen::Vector3f Normal{Eigen::Vector3f::UnitY()};
    Eigen::Vector4f Tangent{1.0f, 0.0f, 0.0f, 1.0f};
    Eigen::Vector2f TexCoord{Eigen::Vector2f::Zero()};
};

struct GltfMaterialCpu {
    string Name{"Default"};
    Eigen::Vector4f BaseColorFactor{1.0f, 1.0f, 1.0f, 1.0f};
    Eigen::Vector3f EmissiveFactor{0.0f, 0.0f, 0.0f};
    float AlphaCutoff{0.5f};
    float Metallic{0.0f};
    float Roughness{0.5f};
    float Specular{0.5f};
    float SpecularTint{0.0f};
    float Anisotropic{0.0f};
    float Sheen{0.0f};
    float SheenTint{0.0f};
    float Flatness{0.0f};
    float Clearcoat{0.0f};
    float ClearcoatGloss{0.0f};
    float SpecTrans{0.0f};
    float Eta{1.5f};
    float NormalScale{1.0f};
    int BaseColorTexture{-1};
    int MetallicRoughnessTexture{-1};
    int NormalTexture{-1};
    GltfAlphaMode AlphaMode{GltfAlphaMode::Opaque};
};

struct GltfMaterialConstants {
    float BaseColorFactor[4];
    float EmissiveFactorAlphaCutoff[4];
    float Principled0[4];
    float Principled1[4];
    float Principled2[4];
    float NormalParams[4];
    uint32_t Flags[4];
};

struct GltfTextureCpu {
    string Name;
    ImageData Image;
    render::SamplerDescriptor Sampler{};
};

struct GltfPrimitiveCpu {
    string Name;
    vector<GltfVertex> Vertices;
    vector<uint32_t> Indices;
    uint32_t MaterialIndex{0};
};

struct GltfNodeInfo {
    string Name;
    int Parent{-1};
    vector<int> Children;
    bool HasMesh{false};
};

struct GltfPrimitiveGpu {
    unique_ptr<render::Buffer> VertexBuffer;
    unique_ptr<render::Buffer> IndexBuffer;
    render::VertexBufferView Vbv;
    render::IndexBufferView Ibv;
    uint32_t IndexCount{0};
    uint32_t MaterialIndex{0};
};

struct GltfTextureGpu {
    unique_ptr<render::Texture> Texture;
    unique_ptr<render::TextureView> View;
    unique_ptr<render::Sampler> Sampler;
};

struct GltfMaterialGpu {
    unique_ptr<render::Buffer> Constants;
    unique_ptr<render::DescriptorSet> DescriptorSet;
};

struct GltfModel {
    string Path;
    vector<GltfPrimitiveCpu> CpuPrimitives;
    vector<GltfMaterialCpu> CpuMaterials;
    vector<GltfTextureCpu> CpuTextures;
    vector<GltfNodeInfo> Nodes;
    vector<int> RootNodes;

    vector<GltfPrimitiveGpu> GpuPrimitives;
    vector<GltfMaterialGpu> GpuMaterials;
    vector<GltfTextureGpu> GpuTextures;
    unique_ptr<render::GraphicsPipelineState> Pso;

    Eigen::Vector3f BoundsMin{Eigen::Vector3f::Zero()};
    Eigen::Vector3f BoundsMax{Eigen::Vector3f::Zero()};
    bool HasBounds{false};
    bool Uploaded{false};
    bool UploadFailed{false};
    bool UploadFailureLogged{false};
    bool DrawSummaryLogged{false};
};

struct ViewerSceneView {
    Eigen::Vector3f Eye{Eigen::Vector3f::Zero()};
    Eigen::Matrix4f View{Eigen::Matrix4f::Identity()};
    Eigen::Matrix4f Proj{Eigen::Matrix4f::Identity()};
    Eigen::Matrix4f ViewProj{Eigen::Matrix4f::Identity()};
};

struct GltfSceneConstants {
    float Mvp[16];
    float CameraPosition[4];
    uint32_t Debug[4];
};

class GltfScenePass : public RenderPass {
public:
    GltfScenePass(render::TextureFormat depthFormat, render::ColorClearValue clearColor)
        : _depthFormat(depthFormat), _clearColor(clearColor) {}

    std::string_view GetName() const noexcept override { return "GltfScenePass"; }

    void Execute(RenderContext& ctx) override {
        // 帧资源全部从 RenderContext 标准字段读取(runtime 不再提供 UserData);
        // viewer 专属状态(model/view/debug)由 App 经 SetFrameData 每帧注入到 pass 成员。
        if (ctx.Device == nullptr || ctx.CmdBuffer == nullptr ||
            ctx.ColorTarget == nullptr || ctx.Width == 0 || ctx.Height == 0 ||
            ctx.Resources == nullptr) {
            return;
        }

        render::TextureDescriptor depthDesc{
            .Dim = render::TextureDimension::Dim2D,
            .Width = ctx.Width,
            .Height = ctx.Height,
            .DepthOrArraySize = 1,
            .MipLevels = 1,
            .SampleCount = 1,
            .Format = _depthFormat,
            .Memory = render::MemoryType::Device,
            .Usage = render::TextureUse::DepthStencilWrite,
            .Hints = render::ResourceHint::None};
        if (ctx.Resources->Acquire("GltfSceneDepth", ctx.FlightIndex, depthDesc, *ctx.Device) == nullptr) {
            return;
        }
        ctx.Resources->Transition("GltfSceneDepth", ctx.FlightIndex, render::TextureState::DepthWrite, *ctx.CmdBuffer);

        render::TextureViewDescriptor depthViewDesc{
            .Dim = render::TextureDimension::Dim2D,
            .Format = _depthFormat,
            .Range = render::SubresourceRange::AllSub(),
            .Usage = render::TextureViewUsage::DepthWrite};
        render::TextureView* depthView = ctx.Resources->GetView("GltfSceneDepth", ctx.FlightIndex, depthViewDesc, *ctx.Device);
        if (depthView == nullptr) {
            return;
        }

        render::ColorAttachment colorAttachment{
            .Target = ctx.ColorTarget,
            .Load = render::LoadAction::Clear,
            .Store = render::StoreAction::Store,
            .ClearValue = _clearColor};
        render::DepthStencilAttachment depthAttachment{
            .Target = depthView,
            .DepthLoad = render::LoadAction::Clear,
            .DepthStore = render::StoreAction::Store,
            .StencilLoad = render::LoadAction::DontCare,
            .StencilStore = render::StoreAction::Discard,
            .ClearValue = render::DepthStencilClearValue{1.0f, uint8_t{0}}};
        render::RenderPassDescriptor passDesc{
            .ColorAttachments = std::span{&colorAttachment, 1},
            .DepthStencilAttachment = depthAttachment,
            .Name = "glTF Scene"};
        auto encoderOpt = ctx.CmdBuffer->BeginRenderPass(passDesc);
        if (!encoderOpt.HasValue()) {
            RADRAY_ERR_LOG("failed to begin glTF render pass");
            return;
        }
        auto encoder = encoderOpt.Release();

        Viewport vp{0.0f, 0.0f, static_cast<float>(ctx.Width), static_cast<float>(ctx.Height), 0.0f, 1.0f};
        if (ctx.Device->GetBackend() == render::RenderBackend::Vulkan) {
            vp.Y = static_cast<float>(ctx.Height);
            vp.Height = -static_cast<float>(ctx.Height);
        }
        encoder->SetViewport(vp);
        encoder->SetScissor(Rect{0, 0, ctx.Width, ctx.Height});

        if (_model == nullptr || !_model->Uploaded || _view == nullptr) {
            ctx.CmdBuffer->EndRenderPass(std::move(encoder));
            return;
        }

        GltfModel& model = *_model;
        if (_rootSig == nullptr || model.Pso == nullptr) {
            ctx.CmdBuffer->EndRenderPass(std::move(encoder));
            return;
        }
        encoder->BindRootSignature(_rootSig);
        encoder->BindGraphicsPipelineState(model.Pso.get());

        auto sceneParam = _rootSig->FindParameterId("gScene");
        if (!sceneParam.has_value()) {
            ctx.CmdBuffer->EndRenderPass(std::move(encoder));
            return;
        }
        GltfSceneConstants sceneConstants{};
        std::memcpy(sceneConstants.Mvp, _view->ViewProj.data(), sizeof(sceneConstants.Mvp));
        sceneConstants.CameraPosition[0] = _view->Eye.x();
        sceneConstants.CameraPosition[1] = _view->Eye.y();
        sceneConstants.CameraPosition[2] = _view->Eye.z();
        sceneConstants.CameraPosition[3] = 1.0f;
        sceneConstants.Debug[0] = _debugMode;
        uint32_t drawCount = 0;
        uint32_t indexCount = 0;
        for (const GltfPrimitiveGpu& primitive : model.GpuPrimitives) {
            if (primitive.IndexCount == 0 || primitive.MaterialIndex >= model.GpuMaterials.size()) {
                continue;
            }
            encoder->BindDescriptorSet(render::DescriptorSetIndex{1}, model.GpuMaterials[primitive.MaterialIndex].DescriptorSet.get());
            encoder->PushConstants(sceneParam.value(), &sceneConstants, sizeof(sceneConstants));
            encoder->BindVertexBuffer(std::span{&primitive.Vbv, 1});
            encoder->BindIndexBuffer(primitive.Ibv);
            encoder->DrawIndexed(primitive.IndexCount, 1, 0, 0, 0);
            ++drawCount;
            indexCount += primitive.IndexCount;
        }
        if (!model.DrawSummaryLogged) {
            RADRAY_INFO_LOG("glTF viewer drew {} primitives, {} indices, debugMode={}", drawCount, indexCount, _debugMode);
            model.DrawSummaryLogged = true;
        }

        ctx.CmdBuffer->EndRenderPass(std::move(encoder));
    }

    void SetRootSignature(render::RootSignature* rootSig) noexcept { _rootSig = rootSig; }

    // 每帧由 App 注入:pass 从 RenderContext 标准字段拿通用帧资源,这里只补充 viewer 专属状态。
    void SetFrameData(GltfModel* model, const ViewerSceneView* view, uint32_t debugMode) noexcept {
        _model = model;
        _view = view;
        _debugMode = debugMode;
    }

private:
    render::RootSignature* _rootSig{nullptr};
    render::TextureFormat _depthFormat;
    render::ColorClearValue _clearColor;
    GltfModel* _model{nullptr};
    const ViewerSceneView* _view{nullptr};
    uint32_t _debugMode{0};
};

Eigen::Matrix4f CgltfMatrixToEigen(const cgltf_float* m) {
    Eigen::Matrix4f out;
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            out(row, col) = m[col * 4 + row];
        }
    }
    return out;
}

render::SamplerDescriptor DefaultSampler() {
    render::SamplerDescriptor desc{};
    desc.AddressS = render::AddressMode::Repeat;
    desc.AddressT = render::AddressMode::Repeat;
    desc.AddressR = render::AddressMode::Repeat;
    desc.MinFilter = render::FilterMode::Linear;
    desc.MagFilter = render::FilterMode::Linear;
    desc.MipmapFilter = render::FilterMode::Linear;
    desc.LodMin = 0.0f;
    desc.LodMax = 0.0f;
    desc.Compare = std::nullopt;
    desc.AnisotropyClamp = 1;
    return desc;
}

render::AddressMode ToAddressMode(cgltf_wrap_mode mode) {
    switch (mode) {
        case cgltf_wrap_mode_clamp_to_edge: return render::AddressMode::ClampToEdge;
        case cgltf_wrap_mode_mirrored_repeat: return render::AddressMode::Mirror;
        case cgltf_wrap_mode_repeat:
        default: return render::AddressMode::Repeat;
    }
}

render::FilterMode ToFilterMode(cgltf_filter_type mode) {
    switch (mode) {
        case cgltf_filter_type_nearest:
        case cgltf_filter_type_nearest_mipmap_nearest:
        case cgltf_filter_type_nearest_mipmap_linear:
            return render::FilterMode::Nearest;
        default:
            return render::FilterMode::Linear;
    }
}

void EnsureDefaultMaterial(GltfModel& model) {
    if (!model.CpuMaterials.empty()) {
        return;
    }
    model.CpuMaterials.push_back(GltfMaterialCpu{});
}

ImageData MakeSolidImage(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    ImageData img;
    img.Width = 1;
    img.Height = 1;
    img.Format = ImageFormat::RGBA8_BYTE;
    img.Data = make_unique<byte[]>(4);
    img.Data[0] = static_cast<byte>(r);
    img.Data[1] = static_cast<byte>(g);
    img.Data[2] = static_cast<byte>(b);
    img.Data[3] = static_cast<byte>(a);
    return img;
}

ImageData ConvertToRGBA8(const ImageData& src) {
    if (src.Format == ImageFormat::RGBA8_BYTE) {
        return src;
    }
    if (src.Format == ImageFormat::RGB8_BYTE) {
        return src.RGB8ToRGBA8(0xff);
    }
    if (src.Format == ImageFormat::R8_BYTE) {
        ImageData out;
        out.Width = src.Width;
        out.Height = src.Height;
        out.Format = ImageFormat::RGBA8_BYTE;
        out.Data = make_unique<byte[]>(out.GetSize());
        const size_t count = static_cast<size_t>(src.Width) * src.Height;
        for (size_t i = 0; i < count; ++i) {
            byte v = src.Data[i];
            out.Data[i * 4 + 0] = v;
            out.Data[i * 4 + 1] = v;
            out.Data[i * 4 + 2] = v;
            out.Data[i * 4 + 3] = byte{0xff};
        }
        return out;
    }
    return MakeSolidImage(255, 255, 255, 255);
}

std::optional<ImageData> LoadImageFromMemory(std::span<const byte> bytes) {
    string storage;
    storage.resize(bytes.size());
    if (!bytes.empty()) {
        std::memcpy(storage.data(), bytes.data(), bytes.size());
    }
    std::istringstream stream{storage, std::ios::binary};
    if (ImageData::IsPNG(stream)) {
        stream.clear();
        stream.seekg(0, std::ios::beg);
        return ImageData::LoadPNG(stream, PNGLoadSettings{.AddAlphaIfRGB = 0xffu});
    }
    stream.clear();
    stream.seekg(0, std::ios::beg);
    if (ImageData::IsJPEG(stream)) {
        stream.clear();
        stream.seekg(0, std::ios::beg);
        return ImageData::LoadJPEG(stream, JPEGLoadSettings{.AddAlphaIfRGB = 0xffu});
    }
    return std::nullopt;
}

std::optional<ImageData> LoadImageFromFile(const std::filesystem::path& path) {
    std::ifstream stream{path, std::ios::binary};
    if (!stream.is_open()) {
        return std::nullopt;
    }
    if (ImageData::IsPNG(stream)) {
        stream.clear();
        stream.seekg(0, std::ios::beg);
        return ImageData::LoadPNG(stream, PNGLoadSettings{.AddAlphaIfRGB = 0xffu});
    }
    stream.clear();
    stream.seekg(0, std::ios::beg);
    if (ImageData::IsJPEG(stream)) {
        stream.clear();
        stream.seekg(0, std::ios::beg);
        return ImageData::LoadJPEG(stream, JPEGLoadSettings{.AddAlphaIfRGB = 0xffu});
    }
    return std::nullopt;
}

std::optional<vector<byte>> DecodeBase64(std::string_view text) {
    auto value = [](char ch) -> int {
        if (ch >= 'A' && ch <= 'Z') return ch - 'A';
        if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
        if (ch >= '0' && ch <= '9') return ch - '0' + 52;
        if (ch == '+') return 62;
        if (ch == '/') return 63;
        return -1;
    };
    vector<byte> out;
    uint32_t buffer = 0;
    uint32_t bits = 0;
    for (char ch : text) {
        if (ch == '=') {
            break;
        }
        if (ch == '\r' || ch == '\n' || ch == ' ' || ch == '\t') {
            continue;
        }
        int v = value(ch);
        if (v < 0) {
            return std::nullopt;
        }
        buffer = (buffer << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<byte>((buffer >> bits) & 0xffu));
        }
    }
    return out;
}

std::optional<ImageData> LoadCgltfImage(const cgltf_image& image, const std::filesystem::path& baseDir) {
    if (image.buffer_view != nullptr && image.buffer_view->buffer != nullptr && image.buffer_view->buffer->data != nullptr) {
        const auto* bytes = static_cast<const byte*>(image.buffer_view->buffer->data) + image.buffer_view->offset;
        return LoadImageFromMemory(std::span<const byte>{bytes, image.buffer_view->size});
    }
    if (image.uri == nullptr || std::strlen(image.uri) == 0) {
        return std::nullopt;
    }
    std::string_view uri{image.uri};
    if (uri.starts_with("data:")) {
        const size_t comma = uri.find(',');
        if (comma == std::string_view::npos) {
            return std::nullopt;
        }
        auto decoded = DecodeBase64(uri.substr(comma + 1));
        if (!decoded.has_value()) {
            return std::nullopt;
        }
        return LoadImageFromMemory(*decoded);
    }
    std::filesystem::path path = baseDir / std::filesystem::path{string{uri}};
    return LoadImageFromFile(path);
}

uint32_t TextureIndex(const cgltf_data* data, const cgltf_texture_view& view) {
    if (view.texture == nullptr || view.texture->image == nullptr || data == nullptr) {
        return std::numeric_limits<uint32_t>::max();
    }
    return static_cast<uint32_t>(cgltf_image_index(data, view.texture->image));
}

void ReadVec2(const cgltf_accessor* accessor, cgltf_size index, Eigen::Vector2f& out) {
    cgltf_float values[4] = {};
    if (accessor != nullptr && cgltf_accessor_read_float(accessor, index, values, 2)) {
        out = Eigen::Vector2f{values[0], values[1]};
    }
}

void ReadVec3(const cgltf_accessor* accessor, cgltf_size index, Eigen::Vector3f& out) {
    cgltf_float values[4] = {};
    if (accessor != nullptr && cgltf_accessor_read_float(accessor, index, values, 3)) {
        out = Eigen::Vector3f{values[0], values[1], values[2]};
    }
}

void ReadVec4(const cgltf_accessor* accessor, cgltf_size index, Eigen::Vector4f& out) {
    cgltf_float values[4] = {};
    if (accessor != nullptr && cgltf_accessor_read_float(accessor, index, values, 4)) {
        out = Eigen::Vector4f{values[0], values[1], values[2], values[3]};
    }
}

const cgltf_accessor* FindAccessor(const cgltf_primitive& primitive, cgltf_attribute_type type, cgltf_int index) {
    for (cgltf_size i = 0; i < primitive.attributes_count; ++i) {
        const cgltf_attribute& attr = primitive.attributes[i];
        if (attr.type == type && attr.index == index) {
            return attr.data;
        }
    }
    return nullptr;
}

void UpdateBounds(GltfModel& model, const Eigen::Vector3f& p) {
    if (!model.HasBounds) {
        model.BoundsMin = p;
        model.BoundsMax = p;
        model.HasBounds = true;
        return;
    }
    model.BoundsMin = model.BoundsMin.cwiseMin(p);
    model.BoundsMax = model.BoundsMax.cwiseMax(p);
}

Eigen::Vector3f MakeFallbackTangent(const Eigen::Vector3f& normal) {
    const Eigen::Vector3f n = normal.squaredNorm() > 1e-8f ? normal.normalized() : Eigen::Vector3f::UnitY();
    Eigen::Vector3f t = std::abs(n.y()) < 0.95f ? Eigen::Vector3f::UnitY().cross(n) : Eigen::Vector3f::UnitX().cross(n);
    if (t.squaredNorm() <= 1e-8f) {
        t = Eigen::Vector3f::UnitX();
    }
    return t.normalized();
}

void GenerateFallbackTangents(vector<GltfVertex>& vertices, std::span<const uint32_t> indices) {
    vector<Eigen::Vector3f> tangents(vertices.size(), Eigen::Vector3f::Zero());
    vector<Eigen::Vector3f> bitangents(vertices.size(), Eigen::Vector3f::Zero());

    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const uint32_t i0 = indices[i + 0];
        const uint32_t i1 = indices[i + 1];
        const uint32_t i2 = indices[i + 2];
        if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) {
            continue;
        }

        const GltfVertex& v0 = vertices[i0];
        const GltfVertex& v1 = vertices[i1];
        const GltfVertex& v2 = vertices[i2];
        const Eigen::Vector3f p10 = v1.Position - v0.Position;
        const Eigen::Vector3f p20 = v2.Position - v0.Position;
        const Eigen::Vector2f uv10 = v1.TexCoord - v0.TexCoord;
        const Eigen::Vector2f uv20 = v2.TexCoord - v0.TexCoord;
        const float det = uv10.x() * uv20.y() - uv10.y() * uv20.x();
        if (std::abs(det) <= 1e-8f) {
            continue;
        }

        const float invDet = 1.0f / det;
        const Eigen::Vector3f t = (p10 * uv20.y() - p20 * uv10.y()) * invDet;
        const Eigen::Vector3f b = (p20 * uv10.x() - p10 * uv20.x()) * invDet;
        tangents[i0] += t;
        tangents[i1] += t;
        tangents[i2] += t;
        bitangents[i0] += b;
        bitangents[i1] += b;
        bitangents[i2] += b;
    }

    for (GltfVertex& vertex : vertices) {
        const size_t index = static_cast<size_t>(&vertex - vertices.data());
        Eigen::Vector3f n = vertex.Normal.squaredNorm() > 1e-8f ? vertex.Normal.normalized() : Eigen::Vector3f::UnitY();
        Eigen::Vector3f t = tangents[index] - n * n.dot(tangents[index]);
        if (t.squaredNorm() <= 1e-8f) {
            t = MakeFallbackTangent(n);
        } else {
            t.normalize();
        }
        const Eigen::Vector3f b = bitangents[index];
        const float handedness = n.cross(t).dot(b) < 0.0f ? -1.0f : 1.0f;
        vertex.Tangent = Eigen::Vector4f{t.x(), t.y(), t.z(), handedness};
    }
}

void AddPrimitiveInstance(
    GltfModel& model,
    const cgltf_data* data,
    const cgltf_primitive& primitive,
    const Eigen::Matrix4f& world,
    std::string_view name) {
    if (primitive.type != cgltf_primitive_type_triangles) {
        return;
    }
    const cgltf_accessor* positionAcc = FindAccessor(primitive, cgltf_attribute_type_position, 0);
    if (positionAcc == nullptr || positionAcc->count == 0) {
        return;
    }
    const cgltf_accessor* normalAcc = FindAccessor(primitive, cgltf_attribute_type_normal, 0);
    const cgltf_accessor* tangentAcc = FindAccessor(primitive, cgltf_attribute_type_tangent, 0);
    const cgltf_accessor* texcoordAcc = FindAccessor(primitive, cgltf_attribute_type_texcoord, 0);

    GltfPrimitiveCpu out;
    out.Name = string{name};
    out.Vertices.resize(positionAcc->count);

    const Eigen::Matrix3f normalMatrix = world.block<3, 3>(0, 0).inverse().transpose();
    for (cgltf_size i = 0; i < positionAcc->count; ++i) {
        GltfVertex vertex{};
        Eigen::Vector3f p;
        ReadVec3(positionAcc, i, p);
        Eigen::Vector4f hp = world * Eigen::Vector4f{p.x(), p.y(), p.z(), 1.0f};
        vertex.Position = hp.head<3>();
        if (normalAcc != nullptr) {
            ReadVec3(normalAcc, i, vertex.Normal);
            vertex.Normal = (normalMatrix * vertex.Normal).normalized();
        }
        if (tangentAcc != nullptr) {
            Eigen::Vector4f tangent;
            ReadVec4(tangentAcc, i, tangent);
            Eigen::Vector3f t = (world.block<3, 3>(0, 0) * tangent.head<3>()).normalized();
            vertex.Tangent = Eigen::Vector4f{t.x(), t.y(), t.z(), tangent.w()};
        }
        if (texcoordAcc != nullptr) {
            ReadVec2(texcoordAcc, i, vertex.TexCoord);
        }
        out.Vertices[i] = vertex;
        UpdateBounds(model, vertex.Position);
    }
    if (primitive.indices != nullptr) {
        out.Indices.resize(primitive.indices->count);
        for (cgltf_size i = 0; i < primitive.indices->count; ++i) {
            cgltf_uint idx = 0;
            cgltf_accessor_read_uint(primitive.indices, i, &idx, 1);
            out.Indices[i] = static_cast<uint32_t>(idx);
        }
    } else {
        out.Indices.resize(out.Vertices.size());
        for (uint32_t i = 0; i < out.Indices.size(); ++i) {
            out.Indices[i] = i;
        }
    }
    if (tangentAcc == nullptr) {
        GenerateFallbackTangents(out.Vertices, out.Indices);
    }

    if (primitive.material != nullptr) {
        out.MaterialIndex = static_cast<uint32_t>(cgltf_material_index(data, primitive.material));
    }
    model.CpuPrimitives.push_back(std::move(out));
}

GltfMaterialCpu ConvertMaterial(const cgltf_data* data, const cgltf_material& src) {
    GltfMaterialCpu out;
    if (src.name != nullptr && src.name[0] != '\0') {
        out.Name = src.name;
    }
    if (src.has_pbr_metallic_roughness) {
        const auto& pbr = src.pbr_metallic_roughness;
        out.BaseColorFactor = Eigen::Vector4f{
            pbr.base_color_factor[0],
            pbr.base_color_factor[1],
            pbr.base_color_factor[2],
            pbr.base_color_factor[3]};
        out.Metallic = pbr.metallic_factor;
        out.Roughness = pbr.roughness_factor;
        uint32_t baseTex = TextureIndex(data, pbr.base_color_texture);
        uint32_t mrTex = TextureIndex(data, pbr.metallic_roughness_texture);
        if (baseTex != std::numeric_limits<uint32_t>::max()) {
            out.BaseColorTexture = static_cast<int>(baseTex);
        }
        if (mrTex != std::numeric_limits<uint32_t>::max()) {
            out.MetallicRoughnessTexture = static_cast<int>(mrTex);
        }
    }
    if (src.has_ior) {
        out.Eta = src.ior.ior;
    }
    if (src.has_specular) {
        out.Specular = src.specular.specular_factor;
        out.SpecularTint = std::max({src.specular.specular_color_factor[0], src.specular.specular_color_factor[1], src.specular.specular_color_factor[2]});
    }
    if (src.has_clearcoat) {
        out.Clearcoat = src.clearcoat.clearcoat_factor;
        out.ClearcoatGloss = 1.0f - src.clearcoat.clearcoat_roughness_factor;
    }
    if (src.has_sheen) {
        out.Sheen = std::max({src.sheen.sheen_color_factor[0], src.sheen.sheen_color_factor[1], src.sheen.sheen_color_factor[2]});
        out.SheenTint = out.Sheen > 0.0f ? 1.0f : 0.0f;
    }
    if (src.has_transmission) {
        out.SpecTrans = src.transmission.transmission_factor;
    }
    if (src.has_anisotropy) {
        out.Anisotropic = src.anisotropy.anisotropy_strength;
    }
    if (src.normal_texture.texture != nullptr) {
        uint32_t tex = TextureIndex(data, src.normal_texture);
        if (tex != std::numeric_limits<uint32_t>::max()) {
            out.NormalTexture = static_cast<int>(tex);
            out.NormalScale = src.normal_texture.scale;
        }
    }
    out.EmissiveFactor = Eigen::Vector3f{src.emissive_factor[0], src.emissive_factor[1], src.emissive_factor[2]};
    out.AlphaCutoff = src.alpha_cutoff;
    if (out.SpecTrans > 0.0f) {
        out.AlphaMode = GltfAlphaMode::Transmission;
    } else if (src.alpha_mode == cgltf_alpha_mode_blend) {
        out.AlphaMode = GltfAlphaMode::Blend;
    } else if (src.alpha_mode == cgltf_alpha_mode_mask) {
        out.AlphaMode = GltfAlphaMode::Mask;
    } else if (out.BaseColorFactor.w() < 0.999f) {
        out.AlphaMode = GltfAlphaMode::Blend;
    } else {
        out.AlphaMode = GltfAlphaMode::Opaque;
    }
    return out;
}

void BuildNodeInfo(GltfModel& model, const cgltf_data* data) {
    model.Nodes.resize(data->nodes_count);
    for (cgltf_size i = 0; i < data->nodes_count; ++i) {
        const cgltf_node& node = data->nodes[i];
        GltfNodeInfo& out = model.Nodes[i];
        out.Name = node.name != nullptr ? node.name : fmt::format("Node {}", i);
        out.Parent = node.parent != nullptr ? static_cast<int>(cgltf_node_index(data, node.parent)) : -1;
        out.HasMesh = node.mesh != nullptr;
        if (out.Parent < 0) {
            model.RootNodes.push_back(static_cast<int>(i));
        }
        for (cgltf_size c = 0; c < node.children_count; ++c) {
            out.Children.push_back(static_cast<int>(cgltf_node_index(data, node.children[c])));
        }
    }
}

std::optional<GltfModel> LoadGltfModel(const std::filesystem::path& path, string& error) {
    cgltf_options options{};
    cgltf_data* data = nullptr;
    cgltf_result result = cgltf_parse_file(&options, path.string().c_str(), &data);
    if (result != cgltf_result_success || data == nullptr) {
        error = fmt::format("cgltf_parse_file failed: {}", static_cast<int>(result));
        return std::nullopt;
    }
    auto guard = MakeScopeGuard([&]() {
        cgltf_free(data);
    });
    result = cgltf_load_buffers(&options, data, path.string().c_str());
    if (result != cgltf_result_success) {
        error = fmt::format("cgltf_load_buffers failed: {}", static_cast<int>(result));
        return std::nullopt;
    }
    result = cgltf_validate(data);
    if (result != cgltf_result_success) {
        error = fmt::format("cgltf_validate failed: {}", static_cast<int>(result));
        return std::nullopt;
    }

    GltfModel model;
    model.Path = path.string();
    const std::filesystem::path baseDir = path.parent_path();

    model.CpuTextures.reserve(data->images_count + 2);
    for (cgltf_size i = 0; i < data->images_count; ++i) {
        GltfTextureCpu tex;
        tex.Name = data->images[i].name != nullptr ? data->images[i].name : fmt::format("Image {}", i);
        auto image = LoadCgltfImage(data->images[i], baseDir);
        tex.Image = image.has_value() ? ConvertToRGBA8(*image) : MakeSolidImage(255, 255, 255, 255);
        tex.Sampler = DefaultSampler();
        for (cgltf_size t = 0; t < data->textures_count; ++t) {
            if (data->textures[t].image != &data->images[i] || data->textures[t].sampler == nullptr) {
                continue;
            }
            const cgltf_sampler& sampler = *data->textures[t].sampler;
            tex.Sampler.AddressS = ToAddressMode(sampler.wrap_s);
            tex.Sampler.AddressT = ToAddressMode(sampler.wrap_t);
            tex.Sampler.MinFilter = ToFilterMode(sampler.min_filter);
            tex.Sampler.MagFilter = ToFilterMode(sampler.mag_filter);
            tex.Sampler.MipmapFilter = ToFilterMode(sampler.min_filter);
            break;
        }
        model.CpuTextures.push_back(std::move(tex));
    }

    model.CpuMaterials.reserve(std::max<cgltf_size>(1, data->materials_count));
    for (cgltf_size i = 0; i < data->materials_count; ++i) {
        model.CpuMaterials.push_back(ConvertMaterial(data, data->materials[i]));
    }
    EnsureDefaultMaterial(model);

    BuildNodeInfo(model, data);
    for (cgltf_size n = 0; n < data->nodes_count; ++n) {
        const cgltf_node& node = data->nodes[n];
        if (node.mesh == nullptr) {
            continue;
        }
        cgltf_float rawWorld[16];
        cgltf_node_transform_world(&node, rawWorld);
        const Eigen::Matrix4f world = CgltfMatrixToEigen(rawWorld);
        for (cgltf_size p = 0; p < node.mesh->primitives_count; ++p) {
            string primName = fmt::format("{} / prim {}", node.name != nullptr ? node.name : "node", p);
            AddPrimitiveInstance(model, data, node.mesh->primitives[p], world, primName);
        }
    }
    if (model.CpuPrimitives.empty()) {
        error = "glTF has no supported triangle primitives";
        return std::nullopt;
    }
    return model;
}

vector<byte> ToBytes(std::span<const GltfVertex> vertices) {
    vector<byte> bytes(vertices.size_bytes());
    if (!bytes.empty()) {
        std::memcpy(bytes.data(), vertices.data(), bytes.size());
    }
    return bytes;
}

vector<byte> ToBytes(std::span<const uint32_t> indices) {
    vector<byte> bytes(indices.size_bytes());
    if (!bytes.empty()) {
        std::memcpy(bytes.data(), indices.data(), bytes.size());
    }
    return bytes;
}

GltfMaterialConstants PackMaterialConstants(const GltfMaterialCpu& material) {
    GltfMaterialConstants out{};
    out.BaseColorFactor[0] = material.BaseColorFactor.x();
    out.BaseColorFactor[1] = material.BaseColorFactor.y();
    out.BaseColorFactor[2] = material.BaseColorFactor.z();
    out.BaseColorFactor[3] = material.BaseColorFactor.w();
    out.EmissiveFactorAlphaCutoff[0] = material.EmissiveFactor.x();
    out.EmissiveFactorAlphaCutoff[1] = material.EmissiveFactor.y();
    out.EmissiveFactorAlphaCutoff[2] = material.EmissiveFactor.z();
    out.EmissiveFactorAlphaCutoff[3] = material.AlphaCutoff;
    out.Principled0[0] = material.Metallic;
    out.Principled0[1] = material.Roughness;
    out.Principled0[2] = material.Specular;
    out.Principled0[3] = material.SpecularTint;
    out.Principled1[0] = material.Anisotropic;
    out.Principled1[1] = material.Sheen;
    out.Principled1[2] = material.SheenTint;
    out.Principled1[3] = material.Flatness;
    out.Principled2[0] = material.Clearcoat;
    out.Principled2[1] = material.ClearcoatGloss;
    out.Principled2[2] = material.SpecTrans;
    out.Principled2[3] = material.Eta;
    out.NormalParams[0] = material.NormalScale;
    out.NormalParams[1] = 0.0f;
    out.NormalParams[2] = 0.0f;
    out.NormalParams[3] = 0.0f;
    out.Flags[0] = material.BaseColorTexture >= 0 ? 1u : 0u;
    out.Flags[1] = material.NormalTexture >= 0 ? 1u : 0u;
    out.Flags[2] = material.MetallicRoughnessTexture >= 0 ? 1u : 0u;
    out.Flags[3] = static_cast<uint32_t>(material.AlphaMode);
    return out;
}

bool WriteUploadBuffer(render::Buffer* buffer, std::span<const byte> data) {
    if (buffer == nullptr) {
        return false;
    }
    void* mapped = buffer->Map(0, data.size());
    if (mapped == nullptr) {
        return false;
    }
    std::memcpy(mapped, data.data(), data.size());
    buffer->Unmap(0, data.size());
    return true;
}

Eigen::Quaternionf MakeCameraRotation(const Eigen::Vector3f& forward) {
    Eigen::Vector3f f = forward.squaredNorm() > 1e-8f ? forward.normalized() : Eigen::Vector3f{0.0f, 0.0f, -1.0f};
    Eigen::Vector3f up = Eigen::Vector3f::UnitY();
    if (std::abs(f.dot(up)) > 0.98f) {
        up = Eigen::Vector3f::UnitZ();
    }
    Eigen::Vector3f right = f.cross(up).normalized();
    Eigen::Vector3f cameraUp = right.cross(f).normalized();
    Eigen::Matrix3f rotation = Eigen::Matrix3f::Identity();
    rotation.col(0) = right;
    rotation.col(1) = cameraUp;
    rotation.col(2) = -f;
    Eigen::Quaternionf quat{rotation};
    quat.normalize();
    return quat;
}

}  // namespace

class GltfViewerApp : public Application {
public:
    static constexpr render::TextureFormat BackBufferFormat = render::TextureFormat::BGRA8_UNORM;
    static constexpr render::TextureFormat DepthFormat = render::TextureFormat::D32_FLOAT;

    void SetInitialLoadPath(std::filesystem::path path) {
        _initialLoadPath = std::move(path);
    }

    void SetDebugMode(GltfDebugMode mode) noexcept {
        _debugMode = mode;
    }

    void OnInit() override {
        InitPipeline();
        ConfigureCameraControl();
        AttachCameraInput();
        SetCameraFrame(Eigen::Vector3f::Zero(), 4.0f);
        _loadPath.resize(1024);
        const std::filesystem::path defaultPath = _initialLoadPath.empty()
            ? (std::filesystem::path{RADRAY_GLTF_VIEWER_ASSET_DIR} / "model.gltf")
            : _initialLoadPath;
        const string initial = defaultPath.string();
        std::memcpy(_loadPath.data(), initial.c_str(), std::min(initial.size(), _loadPath.size() - 1));
        if (!_initialLoadPath.empty()) {
            _pendingLoadPath = _initialLoadPath;
            _pendingLoad = true;
        }
    }

    void OnUpdate(const AppUpdateContext& ctx) override {
        if (_pendingLoad) {
            LoadModelFromPath(_pendingLoadPath);
            _pendingLoad = false;
        }
        if (ImGuiSystem* imgui = GetSubsystem<ImGuiSystem>()) {
            if (imgui->Begin(ctx)) {
                DrawUi(ctx);
                imgui->End();
            }
        }
        PollCameraInput();
    }

    bool OnRenderView(AppFrameContext& ctx, const AppFrameTarget& target) override {
        if (!target.Window->IsMainWindow()) {
            return false;
        }
        if (_pipeline.GetPassCount() == 0) {
            return false;
        }
        render::TextureDescriptor bbDesc = target.BackBuffer->GetDesc();
        GltfModel* frameModel = nullptr;
        {
            std::lock_guard lock{_modelMutex};
            frameModel = _model.get();
            if (frameModel != nullptr && !frameModel->Uploaded && !frameModel->UploadFailed) {
                if (!UploadModel(ctx, *frameModel)) {
                    frameModel->UploadFailed = true;
                    if (!frameModel->UploadFailureLogged) {
                        RADRAY_ERR_LOG("glTF upload failed for '{}'", frameModel->Path);
                        frameModel->UploadFailureLogged = true;
                    }
                }
            }
        }

        UpdateView(bbDesc.Width, bbDesc.Height);

        // viewer 专属状态每帧注入到 pass(代替原来的 RenderContext::UserData)。
        if (_scenePass != nullptr) {
            _scenePass->SetFrameData(frameModel, &_view, static_cast<uint32_t>(_debugMode));
        }

        RenderContext rc{};
        rc.FlightIndex = ctx.FlightIndex();
        rc.CmdBuffer = ctx.GetCommandBuffer();
        rc.Device = GetDevice();
        rc.Gpu = GetGpuSystem();
        rc.ColorTarget = target.BackBufferView;
        rc.ColorFormat = bbDesc.Format;
        rc.Width = bbDesc.Width;
        rc.Height = bbDesc.Height;
        rc.Resources = &_resourcePool;

        _pipeline.Render(rc);
        return true;
    }

    void OnShutdown() override {
        _inputConnections.clear();
        {
            std::lock_guard lock{_modelMutex};
            _model.reset();
            _retiredModels.clear();
        }
        _resourcePool.Clear();
        _scenePass = nullptr;
        _rootSig.reset();
        _vs = nullptr;
        _ps = nullptr;
    }

    void OnRenderFrameComplete(const AppRenderCompleteContext& ctx) override {
        if (!ctx.GpuWorkCompleted) {
            return;
        }
        CompleteRetiredModelFrame();
    }

private:
    void ConfigureCameraControl() {
        _cameraControl.MinDistance = 0.05f;
        _cameraControl.MaxDistance = 10000.0f;
        _cameraControl.OrbitSensitivity = 0.003f;
        _cameraControl.PanSensitivity = 0.003f;
        _cameraControl.DollySensitivity = 0.15f;
        _cameraControl.UseTrackball = false;
        _cameraControl.InvertZoom = false;
    }

    void AttachCameraInput() {
        WindowManager* windows = GetWindowManager();
        AppWindow* mainWindow = windows != nullptr ? windows->GetMainWindow() : nullptr;
        NativeWindow* nativeWindow = mainWindow != nullptr ? mainWindow->GetNativeWindow() : nullptr;
        if (nativeWindow == nullptr) {
            RADRAY_WARN_LOG("glTF viewer camera input disabled: no main window");
            return;
        }

        _mainNativeWindow = nativeWindow;
#ifndef RADRAY_PLATFORM_WINDOWS
        _inputConnections.emplace_back(nativeWindow->EventTouch().connect([this](int x, int y, MouseButton button, Action action) {
            OnCameraPointer(x, y, button, action);
        }));
#endif
        _inputConnections.emplace_back(nativeWindow->EventMouseWheel().connect([this](int delta) {
            OnCameraWheel(delta);
        }));
        _inputConnections.emplace_back(nativeWindow->EventMouseLeave().connect([this]() {
            _cameraControl.IsOrbiting = false;
            _cameraControl.IsPanning = false;
            _cameraControl.IsDollying = false;
            _cameraControl.WheelDelta = 0.0f;
        }));
    }

    void InitPipeline() {
        const std::filesystem::path shaderPath = std::filesystem::path{RADRAY_GLTF_VIEWER_ASSET_DIR} / "gltf_viewer.hlsl";
        _vs = GetGpuSystem()->GetOrCompileShaderFromFile(shaderPath, "VSMain", render::ShaderStage::Vertex, "gltf_viewer").Get();
        _ps = GetGpuSystem()->GetOrCompileShaderFromFile(shaderPath, "PSMain", render::ShaderStage::Pixel, "gltf_viewer").Get();
        if (_vs == nullptr || _ps == nullptr) {
            RADRAY_ERR_LOG("failed to compile glTF viewer shaders");
            return;
        }

        render::Shader* shaders[] = {_vs, _ps};
        render::RootSignatureDescriptor rsDesc{std::span<render::Shader*>{shaders}};
        auto rootSigOpt = GetDevice()->CreateRootSignature(rsDesc);
        if (!rootSigOpt.HasValue()) {
            RADRAY_ERR_LOG("failed to create glTF viewer root signature");
            return;
        }
        _rootSig = rootSigOpt.Release();

        auto pass = make_unique<GltfScenePass>(
            DepthFormat,
            render::ColorClearValue{{0.06f, 0.07f, 0.08f, 1.0f}});
        pass->SetRootSignature(_rootSig.get());
        _scenePass = pass.get();
        _pipeline.AddPass(std::move(pass));
    }

    void LoadModelFromPath(const std::filesystem::path& path) {
        RetireCurrentModel();
        _loadError.clear();
        _selectedNode = -1;
        string error;
        auto model = LoadGltfModel(path, error);
        if (!model.has_value()) {
            _loadError = error;
            RADRAY_ERR_LOG("glTF load failed: {}", error);
            return;
        }
        GltfModel loadedModel = std::move(model.value());
        RADRAY_INFO_LOG(
            "glTF loaded '{}': primitives={}, materials={}, textures={}, nodes={}, bounds_min=({}, {}, {}), bounds_max=({}, {}, {})",
            loadedModel.Path,
            loadedModel.CpuPrimitives.size(),
            loadedModel.CpuMaterials.size(),
            loadedModel.CpuTextures.size(),
            loadedModel.Nodes.size(),
            loadedModel.BoundsMin.x(),
            loadedModel.BoundsMin.y(),
            loadedModel.BoundsMin.z(),
            loadedModel.BoundsMax.x(),
            loadedModel.BoundsMax.y(),
            loadedModel.BoundsMax.z());
        {
            std::lock_guard lock{_modelMutex};
            _model = make_unique<GltfModel>(std::move(loadedModel));
        }
        FrameCameraToModel();
    }

    void RetireCurrentModel() {
        std::lock_guard lock{_modelMutex};
        if (_model == nullptr) {
            return;
        }
        const uint32_t flightCount = GetGpuSystem() != nullptr ? GetGpuSystem()->GetFlightDataCount() : 1u;
        _retiredModels.push_back(RetiredModel{
            .Model = std::move(_model),
            .RemainingFrameCompletions = std::max(1u, flightCount)});
    }

    void CompleteRetiredModelFrame() {
        vector<unique_ptr<GltfModel>> readyToDestroy;
        {
            std::lock_guard lock{_modelMutex};
            for (RetiredModel& retired : _retiredModels) {
                if (retired.RemainingFrameCompletions > 0) {
                    --retired.RemainingFrameCompletions;
                }
            }
            auto it = _retiredModels.begin();
            while (it != _retiredModels.end()) {
                if (it->RemainingFrameCompletions == 0) {
                    readyToDestroy.push_back(std::move(it->Model));
                    it = _retiredModels.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }

    void FrameCameraToModel() {
        Eigen::Vector3f center{Eigen::Vector3f::Zero()};
        float distance = 4.0f;
        {
            std::lock_guard lock{_modelMutex};
            if (_model != nullptr && _model->HasBounds) {
                center = (_model->BoundsMin + _model->BoundsMax) * 0.5f;
                const float radius = std::max((_model->BoundsMax - _model->BoundsMin).norm() * 0.5f, 0.1f);
                distance = radius * 2.8f;
            }
        }
        SetCameraFrame(center, distance);
    }

    void SetCameraFrame(const Eigen::Vector3f& center, float distance) {
        _cameraControl.Reset();
        _cameraControl.SetOrbitTarget(center);
        distance = Clamp(distance, _cameraControl.MinDistance, _cameraControl.MaxDistance);
        _cameraTarget = center;
        _cameraDistance = distance;
        _cameraYaw = 0.0f;
        _cameraPitch = Radian(20.0f);
        UpdateCameraTransform();
    }

    bool IsUiBlockingCameraInput() const {
        if (ImGui::GetCurrentContext() == nullptr) {
            return false;
        }
        const ImGuiIO& io = ImGui::GetIO();
        return io.WantCaptureMouse ||
               ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) ||
               ImGui::IsAnyItemActive();
    }

    void ApplyCameraControl(const Eigen::Vector2f& mousePos, bool orbit, bool pan, bool inputBlocked) {
        const Eigen::Vector2f delta = mousePos - _lastCameraPollMousePos;
        _lastCameraPollMousePos = mousePos;
        _cameraControl.CurrentMousePos = mousePos;
        if (inputBlocked) {
            _cameraControl.IsOrbiting = false;
            _cameraControl.IsPanning = false;
            _cameraControl.IsDollying = false;
            _cameraControl.LastMousePos = mousePos;
            return;
        }

        if (delta.squaredNorm() < 1e-6f) {
            return;
        }

        if (orbit) {
            _cameraYaw += delta.x() * _cameraControl.OrbitSensitivity;
            _cameraPitch -= delta.y() * _cameraControl.OrbitSensitivity;
            _cameraPitch = Clamp(_cameraPitch, Radian(-85.0f), Radian(85.0f));
            UpdateCameraTransform();
        } else if (pan) {
            const Eigen::Vector3f right = _cameraRotation * Eigen::Vector3f::UnitX();
            const Eigen::Vector3f up = _cameraRotation * Eigen::Vector3f::UnitY();
            const float scale = _cameraDistance * _cameraControl.PanSensitivity * 0.5f;
            const Eigen::Vector3f panVector = right * (delta.x() * scale) + up * (delta.y() * scale);
            _cameraTarget += panVector;
            _cameraControl.SetOrbitTarget(_cameraTarget);
            UpdateCameraTransform();
        }
    }

    void PollCameraInput() {
#ifdef RADRAY_PLATFORM_WINDOWS
        if (_mainNativeWindow == nullptr) {
            _cameraPollAnyButtonDown = false;
            _cameraInputCapturedByUi = false;
            _cameraControl.IsOrbiting = false;
            _cameraControl.IsPanning = false;
            return;
        }

        POINT screenPos{};
        if (::GetCursorPos(&screenPos) == 0) {
            return;
        }
        const Eigen::Vector2i clientPos = _mainNativeWindow->ScreenToClient(Eigen::Vector2i{screenPos.x, screenPos.y});
        const Eigen::Vector2i windowSize = _mainNativeWindow->GetSize();
        const bool insideClient =
            clientPos.x() >= 0 &&
            clientPos.y() >= 0 &&
            clientPos.x() < windowSize.x() &&
            clientPos.y() < windowSize.y();

        const bool leftDown = (::GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        const bool rightDown = (::GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
        const bool middleDown = (::GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
        const bool anyDown = leftDown || rightDown || middleDown;
        const Eigen::Vector2f mousePos{static_cast<float>(clientPos.x()), static_cast<float>(clientPos.y())};
        const bool uiBlockingCamera = IsUiBlockingCameraInput();

        if (!anyDown) {
            _cameraPollAnyButtonDown = false;
            _cameraInputCapturedByUi = false;
            _cameraControl.IsOrbiting = false;
            _cameraControl.IsPanning = false;
            _cameraControl.IsDollying = false;
            _cameraControl.LastMousePos = mousePos;
            _lastCameraPollMousePos = mousePos;
            return;
        }

        if (!_cameraPollAnyButtonDown) {
            _cameraPollAnyButtonDown = true;
            _cameraInputCapturedByUi = !insideClient || uiBlockingCamera;
            _cameraControl.LastMousePos = mousePos;
            _lastCameraPollMousePos = mousePos;
        }

        const bool pan = rightDown || middleDown;
        const bool orbit = leftDown && !pan;
        ApplyCameraControl(mousePos, orbit, pan, _cameraInputCapturedByUi);
#endif
    }

    void OnCameraPointer(int x, int y, MouseButton button, Action action) {
        if (action == Action::UNKNOWN || button == MouseButton::UNKNOWN) {
            return;
        }

        const Eigen::Vector2f mousePos{static_cast<float>(x), static_cast<float>(y)};
        _cameraControl.CurrentMousePos = mousePos;

        if (action == Action::PRESSED) {
            if (IsUiBlockingCameraInput()) {
                _cameraInputCapturedByUi = true;
                _cameraControl.IsOrbiting = false;
                _cameraControl.IsPanning = false;
                _cameraControl.IsDollying = false;
                _cameraControl.LastMousePos = mousePos;
                return;
            }
            _cameraInputCapturedByUi = false;
            _cameraControl.LastMousePos = mousePos;
            if (button == MouseButton::BUTTON_LEFT) {
                _cameraControl.IsOrbiting = true;
            } else if (button == MouseButton::BUTTON_RIGHT || button == MouseButton::BUTTON_MIDDLE) {
                _cameraControl.IsPanning = true;
            }
            return;
        }

        if (action == Action::RELEASED) {
            if (button == MouseButton::BUTTON_LEFT) {
                _cameraControl.IsOrbiting = false;
            } else if (button == MouseButton::BUTTON_RIGHT || button == MouseButton::BUTTON_MIDDLE) {
                _cameraControl.IsPanning = false;
            }
            _cameraControl.LastMousePos = mousePos;
            if (!_cameraControl.IsOrbiting && !_cameraControl.IsPanning) {
                _cameraInputCapturedByUi = false;
            }
            return;
        }

        if (action == Action::REPEATED && !_cameraInputCapturedByUi) {
            if (_cameraControl.IsOrbiting) {
                _cameraControl.Orbit(_cameraPosition, _cameraRotation);
            }
            if (_cameraControl.IsPanning) {
                _cameraControl.Pan(_cameraPosition, _cameraRotation);
            }
            _cameraRotation.normalize();
        }
    }

    void OnCameraWheel(int delta) {
        if (IsUiBlockingCameraInput()) {
            return;
        }
        _cameraControl.WheelDelta += static_cast<float>(delta) / 120.0f;
        _cameraControl.Dolly(_cameraPosition, _cameraRotation);
        _cameraDistance = _cameraControl.Distance;
        _cameraTarget = _cameraControl.OrbitCenter;
        UpdateCameraTransform();
    }

    void UpdateCameraTransform() {
        const float cp = std::cos(_cameraPitch);
        const Eigen::Vector3f dir{
            std::sin(_cameraYaw) * cp,
            std::sin(_cameraPitch),
            std::cos(_cameraYaw) * cp};
        _cameraPosition = _cameraTarget - dir * _cameraDistance;
        _cameraRotation = MakeCameraRotation(_cameraTarget - _cameraPosition);
        _cameraControl.SetOrbitTarget(_cameraTarget);
        _cameraControl.UpdateDistance(_cameraPosition);
    }

    void UpdateView(uint32_t width, uint32_t height) {
        UpdateCameraTransform();
        const Eigen::Vector3f forward = _cameraRotation * Eigen::Vector3f{0.0f, 0.0f, -1.0f};
        const Eigen::Vector3f up = _cameraRotation * Eigen::Vector3f::UnitY();
        _view.Eye = _cameraPosition;
        _view.View = LookAtLH<float>(_cameraPosition, _cameraPosition + forward, up);
        const float aspect = height == 0 ? 1.0f : static_cast<float>(width) / static_cast<float>(height);
        _view.Proj = PerspectiveLH<float>(Radian(60.0f), aspect, 0.01f, 10000.0f);
        _view.ViewProj = _view.Proj * _view.View;
    }

    void DrawNodeTree(const GltfModel& model, int nodeIndex) {
        if (nodeIndex < 0 || nodeIndex >= static_cast<int>(model.Nodes.size())) {
            return;
        }
        const GltfNodeInfo& node = model.Nodes[nodeIndex];
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (node.Children.empty()) {
            flags |= ImGuiTreeNodeFlags_Leaf;
        }
        if (_selectedNode == nodeIndex) {
            flags |= ImGuiTreeNodeFlags_Selected;
        }
        bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<intptr_t>(nodeIndex)), flags, "%s%s", node.Name.c_str(), node.HasMesh ? "  [mesh]" : "");
        if (ImGui::IsItemClicked()) {
            _selectedNode = nodeIndex;
        }
        if (open) {
            for (int child : node.Children) {
                DrawNodeTree(model, child);
            }
            ImGui::TreePop();
        }
    }

    void DrawUi(const AppUpdateContext& ctx) {
        ImGui::SetNextWindowSize(ImVec2{380.0f, 620.0f}, ImGuiCond_FirstUseEver);
        if (ImGui::Begin("glTF Viewer")) {
            ImGui::Text("Frame %.3f ms  GPU %.3f ms", ctx.DeltaTime.count() * 1000.0f, GetGpuSystem()->GetLastGpuTimeMs());
            ImGui::InputText("Path", _loadPath.data(), _loadPath.size());
            if (ImGui::Button("Load")) {
                _pendingLoadPath = _loadPath.data();
                _pendingLoad = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Unload")) {
                RetireCurrentModel();
                _selectedNode = -1;
            }
            ImGui::SameLine();
            if (ImGui::Button("Frame")) {
                FrameCameraToModel();
            }
            static constexpr std::array DebugModeNames{
                "Shaded",
                "Geometry Normal",
                "UV",
                "White",
                "Normal Texture",
                "Shading Normal",
            };
            int debugMode = static_cast<int>(_debugMode);
            if (ImGui::Combo("Debug View", &debugMode, DebugModeNames.data(), static_cast<int>(DebugModeNames.size()))) {
                _debugMode = static_cast<GltfDebugMode>(debugMode);
            }
            if (!_loadError.empty()) {
                ImGui::TextWrapped("Error: %s", _loadError.c_str());
            }
            ImGui::Separator();
            std::lock_guard lock{_modelMutex};
            if (_model) {
                ImGui::Text("GPU: %s", _model->Uploaded ? "uploaded" : (_model->UploadFailed ? "upload failed" : "pending"));
                ImGui::Text("Primitives: %zu", _model->CpuPrimitives.size());
                ImGui::Text("Materials: %zu", _model->CpuMaterials.size());
                ImGui::Text("Textures: %zu", _model->CpuTextures.size());
                ImGui::Text("Nodes: %zu", _model->Nodes.size());
                if (ImGui::CollapsingHeader("Scene Nodes", ImGuiTreeNodeFlags_DefaultOpen)) {
                    for (int root : _model->RootNodes) {
                        DrawNodeTree(*_model, root);
                    }
                }
                if (ImGui::CollapsingHeader("Materials")) {
                    for (size_t i = 0; i < _model->CpuMaterials.size(); ++i) {
                        const GltfMaterialCpu& material = _model->CpuMaterials[i];
                        ImGui::PushID(static_cast<int>(i));
                        if (ImGui::TreeNode(material.Name.c_str())) {
                            ImGui::Text("Base color texture: %d", material.BaseColorTexture);
                            ImGui::Text("Metallic roughness texture: %d", material.MetallicRoughnessTexture);
                            ImGui::Text("Normal texture: %d", material.NormalTexture);
                            ImGui::Text("Normal scale: %.3f", material.NormalScale);
                            ImGui::TreePop();
                        }
                        ImGui::PopID();
                    }
                }
                if (_selectedNode >= 0 && _selectedNode < static_cast<int>(_model->Nodes.size())) {
                    const GltfNodeInfo& node = _model->Nodes[_selectedNode];
                    ImGui::Separator();
                    ImGui::Text("Selected: %s", node.Name.c_str());
                    ImGui::Text("Parent: %d", node.Parent);
                    ImGui::Text("Children: %zu", node.Children.size());
                    ImGui::Text("Has mesh: %s", node.HasMesh ? "yes" : "no");
                }
            }
        }
        ImGui::End();
    }

    bool UploadModel(AppFrameContext& frame, GltfModel& model) {
        if (_rootSig == nullptr) {
            RADRAY_ERR_LOG("glTF upload skipped: root signature is null");
            return false;
        }
        model.GpuTextures.clear();
        model.GpuMaterials.clear();
        model.GpuPrimitives.clear();
        model.Pso.reset();
        model.DrawSummaryLogged = false;
        CreateDefaultTextures(model);

        model.GpuTextures.reserve(model.CpuTextures.size());
        for (const GltfTextureCpu& cpu : model.CpuTextures) {
            GltfTextureGpu gpu{};
            render::TextureDescriptor texDesc{
                .Dim = render::TextureDimension::Dim2D,
                .Width = cpu.Image.Width,
                .Height = cpu.Image.Height,
                .DepthOrArraySize = 1,
                .MipLevels = 1,
                .SampleCount = 1,
                .Format = render::TextureFormat::RGBA8_UNORM,
                .Memory = render::MemoryType::Device,
                .Usage = render::TextureUse::CopyDestination | render::TextureUse::Resource,
                .Hints = render::ResourceHint::None};
            auto texOpt = GetDevice()->CreateTexture(texDesc);
            if (!texOpt.HasValue()) {
                RADRAY_ERR_LOG("failed to create glTF texture '{}'", cpu.Name);
                return false;
            }
            gpu.Texture = texOpt.Release();
            frame.GetUploader().UploadTexture(frame.GetCommandBuffer(), TextureUploadRequest{
                .SrcData = cpu.Image.GetSpan(),
                .DstTexture = gpu.Texture.get(),
                .DstRange = render::SubresourceRange{0, 1, 0, 1},
                .SrcRowPitch = static_cast<uint64_t>(cpu.Image.Width) * 4u,
                .Before = render::TextureState::Undefined,
                .After = render::TextureState::ShaderRead});
            render::TextureViewDescriptor viewDesc{
                .Target = gpu.Texture.get(),
                .Dim = render::TextureDimension::Dim2D,
                .Format = render::TextureFormat::RGBA8_UNORM,
                .Range = render::SubresourceRange{0, 1, 0, 1},
                .Usage = render::TextureViewUsage::Resource};
            auto viewOpt = GetDevice()->CreateTextureView(viewDesc);
            if (!viewOpt.HasValue()) {
                RADRAY_ERR_LOG("failed to create glTF texture view '{}'", cpu.Name);
                return false;
            }
            gpu.View = viewOpt.Release();
            auto samplerOpt = GetDevice()->CreateSampler(cpu.Sampler);
            if (!samplerOpt.HasValue()) {
                RADRAY_ERR_LOG("failed to create glTF sampler '{}'", cpu.Name);
                return false;
            }
            gpu.Sampler = samplerOpt.Release();
            model.GpuTextures.push_back(std::move(gpu));
        }

        model.GpuMaterials.reserve(model.CpuMaterials.size());
        const uint64_t cbufferSize = Align(sizeof(GltfMaterialConstants), static_cast<uint64_t>(GetDevice()->GetDetail().CBufferAlignment));
        for (const GltfMaterialCpu& cpu : model.CpuMaterials) {
            GltfMaterialGpu gpu{};
            render::BufferDescriptor bufDesc{
                .Size = cbufferSize,
                .Memory = render::MemoryType::Upload,
                .Usage = render::BufferUse::MapWrite | render::BufferUse::CBuffer,
                .Hints = render::ResourceHint::None};
            auto bufOpt = GetDevice()->CreateBuffer(bufDesc);
            if (!bufOpt.HasValue()) {
                RADRAY_ERR_LOG("failed to create glTF material constant buffer '{}'", cpu.Name);
                return false;
            }
            gpu.Constants = bufOpt.Release();
            GltfMaterialConstants constants = PackMaterialConstants(cpu);
            vector<byte> padded(cbufferSize, byte{0});
            std::memcpy(padded.data(), &constants, sizeof(constants));
            if (!WriteUploadBuffer(gpu.Constants.get(), padded)) {
                RADRAY_ERR_LOG("failed to write glTF material constant buffer '{}'", cpu.Name);
                return false;
            }
            if (GetDevice()->GetBackend() == render::RenderBackend::Vulkan) {
                render::ResourceBarrierDescriptor barrier = render::BarrierBufferDescriptor{
                    .Target = gpu.Constants.get(),
                    .Before = render::BufferState::HostWrite,
                    .After = render::BufferState::CBuffer};
                frame.GetCommandBuffer()->ResourceBarrier(std::span{&barrier, 1});
            }

            auto setOpt = GetDevice()->CreateDescriptorSet(_rootSig.get(), render::DescriptorSetIndex{1});
            if (!setOpt.HasValue()) {
                RADRAY_ERR_LOG("failed to create glTF descriptor set '{}'", cpu.Name);
                return false;
            }
            gpu.DescriptorSet = setOpt.Release();
            render::BufferBindingDescriptor cbufferView{
                .Target = gpu.Constants.get(),
                .Range = render::BufferRange{0, cbufferSize},
                .Stride = 0,
                .Format = render::TextureFormat::UNKNOWN,
                .Usage = render::BufferViewUsage::CBuffer};
            gpu.DescriptorSet->WriteResource("gMaterial", cbufferView);
            WriteMaterialTextureBindings(gpu, cpu, model);
            model.GpuMaterials.push_back(std::move(gpu));
        }

        model.GpuPrimitives.reserve(model.CpuPrimitives.size());
        for (const GltfPrimitiveCpu& cpu : model.CpuPrimitives) {
            GltfPrimitiveGpu gpu{};
            vector<byte> vb = ToBytes(cpu.Vertices);
            vector<byte> ib = ToBytes(cpu.Indices);
            render::BufferDescriptor vbDesc{
                .Size = vb.size(),
                .Memory = render::MemoryType::Device,
                .Usage = render::BufferUse::Vertex | render::BufferUse::CopyDestination,
                .Hints = render::ResourceHint::None};
            auto vbOpt = GetDevice()->CreateBuffer(vbDesc);
            if (!vbOpt.HasValue()) {
                RADRAY_ERR_LOG("failed to create glTF vertex buffer '{}'", cpu.Name);
                return false;
            }
            gpu.VertexBuffer = vbOpt.Release();
            frame.GetUploader().UploadBuffer(frame.GetCommandBuffer(), BufferUploadRequest{
                .SrcData = vb,
                .DstBuffer = gpu.VertexBuffer.get(),
                .DstOffset = 0,
                .Before = render::BufferState::Common,
                .After = render::BufferState::Vertex});

            render::BufferDescriptor ibDesc{
                .Size = ib.size(),
                .Memory = render::MemoryType::Device,
                .Usage = render::BufferUse::Index | render::BufferUse::CopyDestination,
                .Hints = render::ResourceHint::None};
            auto ibOpt = GetDevice()->CreateBuffer(ibDesc);
            if (!ibOpt.HasValue()) {
                RADRAY_ERR_LOG("failed to create glTF index buffer '{}'", cpu.Name);
                return false;
            }
            gpu.IndexBuffer = ibOpt.Release();
            frame.GetUploader().UploadBuffer(frame.GetCommandBuffer(), BufferUploadRequest{
                .SrcData = ib,
                .DstBuffer = gpu.IndexBuffer.get(),
                .DstOffset = 0,
                .Before = render::BufferState::Common,
                .After = render::BufferState::Index});
            gpu.Vbv = render::VertexBufferView{gpu.VertexBuffer.get(), 0, vb.size()};
            gpu.Ibv = render::IndexBufferView{gpu.IndexBuffer.get(), 0, sizeof(uint32_t)};
            gpu.IndexCount = static_cast<uint32_t>(cpu.Indices.size());
            gpu.MaterialIndex = std::min<uint32_t>(cpu.MaterialIndex, static_cast<uint32_t>(model.GpuMaterials.size() - 1));
            model.GpuPrimitives.push_back(std::move(gpu));
        }

        CreatePipeline(model);
        model.Uploaded = model.Pso != nullptr;
        if (!model.Uploaded) {
            RADRAY_ERR_LOG("failed to upload glTF '{}': graphics pipeline was not created", model.Path);
            return false;
        }
        RADRAY_INFO_LOG(
            "glTF uploaded '{}': gpuPrimitives={}, gpuMaterials={}, gpuTextures={}",
            model.Path,
            model.GpuPrimitives.size(),
            model.GpuMaterials.size(),
            model.GpuTextures.size());
        return model.Uploaded;
    }

    void CreateDefaultTextures(GltfModel& model) {
        GltfTextureCpu white;
        white.Name = "Default White";
        white.Image = MakeSolidImage(255, 255, 255, 255);
        white.Sampler = DefaultSampler();
        _defaultWhiteTexture = static_cast<int>(model.CpuTextures.size());
        model.CpuTextures.push_back(std::move(white));

        GltfTextureCpu normal;
        normal.Name = "Default Normal";
        normal.Image = MakeSolidImage(128, 128, 255, 255);
        normal.Sampler = DefaultSampler();
        _defaultNormalTexture = static_cast<int>(model.CpuTextures.size());
        model.CpuTextures.push_back(std::move(normal));
    }

    void WriteMaterialTextureBindings(GltfMaterialGpu& gpu, const GltfMaterialCpu& cpu, const GltfModel& model) {
        auto resolve = [&](int index, int fallback) -> const GltfTextureGpu& {
            const int resolved = (index >= 0 && index < static_cast<int>(model.GpuTextures.size())) ? index : fallback;
            return model.GpuTextures[static_cast<size_t>(resolved)];
        };
        const GltfTextureGpu& base = resolve(cpu.BaseColorTexture, _defaultWhiteTexture);
        const GltfTextureGpu& mr = resolve(cpu.MetallicRoughnessTexture, _defaultWhiteTexture);
        const GltfTextureGpu& normal = resolve(cpu.NormalTexture, _defaultNormalTexture);
        gpu.DescriptorSet->WriteResource("gBaseColorTexture", base.View.get());
        gpu.DescriptorSet->WriteResource("gMetallicRoughnessTexture", mr.View.get());
        gpu.DescriptorSet->WriteResource("gNormalTexture", normal.View.get());
        gpu.DescriptorSet->WriteSampler("gSampler", base.Sampler.get());
    }

    void CreatePipeline(GltfModel& model) {
        if (model.Pso != nullptr || _rootSig == nullptr) {
            return;
        }
        _vertexElements = {
            render::VertexElement{offsetof(GltfVertex, Position), "POSITION", 0, render::VertexFormat::FLOAT32X3, 0},
            render::VertexElement{offsetof(GltfVertex, Normal), "NORMAL", 0, render::VertexFormat::FLOAT32X3, 1},
            render::VertexElement{offsetof(GltfVertex, Tangent), "TANGENT", 0, render::VertexFormat::FLOAT32X4, 2},
            render::VertexElement{offsetof(GltfVertex, TexCoord), "TEXCOORD", 0, render::VertexFormat::FLOAT32X2, 3},
        };
        render::VertexBufferLayout layout{
            sizeof(GltfVertex),
            render::VertexStepMode::Vertex,
            _vertexElements};
        render::ColorTargetState color = render::ColorTargetState::Default(BackBufferFormat);
        render::DepthStencilState depth = render::DepthStencilState::Default();
        depth.Format = DepthFormat;
        render::PrimitiveState primitive = render::PrimitiveState::Default();
        primitive.Cull = render::CullMode::None;
        render::GraphicsPipelineStateDescriptor desc{
            _rootSig.get(),
            render::ShaderEntry{_vs, "VSMain"},
            render::ShaderEntry{_ps, "PSMain"},
            std::span<const render::VertexBufferLayout>{&layout, 1},
            primitive,
            depth,
            render::MultiSampleState::Default(),
            std::span<const render::ColorTargetState>{&color, 1}};
        auto psoOpt = GetDevice()->CreateGraphicsPipelineState(desc);
        if (psoOpt.HasValue()) {
            model.Pso = psoOpt.Release();
        }
    }

    RenderPipeline _pipeline;
    RenderResourcePool _resourcePool;
    GltfScenePass* _scenePass{nullptr};  // 非 owning,由 _pipeline 持有;每帧注入 viewer 状态。
    ViewerSceneView _view;
    unique_ptr<GltfModel> _model;
    struct RetiredModel {
        unique_ptr<GltfModel> Model;
        uint32_t RemainingFrameCompletions{0};
    };
    vector<RetiredModel> _retiredModels;
    mutable std::mutex _modelMutex;
    unique_ptr<render::RootSignature> _rootSig;
    render::Shader* _vs{nullptr};
    render::Shader* _ps{nullptr};
    vector<render::VertexElement> _vertexElements;

    vector<char> _loadPath;
    string _loadError;
    std::filesystem::path _initialLoadPath;
    bool _pendingLoad{false};
    std::filesystem::path _pendingLoadPath;
    int _selectedNode{-1};
    GltfDebugMode _debugMode{GltfDebugMode::Shaded};

    CameraControl _cameraControl;
    Eigen::Vector3f _cameraTarget{Eigen::Vector3f::Zero()};
    float _cameraDistance{4.0f};
    float _cameraYaw{0.0f};
    float _cameraPitch{Radian(20.0f)};
    Eigen::Vector3f _cameraPosition{0.0f, 0.0f, 4.0f};
    Eigen::Quaternionf _cameraRotation{Eigen::Quaternionf::Identity()};
    NativeWindow* _mainNativeWindow{nullptr};
    vector<sigslot::scoped_connection> _inputConnections;
    bool _cameraInputCapturedByUi{false};
    bool _cameraPollAnyButtonDown{false};
    Eigen::Vector2f _lastCameraPollMousePos{Eigen::Vector2f::Zero()};
    int _defaultWhiteTexture{-1};
    int _defaultNormalTexture{-1};
};

int main(int argc, char* argv[]) {
    ApplicationRuntimeDescriptor desc{};
    desc.Backend = render::RenderBackend::Vulkan;
    desc.AppName = "RadRay glTF Viewer";
    desc.EngineName = "RadRay";
    desc.WindowTitle = "RadRay glTF Viewer";
    desc.WindowWidth = 1280;
    desc.WindowHeight = 720;
    desc.BackBufferFormat = GltfViewerApp::BackBufferFormat;
    desc.PresentMode = render::PresentMode::FIFO;

    std::optional<std::filesystem::path> loadPath;
    GltfDebugMode debugMode = GltfDebugMode::Shaded;
    for (int i = 0; i < argc; ++i) {
        std::string_view arg{argv[i]};
        if (arg == "--backend" && i + 1 < argc) {
            std::string_view backendStr{argv[++i]};
            if (backendStr == "vulkan") {
                desc.Backend = render::RenderBackend::Vulkan;
            } else if (backendStr == "d3d12") {
                desc.Backend = render::RenderBackend::D3D12;
            }
        } else if (arg == "--valid-layer") {
            desc.EnableValidation = true;
        } else if (arg == "--multithread") {
            desc.Multithreaded = true;
        } else if (arg == "--debug-shader") {
            debugMode = GltfDebugMode::GeometryNormal;
        } else if (arg == "--load" && i + 1 < argc) {
            loadPath = std::filesystem::path{argv[++i]};
        }
    }

    GltfViewerApp app{};
    app.SetDebugMode(debugMode);
    if (loadPath.has_value()) {
        app.SetInitialLoadPath(loadPath.value());
    }
    app.RegisterSubsystem<ImGuiSystem>();
    return app.Run(desc);
}
