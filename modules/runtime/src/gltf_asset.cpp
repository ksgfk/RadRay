#include <radray/runtime/gltf_asset.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <span>
#include <string_view>

#include <radray/logger.h>
#include <radray/basic_math.h>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/components/scene_component.h>
#include <radray/runtime/components/static_mesh_component.h>
#include <radray/runtime/game_framework/actor.h>
#include <radray/runtime/game_framework/world.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/renderer/primitive_scene_proxy.h>
#include <radray/scope_guard.h>
#include <radray/vertex_data.h>

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

namespace radray {
namespace {

struct GltfVertex {
    Eigen::Vector3f Position{Eigen::Vector3f::Zero()};
    Eigen::Vector3f Normal{Eigen::Vector3f::UnitY()};
    Eigen::Vector2f TexCoord{Eigen::Vector2f::Zero()};
    Eigen::Vector4f Tangent{1.0f, 0.0f, 0.0f, 1.0f};
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
};

struct GltfPrimitiveCpu {
    string Name;
    vector<GltfVertex> Vertices;
    vector<uint32_t> Indices;
    uint32_t MaterialIndex{0};
    vector<MaterialParameterAssignment> MaterialParams;
    Eigen::Vector3f BoundsMin{Eigen::Vector3f::Zero()};
    Eigen::Vector3f BoundsMax{Eigen::Vector3f::Zero()};
    bool HasBounds{false};
};

struct ParsedGltf {
    vector<GltfNodeDesc> Nodes;
    vector<int> RootNodes;
    vector<GltfPrimitiveDesc> Primitives;
    vector<string> MaterialNames;
    vector<GltfMaterialCpu> Materials;
    vector<GltfImageDesc> Images;
    Eigen::Vector3f BoundsMin{Eigen::Vector3f::Zero()};
    Eigen::Vector3f BoundsMax{Eigen::Vector3f::Zero()};
    bool HasBounds{false};
};

uint64_t StableHash64(std::string_view text) noexcept {
    uint64_t hash = 1469598103934665603ull;
    for (unsigned char ch : text) {
        hash ^= static_cast<uint64_t>(ch);
        hash *= 1099511628211ull;
    }
    return hash;
}

AssetId MakeDerivedAssetId(const std::filesystem::path& path, std::string_view tag, uint32_t index) {
    const string key = fmt::format("{}:{}:{}", std::filesystem::absolute(path).generic_string(), tag, index);
    std::array<uint8_t, Guid::Size> bytes{};
    uint64_t h0 = StableHash64(key);
    uint64_t h1 = StableHash64(fmt::format("{}:salt", key));
    for (size_t i = 0; i < 8; ++i) {
        bytes[i] = static_cast<uint8_t>((h0 >> ((7 - i) * 8)) & 0xffu);
        bytes[i + 8] = static_cast<uint8_t>((h1 >> ((7 - i) * 8)) & 0xffu);
    }
    bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0fu) | 0x40u);
    bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3fu) | 0x80u);
    return AssetId{bytes};
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

void UpdateBounds(Eigen::Vector3f& boundsMin, Eigen::Vector3f& boundsMax, bool& hasBounds, const Eigen::Vector3f& p) {
    if (!hasBounds) {
        boundsMin = p;
        boundsMax = p;
        hasBounds = true;
        return;
    }
    boundsMin = boundsMin.cwiseMin(p);
    boundsMax = boundsMax.cwiseMax(p);
}

void UpdateBoundsWithTransformedBox(
    Eigen::Vector3f& boundsMin,
    Eigen::Vector3f& boundsMax,
    bool& hasBounds,
    const Eigen::Vector3f& localMin,
    const Eigen::Vector3f& localMax,
    const Eigen::Matrix4f& transform) {
    for (uint32_t corner = 0; corner < 8; ++corner) {
        Eigen::Vector3f local{
            (corner & 1u) != 0 ? localMax.x() : localMin.x(),
            (corner & 2u) != 0 ? localMax.y() : localMin.y(),
            (corner & 4u) != 0 ? localMax.z() : localMin.z()};
        Eigen::Vector4f world = transform * Eigen::Vector4f{local.x(), local.y(), local.z(), 1.0f};
        UpdateBounds(boundsMin, boundsMax, hasBounds, world.head<3>());
    }
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

        const Eigen::Vector3f p0 = vertices[i0].Position;
        const Eigen::Vector3f p1 = vertices[i1].Position;
        const Eigen::Vector3f p2 = vertices[i2].Position;
        const Eigen::Vector2f uv0 = vertices[i0].TexCoord;
        const Eigen::Vector2f uv1 = vertices[i1].TexCoord;
        const Eigen::Vector2f uv2 = vertices[i2].TexCoord;

        const Eigen::Vector3f e1 = p1 - p0;
        const Eigen::Vector3f e2 = p2 - p0;
        const Eigen::Vector2f d1 = uv1 - uv0;
        const Eigen::Vector2f d2 = uv2 - uv0;
        const float denom = d1.x() * d2.y() - d1.y() * d2.x();
        if (std::abs(denom) <= 1e-8f) {
            continue;
        }
        const float r = 1.0f / denom;
        const Eigen::Vector3f t = (e1 * d2.y() - e2 * d1.y()) * r;
        const Eigen::Vector3f b = (e2 * d1.x() - e1 * d2.x()) * r;
        tangents[i0] += t;
        tangents[i1] += t;
        tangents[i2] += t;
        bitangents[i0] += b;
        bitangents[i1] += b;
        bitangents[i2] += b;
    }

    for (size_t i = 0; i < vertices.size(); ++i) {
        GltfVertex& vertex = vertices[i];
        Eigen::Vector3f n = vertex.Normal.squaredNorm() > 1e-8f ? vertex.Normal.normalized() : Eigen::Vector3f::UnitY();
        Eigen::Vector3f t = tangents[i] - n * n.dot(tangents[i]);
        if (t.squaredNorm() <= 1e-8f) {
            t = MakeFallbackTangent(n);
        } else {
            t.normalize();
        }
        const float handedness = n.cross(t).dot(bitangents[i]) < 0.0f ? -1.0f : 1.0f;
        vertex.Normal = n;
        vertex.Tangent = Eigen::Vector4f{t.x(), t.y(), t.z(), handedness};
    }
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

StreamingAssetRef<ImageAsset> LoadCgltfImage(
    AssetManager& assetManager,
    const std::filesystem::path& gltfPath,
    const cgltf_image& image,
    uint32_t imageIndex) {
    const string name = image.name != nullptr ? image.name : fmt::format("Image {}", imageIndex);
    const ImageAssetLoadOptions options{
        .ConvertToRgba8 = true,
        .FallbackImage = MakeSolidImage(255, 255, 255, 255)};

    if (image.buffer_view != nullptr && image.buffer_view->buffer != nullptr && image.buffer_view->buffer->data != nullptr) {
        const auto* bytes = static_cast<const byte*>(image.buffer_view->buffer->data) + image.buffer_view->offset;
        vector<byte> encodedBytes(image.buffer_view->size);
        if (!encodedBytes.empty()) {
            std::memcpy(encodedBytes.data(), bytes, encodedBytes.size());
        }
        return LoadImageAssetFromMemory(
            assetManager,
            MakeDerivedAssetId(gltfPath, "image", imageIndex),
            name,
            std::move(encodedBytes),
            options);
    }
    if (image.uri == nullptr || std::strlen(image.uri) == 0) {
        return LoadImageAssetFromMemory(
            assetManager,
            MakeDerivedAssetId(gltfPath, "image", imageIndex),
            name,
            {},
            options);
    }
    std::string_view uri{image.uri};
    if (uri.starts_with("data:")) {
        const size_t comma = uri.find(',');
        if (comma == std::string_view::npos) {
            return LoadImageAssetFromMemory(
                assetManager,
                MakeDerivedAssetId(gltfPath, "image", imageIndex),
                name,
                {},
                options);
        }
        auto decoded = DecodeBase64(uri.substr(comma + 1));
        if (!decoded.has_value()) {
            decoded = vector<byte>{};
        }
        return LoadImageAssetFromMemory(
            assetManager,
            MakeDerivedAssetId(gltfPath, "image", imageIndex),
            name,
            std::move(decoded.value()),
            options);
    }
    const std::filesystem::path imagePath = gltfPath.parent_path() / std::filesystem::path{string{uri}};
    return LoadImageAsset(assetManager, imagePath, options);
}

GltfMaterialCpu ConvertMaterial(const cgltf_material& src) {
    GltfMaterialCpu out;
    if (src.name != nullptr && src.name[0] != '\0') {
        out.Name = src.name;
    }
    if (src.has_pbr_metallic_roughness) {
        const cgltf_pbr_metallic_roughness& pbr = src.pbr_metallic_roughness;
        out.BaseColorFactor = Eigen::Vector4f{
            pbr.base_color_factor[0],
            pbr.base_color_factor[1],
            pbr.base_color_factor[2],
            pbr.base_color_factor[3]};
        out.Metallic = pbr.metallic_factor;
        out.Roughness = pbr.roughness_factor;
    } else if (src.has_pbr_specular_glossiness) {
        const cgltf_pbr_specular_glossiness& pbr = src.pbr_specular_glossiness;
        out.BaseColorFactor = Eigen::Vector4f{
            pbr.diffuse_factor[0],
            pbr.diffuse_factor[1],
            pbr.diffuse_factor[2],
            pbr.diffuse_factor[3]};
        out.Roughness = 1.0f - pbr.glossiness_factor;
        out.Metallic = 0.0f;
    }
    if (src.has_ior) {
        out.Eta = src.ior.ior;
    }
    if (src.has_specular) {
        out.Specular = src.specular.specular_factor;
        out.SpecularTint = std::max({
            src.specular.specular_color_factor[0],
            src.specular.specular_color_factor[1],
            src.specular.specular_color_factor[2]});
    }
    if (src.has_clearcoat) {
        out.Clearcoat = src.clearcoat.clearcoat_factor;
        out.ClearcoatGloss = 1.0f - src.clearcoat.clearcoat_roughness_factor;
    }
    if (src.has_sheen) {
        out.Sheen = std::max({
            src.sheen.sheen_color_factor[0],
            src.sheen.sheen_color_factor[1],
            src.sheen.sheen_color_factor[2]});
        out.SheenTint = out.Sheen > 0.0f ? 1.0f : 0.0f;
    }
    if (src.has_transmission) {
        out.SpecTrans = src.transmission.transmission_factor;
    }
    if (src.has_anisotropy) {
        out.Anisotropic = src.anisotropy.anisotropy_strength;
    }
    out.EmissiveFactor = Eigen::Vector3f{src.emissive_factor[0], src.emissive_factor[1], src.emissive_factor[2]};
    if (src.has_emissive_strength) {
        out.EmissiveFactor *= src.emissive_strength.emissive_strength;
    }
    out.AlphaCutoff = src.alpha_cutoff;
    return out;
}

// 把 glTF 材质转成 per-使用点材质参数(按名写入 gMaterial cbuffer)。
// 字段名与布局与原来烘进顶点的 5 个 float4 一一对应,保证颜色不变。
vector<MaterialParameterAssignment> BuildMaterialParams(const GltfMaterialCpu& material) {
    vector<MaterialParameterAssignment> params;
    params.reserve(5);
    params.push_back(MaterialParameterAssignment{
        "BaseColorFactor",
        material.BaseColorFactor});
    params.push_back(MaterialParameterAssignment{
        "EmissiveFactorAlphaCutoff",
        Eigen::Vector4f{
            material.EmissiveFactor.x(),
            material.EmissiveFactor.y(),
            material.EmissiveFactor.z(),
            material.AlphaCutoff}});
    params.push_back(MaterialParameterAssignment{
        "Principled0",
        Eigen::Vector4f{
            material.Metallic,
            material.Roughness,
            material.Specular,
            material.SpecularTint}});
    params.push_back(MaterialParameterAssignment{
        "Principled1",
        Eigen::Vector4f{
            material.Anisotropic,
            material.Sheen,
            material.SheenTint,
            material.Flatness}});
    params.push_back(MaterialParameterAssignment{
        "Principled2",
        Eigen::Vector4f{
            material.Clearcoat,
            material.ClearcoatGloss,
            material.SpecTrans,
            material.Eta}});
    return params;
}

void DecomposeNodeTransform(const cgltf_node& node, GltfNodeDesc& out) {
    cgltf_float translation[3] = {0.0f, 0.0f, 0.0f};
    cgltf_float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    cgltf_float scale[3] = {1.0f, 1.0f, 1.0f};

    if (node.has_translation) {
        std::memcpy(translation, node.translation, sizeof(translation));
    }
    if (node.has_rotation) {
        std::memcpy(rotation, node.rotation, sizeof(rotation));
    }
    if (node.has_scale) {
        std::memcpy(scale, node.scale, sizeof(scale));
    }
    if (node.has_matrix) {
        Eigen::Matrix4f matrix;
        for (int col = 0; col < 4; ++col) {
            for (int row = 0; row < 4; ++row) {
                matrix(row, col) = node.matrix[col * 4 + row];
            }
        }
        DecomposeTransform<float>(matrix, out.Translation, out.Rotation, out.Scale);
        out.Rotation.normalize();
        return;
    }

    out.Translation = Eigen::Vector3f{translation[0], translation[1], translation[2]};
    out.Rotation = Eigen::Quaternionf{rotation[3], rotation[0], rotation[1], rotation[2]};
    out.Rotation.normalize();
    out.Scale = Eigen::Vector3f{scale[0], scale[1], scale[2]};
}

std::optional<GltfPrimitiveCpu> ConvertPrimitive(
    const cgltf_data* data,
    const cgltf_primitive& primitive,
    std::span<const GltfMaterialCpu> materials,
    std::string_view name) {
    if (primitive.type != cgltf_primitive_type_triangles) {
        return std::nullopt;
    }

    const cgltf_accessor* positionAcc = FindAccessor(primitive, cgltf_attribute_type_position, 0);
    if (positionAcc == nullptr || positionAcc->count == 0) {
        return std::nullopt;
    }

    const cgltf_accessor* normalAcc = FindAccessor(primitive, cgltf_attribute_type_normal, 0);
    const cgltf_accessor* tangentAcc = FindAccessor(primitive, cgltf_attribute_type_tangent, 0);
    const cgltf_accessor* texcoordAcc = FindAccessor(primitive, cgltf_attribute_type_texcoord, 0);

    GltfPrimitiveCpu out;
    out.Name = string{name};
    if (primitive.material != nullptr) {
        out.MaterialIndex = static_cast<uint32_t>(cgltf_material_index(data, primitive.material));
    }
    GltfMaterialCpu defaultMaterial{};
    const GltfMaterialCpu& material = out.MaterialIndex < materials.size()
        ? materials[out.MaterialIndex]
        : defaultMaterial;
    out.MaterialParams = BuildMaterialParams(material);
    out.Vertices.resize(positionAcc->count);
    for (cgltf_size i = 0; i < positionAcc->count; ++i) {
        GltfVertex vertex{};
        ReadVec3(positionAcc, i, vertex.Position);
        if (normalAcc != nullptr) {
            ReadVec3(normalAcc, i, vertex.Normal);
            if (vertex.Normal.squaredNorm() > 1e-8f) {
                vertex.Normal.normalize();
            }
        }
        if (texcoordAcc != nullptr) {
            ReadVec2(texcoordAcc, i, vertex.TexCoord);
        }
        if (tangentAcc != nullptr) {
            ReadVec4(tangentAcc, i, vertex.Tangent);
            Eigen::Vector3f t = vertex.Tangent.head<3>();
            if (t.squaredNorm() > 1e-8f) {
                t.normalize();
                vertex.Tangent = Eigen::Vector4f{t.x(), t.y(), t.z(), vertex.Tangent.w()};
            }
        }
        out.Vertices[i] = vertex;
        UpdateBounds(out.BoundsMin, out.BoundsMax, out.HasBounds, vertex.Position);
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
    return out;
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

MeshResource MakeMeshResource(const GltfPrimitiveCpu& primitive) {
    MeshResource mesh;
    mesh.Name = primitive.Name;

    vector<byte> vertexData = ToBytes(primitive.Vertices);
    vector<byte> indexData = ToBytes(primitive.Indices);

    MeshPrimitive meshPrimitive{};
    meshPrimitive.VertexCount = static_cast<uint32_t>(primitive.Vertices.size());
    meshPrimitive.VertexBuffers = {
        VertexBufferEntry{
            .Semantic = string{VertexSemantics::POSITION},
            .SemanticIndex = 0,
            .BufferIndex = 0,
            .Type = VertexDataType::FLOAT,
            .ComponentCount = 3,
            .Offset = static_cast<uint32_t>(offsetof(GltfVertex, Position)),
            .Stride = sizeof(GltfVertex)},
        VertexBufferEntry{
            .Semantic = string{VertexSemantics::NORMAL},
            .SemanticIndex = 0,
            .BufferIndex = 0,
            .Type = VertexDataType::FLOAT,
            .ComponentCount = 3,
            .Offset = static_cast<uint32_t>(offsetof(GltfVertex, Normal)),
            .Stride = sizeof(GltfVertex)},
        VertexBufferEntry{
            .Semantic = string{VertexSemantics::TEXCOORD},
            .SemanticIndex = 0,
            .BufferIndex = 0,
            .Type = VertexDataType::FLOAT,
            .ComponentCount = 2,
            .Offset = static_cast<uint32_t>(offsetof(GltfVertex, TexCoord)),
            .Stride = sizeof(GltfVertex)},
        VertexBufferEntry{
            .Semantic = string{VertexSemantics::TANGENT},
            .SemanticIndex = 0,
            .BufferIndex = 0,
            .Type = VertexDataType::FLOAT,
            .ComponentCount = 4,
            .Offset = static_cast<uint32_t>(offsetof(GltfVertex, Tangent)),
            .Stride = sizeof(GltfVertex)},
    };
    meshPrimitive.IndexBuffer.BufferIndex = 1;
    meshPrimitive.IndexBuffer.IndexCount = static_cast<uint32_t>(primitive.Indices.size());
    meshPrimitive.IndexBuffer.Offset = 0;
    meshPrimitive.IndexBuffer.Stride = sizeof(uint32_t);

    mesh.Bins.emplace_back(std::span<const byte>{vertexData.data(), vertexData.size()});
    mesh.Bins.emplace_back(std::span<const byte>{indexData.data(), indexData.size()});
    mesh.Primitives.emplace_back(std::move(meshPrimitive));
    return mesh;
}

Eigen::Matrix4f ComputeNodeWorldMatrix(const ParsedGltf& parsed, int nodeIndex) {
    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(parsed.Nodes.size())) {
        return Eigen::Matrix4f::Identity();
    }
    const GltfNodeDesc& node = parsed.Nodes[static_cast<size_t>(nodeIndex)];
    const Eigen::Matrix4f local = ComposeTransform<float>(node.Translation, node.Rotation, node.Scale);
    if (node.Parent < 0) {
        return local;
    }
    return ComputeNodeWorldMatrix(parsed, node.Parent) * local;
}

}  // namespace

GltfAsset::GltfAsset(
    std::filesystem::path path,
    vector<GltfNodeDesc> nodes,
    vector<int> rootNodes,
    vector<GltfPrimitiveDesc> primitives,
    vector<string> materialNames,
    vector<GltfImageDesc> images,
    Eigen::Vector3f boundsMin,
    Eigen::Vector3f boundsMax,
    bool hasBounds) noexcept
    : _path(std::move(path)),
      _nodes(std::move(nodes)),
      _rootNodes(std::move(rootNodes)),
      _primitives(std::move(primitives)),
      _materialNames(std::move(materialNames)),
      _images(std::move(images)),
      _boundsMin(boundsMin),
      _boundsMax(boundsMax),
      _hasBounds(hasBounds) {
}

GltfAsset::~GltfAsset() noexcept = default;

void GltfAsset::OnUnload() {
    _nodes.clear();
    _rootNodes.clear();
    _primitives.clear();
    _materialNames.clear();
    _images.clear();
    _boundsMin = Eigen::Vector3f::Zero();
    _boundsMax = Eigen::Vector3f::Zero();
    _hasBounds = false;
}

AssetTypeId GltfAsset::GetTypeId() const noexcept {
    return runtime_type_id_v<GltfAsset>;
}

Actor* GltfAsset::ExportToScene(World& world) const {
    Actor* rootActor = world.SpawnActor<Actor>();
    SceneComponent* root = rootActor->AddComponent<SceneComponent>();
    rootActor->SetRootComponent(root);

    for (const GltfPrimitiveDesc& primitive : _primitives) {
        if (primitive.NodeIndex < 0 || primitive.NodeIndex >= static_cast<int>(_nodes.size())) {
            continue;
        }
        Actor* meshActor = world.SpawnActor<Actor>();
        StaticMeshComponent* component = meshActor->AddComponent<StaticMeshComponent>();
        component->SetStaticMesh(primitive.Mesh);
        component->SetMaterial(primitive.Material);
        component->SetMaterialParams(primitive.MaterialParams);
        meshActor->SetRootComponent(component);
        component->AttachTo(root);

        const Eigen::Matrix4f& worldMatrix = _nodes[static_cast<size_t>(primitive.NodeIndex)].WorldMatrix;
        Eigen::Vector3f translation;
        Eigen::Quaternionf rotation;
        Eigen::Vector3f scale;
        DecomposeTransform<float>(worldMatrix, translation, rotation, scale);
        component->SetRelativeLocation(translation);
        component->SetRelativeRotation(rotation);
        component->SetRelativeScale(scale);
        component->TickComponent(0.0f);
    }

    return rootActor;
}

StreamingAssetRef<GltfAsset> LoadGltfAsset(
    AssetManager& assetManager,
    FrameUploadScheduler& frameUploads,
    const std::filesystem::path& path,
    const GltfAssetLoadOptions& options) {
    const AssetId assetId = MakeDerivedAssetId(path, "gltf", 0);
    return assetManager.Load<GltfAsset>(AssetLoadRequest{
        .Id = assetId,
        .Task = [](
            AssetManager& assetManager,
            FrameUploadScheduler& frameUploads,
            std::filesystem::path path,
            GltfAssetLoadOptions options) -> AssetLoadTask {
            ParsedGltf parsed{};

            cgltf_options cgltfOptions{};
            cgltf_data* data = nullptr;
            cgltf_result parseResult = cgltf_parse_file(&cgltfOptions, path.string().c_str(), &data);
            if (parseResult != cgltf_result_success || data == nullptr) {
                co_return AssetLoadResult::Failure(fmt::format("cgltf_parse_file failed: {}", static_cast<int>(parseResult)));
            }
            auto guard = MakeScopeGuard([&]() {
                cgltf_free(data);
            });

            parseResult = cgltf_load_buffers(&cgltfOptions, data, path.string().c_str());
            if (parseResult != cgltf_result_success) {
                co_return AssetLoadResult::Failure(fmt::format("cgltf_load_buffers failed: {}", static_cast<int>(parseResult)));
            }

            parseResult = cgltf_validate(data);
            if (parseResult != cgltf_result_success) {
                co_return AssetLoadResult::Failure(fmt::format("cgltf_validate failed: {}", static_cast<int>(parseResult)));
            }

            parsed.Images.reserve(data->images_count);
            for (cgltf_size i = 0; i < data->images_count; ++i) {
                GltfImageDesc imageDesc{};
                imageDesc.Name = data->images[i].name != nullptr ? data->images[i].name : fmt::format("Image {}", i);
                imageDesc.Image = LoadCgltfImage(assetManager, path, data->images[i], static_cast<uint32_t>(i));
                parsed.Images.push_back(std::move(imageDesc));
            }

            parsed.MaterialNames.reserve(std::max<cgltf_size>(1, data->materials_count));
            parsed.Materials.reserve(std::max<cgltf_size>(1, data->materials_count));
            for (cgltf_size i = 0; i < data->materials_count; ++i) {
                GltfMaterialCpu material = ConvertMaterial(data->materials[i]);
                if (material.Name == "Default") {
                    material.Name = fmt::format("Material {}", i);
                }
                parsed.MaterialNames.push_back(material.Name);
                parsed.Materials.push_back(std::move(material));
            }
            if (parsed.MaterialNames.empty()) {
                parsed.MaterialNames.push_back("Default");
                parsed.Materials.push_back(GltfMaterialCpu{});
            }

            parsed.Nodes.resize(data->nodes_count);
            for (cgltf_size i = 0; i < data->nodes_count; ++i) {
                const cgltf_node& node = data->nodes[i];
                GltfNodeDesc& out = parsed.Nodes[i];
                out.Name = node.name != nullptr ? node.name : fmt::format("Node {}", i);
                out.Parent = node.parent != nullptr ? static_cast<int>(cgltf_node_index(data, node.parent)) : -1;
                out.HasMesh = node.mesh != nullptr;
                DecomposeNodeTransform(node, out);
                if (out.Parent < 0) {
                    parsed.RootNodes.push_back(static_cast<int>(i));
                }
                for (cgltf_size c = 0; c < node.children_count; ++c) {
                    out.Children.push_back(static_cast<int>(cgltf_node_index(data, node.children[c])));
                }
            }

            for (size_t i = 0; i < parsed.Nodes.size(); ++i) {
                parsed.Nodes[i].WorldMatrix = ComputeNodeWorldMatrix(parsed, static_cast<int>(i));
            }

            uint32_t primitiveIndex = 0;
            for (cgltf_size n = 0; n < data->nodes_count; ++n) {
                const cgltf_node& node = data->nodes[n];
                if (node.mesh == nullptr) {
                    continue;
                }
                for (cgltf_size p = 0; p < node.mesh->primitives_count; ++p) {
                    const string primitiveName = fmt::format("{} / prim {}", parsed.Nodes[n].Name, p);
                    std::optional<GltfPrimitiveCpu> primitive = ConvertPrimitive(
                        data,
                        node.mesh->primitives[p],
                        parsed.Materials,
                        primitiveName);
                    if (!primitive.has_value()) {
                        continue;
                    }

                    MeshResource meshResource = MakeMeshResource(primitive.value());
                    AssetId meshId = MakeDerivedAssetId(path, "mesh", primitiveIndex);
                    StreamingAssetRef<StaticMesh> mesh = assetManager.Load<StaticMesh>(AssetLoadRequest{
                        .Id = meshId,
                        .Task = LoadStaticMesh(frameUploads, std::move(meshResource)),
                        .DebugName = primitiveName});

                    GltfPrimitiveDesc desc{};
                    desc.Name = primitiveName;
                    desc.NodeIndex = static_cast<int>(n);
                    desc.SourceMaterialIndex = primitive->MaterialIndex;
                    desc.Mesh = mesh;
                    desc.Material = options.DefaultMaterial.CastTo<Material>();
                    desc.MaterialParams = std::move(primitive->MaterialParams);
                    desc.BoundsMin = primitive->BoundsMin;
                    desc.BoundsMax = primitive->BoundsMax;
                    desc.HasBounds = primitive->HasBounds;
                    parsed.Primitives.push_back(std::move(desc));

                    if (primitive->HasBounds) {
                        const Eigen::Matrix4f& nodeWorld = parsed.Nodes[static_cast<size_t>(n)].WorldMatrix;
                        UpdateBoundsWithTransformedBox(
                            parsed.BoundsMin,
                            parsed.BoundsMax,
                            parsed.HasBounds,
                            primitive->BoundsMin,
                            primitive->BoundsMax,
                            nodeWorld);
                    }
                    ++primitiveIndex;
                }
            }

            if (parsed.Primitives.empty()) {
                co_return AssetLoadResult::Failure("glTF has no supported triangle primitives");
            }

            auto asset = make_unique<GltfAsset>(
                path,
                std::move(parsed.Nodes),
                std::move(parsed.RootNodes),
                std::move(parsed.Primitives),
                std::move(parsed.MaterialNames),
                std::move(parsed.Images),
                parsed.BoundsMin,
                parsed.BoundsMax,
                parsed.HasBounds);

            RADRAY_INFO_LOG(
                "glTF asset loaded '{}': primitives={}, materials={}, images={}, nodes={}",
                path.string(),
                asset->GetPrimitives().size(),
                asset->GetMaterialNames().size(),
                asset->GetImages().size(),
                asset->GetNodes().size());

            co_return AssetLoadResult::Success(std::move(asset));
        }(assetManager, frameUploads, path, options),
        .DebugName = path.string()});
}

}  // namespace radray
