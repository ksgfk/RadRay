#include <radray/runtime/gltf_asset.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <span>
#include <string_view>
#include <utility>

#include <radray/logger.h>
#include <radray/scope_guard.h>
#include <radray/vertex_data.h>
#include <radray/runtime/components/scene_component.h>
#include <radray/runtime/components/static_mesh_component.h>
#include <radray/runtime/game_framework/actor.h>
#include <radray/runtime/game_framework/world.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/image_asset.h>
#include <radray/runtime/material_asset.h>

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

namespace radray {
namespace {

// 交错顶点布局: POSITION3f + NORMAL3f + TEXCOORD2f + TANGENT4f。
// 与 gltf_standard_pass.hlsl 的 VertexInput 及 ToSimpleMeshResource 顺序一致。
struct GltfVertex {
    Eigen::Vector3f Position{Eigen::Vector3f::Zero()};
    Eigen::Vector3f Normal{Eigen::Vector3f::UnitY()};
    Eigen::Vector2f TexCoord{Eigen::Vector2f::Zero()};
    Eigen::Vector4f Tangent{1.0f, 0.0f, 0.0f, 1.0f};
};

struct GltfPrimitiveCpu {
    string Name;
    vector<GltfVertex> Vertices;
    vector<uint32_t> Indices;
    uint32_t MaterialIndex{0};
    Eigen::Vector3f BoundsMin{Eigen::Vector3f::Zero()};
    Eigen::Vector3f BoundsMax{Eigen::Vector3f::Zero()};
    bool HasBounds{false};
};

struct PackedPrimitiveSection {
    uint32_t FirstIndex{0};
    uint32_t IndexCount{0};
    uint32_t MinVertexIndex{0};
    uint32_t MaxVertexIndex{0};
    int32_t VertexOffset{0};
};

// ── 稳定 AssetId 派生 (同一 glTF 的同一子资源始终得到同一 Id, 便于缓存去重) ──

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

// ── RH->LH 转换 ──
// glTF 使用右手系 (+Z 朝向观察者), RadRay 使用左手系 (+Z 朝向屏幕内)。
// 以反射矩阵 S = diag(1,1,-1) 完成 RH->LH: 只翻转 Z, 保持上/左右朝向, 避免镜像。
// S 自逆且对称, 故方向量 (法线/切线) 与点一样只翻转 Z。

Eigen::Vector3f ReflectZToLH(const Eigen::Vector3f& v) noexcept {
    return Eigen::Vector3f{v.x(), v.y(), -v.z()};
}

// 切线 (xyz, w): xyz 反射 Z; w 是 TBN 手性符号, 反射使叉乘反号, 需翻转 w 保持副切线方向。
Eigen::Vector4f ReflectTangentToLH(const Eigen::Vector4f& t) noexcept {
    return Eigen::Vector4f{t.x(), t.y(), -t.z(), -t.w()};
}

// 旋转四元数: 反射共轭 S*R*S 等价于 (w,x,y,z) -> (w,-x,-y,z)。
Eigen::Quaternionf ReflectRotationToLH(const Eigen::Quaternionf& q) noexcept {
    return Eigen::Quaternionf{q.w(), -q.x(), -q.y(), q.z()};
}

// 反射 (det=-1) 翻转三角形绕序: glTF RH 下 CCW 为正面, 反射后变 CW,
// 恰好匹配 RadRay 默认 FrontFace::CW + CullMode::Back。交换后两个索引恢复绕序。
void FlipTriangleWindingToLH(std::span<uint32_t> indices) noexcept {
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        std::swap(indices[i + 1], indices[i + 2]);
    }
}

// ── accessor 读取工具 ──

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

// ── fallback 切线生成 (无 TANGENT 属性时) ──

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

// ── 图像字节提取 (buffer_view / data URI / 外部文件) ──

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

std::optional<vector<byte>> ExtractImageEncodedBytes(
    const cgltf_image& image,
    const std::filesystem::path& gltfPath) {
    if (image.buffer_view != nullptr && image.buffer_view->buffer != nullptr && image.buffer_view->buffer->data != nullptr) {
        const auto* bytes = static_cast<const byte*>(image.buffer_view->buffer->data) + image.buffer_view->offset;
        vector<byte> encodedBytes(image.buffer_view->size);
        if (!encodedBytes.empty()) {
            std::memcpy(encodedBytes.data(), bytes, encodedBytes.size());
        }
        return encodedBytes;
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
        return DecodeBase64(uri.substr(comma + 1));
    }
    const std::filesystem::path imagePath = gltfPath.parent_path() / std::filesystem::path{string{uri}};
    std::ifstream stream{imagePath, std::ios::binary | std::ios::ate};
    if (!stream.is_open()) {
        return std::nullopt;
    }
    const std::streampos end = stream.tellg();
    if (end <= 0) {
        return vector<byte>{};
    }
    vector<byte> encodedBytes(static_cast<size_t>(end));
    stream.seekg(0, std::ios::beg);
    stream.read(reinterpret_cast<char*>(encodedBytes.data()), static_cast<std::streamsize>(encodedBytes.size()));
    if (!stream) {
        return std::nullopt;
    }
    return encodedBytes;
}

int ImageIndexOf(const cgltf_data* data, const cgltf_texture_view& view) noexcept {
    if (data == nullptr || data->images == nullptr || view.texture == nullptr || view.texture->image == nullptr) {
        return -1;
    }
    const ptrdiff_t index = view.texture->image - data->images;
    if (index < 0 || index >= static_cast<ptrdiff_t>(data->images_count)) {
        return -1;
    }
    return static_cast<int>(index);
}

// ── 材质转换 (cgltf -> 中性描述) ──
// 贴图索引先记 image 索引; 加载纹理后再重映射到 GltfAsset 纹理表下标 (见 LoadGltfAsset)。

GltfAlphaMode ToAlphaMode(cgltf_alpha_mode mode) noexcept {
    switch (mode) {
        case cgltf_alpha_mode_mask: return GltfAlphaMode::Mask;
        case cgltf_alpha_mode_blend: return GltfAlphaMode::Blend;
        case cgltf_alpha_mode_opaque:
        default: return GltfAlphaMode::Opaque;
    }
}

GltfMaterialDesc ConvertMaterial(const cgltf_data* data, const cgltf_material& src) {
    GltfMaterialDesc out;
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
        out.MetallicFactor = pbr.metallic_factor;
        out.RoughnessFactor = pbr.roughness_factor;
        out.BaseColorTexture = ImageIndexOf(data, pbr.base_color_texture);
        out.MetallicRoughnessTexture = ImageIndexOf(data, pbr.metallic_roughness_texture);
    } else if (src.has_pbr_specular_glossiness) {
        // 退化处理: 用 diffuse 当 base color, glossiness 反推 roughness, metallic 归 0。
        const cgltf_pbr_specular_glossiness& pbr = src.pbr_specular_glossiness;
        out.BaseColorFactor = Eigen::Vector4f{
            pbr.diffuse_factor[0],
            pbr.diffuse_factor[1],
            pbr.diffuse_factor[2],
            pbr.diffuse_factor[3]};
        out.RoughnessFactor = 1.0f - pbr.glossiness_factor;
        out.MetallicFactor = 0.0f;
        out.BaseColorTexture = ImageIndexOf(data, pbr.diffuse_texture);
    }

    // KHR_materials_specular
    if (src.has_specular) {
        out.Specular = src.specular.specular_factor;
        out.SpecularTint = std::max({
            src.specular.specular_color_factor[0],
            src.specular.specular_color_factor[1],
            src.specular.specular_color_factor[2]});
    }
    // KHR_materials_clearcoat
    if (src.has_clearcoat) {
        out.Clearcoat = src.clearcoat.clearcoat_factor;
        out.ClearcoatGloss = 1.0f - src.clearcoat.clearcoat_roughness_factor;
    }
    // KHR_materials_sheen
    if (src.has_sheen) {
        out.Sheen = std::max({
            src.sheen.sheen_color_factor[0],
            src.sheen.sheen_color_factor[1],
            src.sheen.sheen_color_factor[2]});
        out.SheenTint = out.Sheen > 0.0f ? 1.0f : 0.0f;
    }

    out.EmissiveFactor = Eigen::Vector3f{src.emissive_factor[0], src.emissive_factor[1], src.emissive_factor[2]};
    // KHR_materials_emissive_strength
    if (src.has_emissive_strength) {
        out.EmissiveStrength = src.emissive_strength.emissive_strength;
    }

    out.NormalTexture = ImageIndexOf(data, src.normal_texture);
    out.NormalScale = src.normal_texture.scale != 0.0f ? src.normal_texture.scale : 1.0f;
    out.OcclusionTexture = ImageIndexOf(data, src.occlusion_texture);
    out.OcclusionStrength = src.occlusion_texture.scale != 0.0f ? src.occlusion_texture.scale : 1.0f;
    out.EmissiveTexture = ImageIndexOf(data, src.emissive_texture);

    out.AlphaCutoff = src.alpha_cutoff;
    out.AlphaMode = ToAlphaMode(src.alpha_mode);
    out.DoubleSided = src.double_sided != 0;
    return out;
}

// ── primitive 转换 ──

std::optional<GltfPrimitiveCpu> ConvertPrimitive(
    const cgltf_data* data,
    const cgltf_primitive& primitive,
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
        // RH->LH: 位置/法线只翻转 Z; 切线额外翻转 w (TBN 手性)。
        vertex.Position = ReflectZToLH(vertex.Position);
        vertex.Normal = ReflectZToLH(vertex.Normal);
        vertex.Tangent = ReflectTangentToLH(vertex.Tangent);
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
    FlipTriangleWindingToLH(out.Indices);
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

MeshResource MakePackedMeshResource(
    const vector<GltfPrimitiveCpu>& primitives,
    vector<PackedPrimitiveSection>& sections) {
    MeshResource mesh;
    mesh.Name = "gltf_geometry";
    vector<GltfVertex> vertices;
    vector<uint32_t> indices;
    sections.clear();
    sections.reserve(primitives.size());
    for (const GltfPrimitiveCpu& primitive : primitives) {
        const uint32_t baseVertex = static_cast<uint32_t>(vertices.size());
        const uint32_t firstIndex = static_cast<uint32_t>(indices.size());
        vertices.insert(vertices.end(), primitive.Vertices.begin(), primitive.Vertices.end());
        indices.reserve(indices.size() + primitive.Indices.size());
        for (const uint32_t index : primitive.Indices) {
            indices.push_back(index);
        }
        sections.push_back(PackedPrimitiveSection{
            .FirstIndex = firstIndex,
            .IndexCount = static_cast<uint32_t>(primitive.Indices.size()),
            .MinVertexIndex = 0,
            .MaxVertexIndex = primitive.Vertices.empty()
                                  ? 0
                                  : static_cast<uint32_t>(primitive.Vertices.size()) - 1u,
            .VertexOffset = static_cast<int32_t>(baseVertex)});
    }

    vector<byte> vertexData = ToBytes(std::span<const GltfVertex>{vertices});
    vector<byte> indexData = ToBytes(std::span<const uint32_t>{indices});

    MeshPrimitive meshPrimitive{};
    meshPrimitive.VertexCount = static_cast<uint32_t>(vertices.size());
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
    meshPrimitive.IndexBuffer.IndexCount = static_cast<uint32_t>(indices.size());
    meshPrimitive.IndexBuffer.Offset = 0;
    meshPrimitive.IndexBuffer.Stride = sizeof(uint32_t);

    mesh.Bins.emplace_back(std::span<const byte>{vertexData.data(), vertexData.size()});
    mesh.Bins.emplace_back(std::span<const byte>{indexData.data(), indexData.size()});
    mesh.Primitives.emplace_back(std::move(meshPrimitive));
    return mesh;
}

// ── 节点变换 ──

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
    } else {
        out.Translation = Eigen::Vector3f{translation[0], translation[1], translation[2]};
        out.Rotation = Eigen::Quaternionf{rotation[3], rotation[0], rotation[1], rotation[2]};
        out.Rotation.normalize();
        out.Scale = Eigen::Vector3f{scale[0], scale[1], scale[2]};
    }

    // RH->LH: 对节点局部变换做 diag(1,1,-1) 反射共轭。平移翻 Z, 旋转四元数共轭, 缩放不变。
    out.Translation = ReflectZToLH(out.Translation);
    out.Rotation = ReflectRotationToLH(out.Rotation);
    out.Rotation.normalize();
}

Eigen::Matrix4f ComputeNodeWorldMatrix(const vector<GltfNodeDesc>& nodes, int nodeIndex) {
    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(nodes.size())) {
        return Eigen::Matrix4f::Identity();
    }
    const GltfNodeDesc& node = nodes[static_cast<size_t>(nodeIndex)];
    const Eigen::Matrix4f local = ComposeTransform<float>(node.Translation, node.Rotation, node.Scale);
    if (node.Parent < 0) {
        return local;
    }
    return ComputeNodeWorldMatrix(nodes, node.Parent) * local;
}

// ── 纹理加载 (按 image 索引 + sRGB 缓存去重) ──

struct TextureLoadKey {
    int ImageIndex;
    bool Srgb;
    bool operator==(const TextureLoadKey&) const noexcept = default;
};

uint64_t MakeTextureCacheKey(int imageIndex, bool srgb) noexcept {
    return (static_cast<uint64_t>(static_cast<uint32_t>(imageIndex)) << 1u) | (srgb ? 1ull : 0ull);
}

string GltfImageName(const cgltf_data* data, int imageIndex) {
    if (data != nullptr && imageIndex >= 0 && imageIndex < static_cast<int>(data->images_count)) {
        const cgltf_image& image = data->images[static_cast<size_t>(imageIndex)];
        if (image.name != nullptr && image.name[0] != '\0') {
            return image.name;
        }
        if (image.uri != nullptr && image.uri[0] != '\0') {
            return image.uri;
        }
    }
    return fmt::format("Image {}", imageIndex);
}

// 加载 (imageIndex, sRGB) 组合的纹理, 返回其在 outTextures 中的下标 (缓存命中则复用)。
// imageIndex < 0 返回 -1 (无贴图)。
int AcquireTexture(
    AssetManager& assetManager,
    FrameUploadScheduler& frameUploads,
    const std::filesystem::path& path,
    const cgltf_data* data,
    int imageIndex,
    bool srgb,
    unordered_map<uint64_t, int>& cache,
    vector<GltfTextureRef>& outTextures) {
    if (imageIndex < 0 || data == nullptr || imageIndex >= static_cast<int>(data->images_count)) {
        return -1;
    }
    const uint64_t key = MakeTextureCacheKey(imageIndex, srgb);
    auto it = cache.find(key);
    if (it != cache.end()) {
        return it->second;
    }

    std::optional<vector<byte>> encoded = ExtractImageEncodedBytes(data->images[static_cast<size_t>(imageIndex)], path);
    const string name = fmt::format("{} ({})", GltfImageName(data, imageIndex), srgb ? "sRGB" : "linear");
    TextureAssetLoadOptions options{
        .Srgb = srgb,
        // 解码失败回退: sRGB 用白, linear 用中性灰 (法线贴图缺失时应为 (0.5,0.5,1) 但白也可接受为占位)。
        .FallbackImage = MakeSolidImage(255, 255, 255, 255)};

    StreamingAssetRef<TextureAsset> texture = LoadTextureAssetFromMemory(
        assetManager,
        frameUploads,
        MakeDerivedAssetId(path, srgb ? "tex-srgb" : "tex-linear", static_cast<uint32_t>(imageIndex)),
        name,
        encoded.has_value() ? std::move(encoded.value()) : vector<byte>{},
        options);

    const int slot = static_cast<int>(outTextures.size());
    outTextures.push_back(GltfTextureRef{texture, srgb});
    cache.emplace(key, slot);
    return slot;
}

}  // namespace

GltfAsset::GltfAsset(
    std::filesystem::path path,
    vector<GltfNodeDesc> nodes,
    vector<int> rootNodes,
    vector<GltfPrimitiveDesc> primitives,
    vector<GltfMaterialDesc> materials,
    vector<GltfTextureRef> textures,
    Eigen::Vector3f boundsMin,
    Eigen::Vector3f boundsMax,
    bool hasBounds) noexcept
    : _path(std::move(path)),
      _nodes(std::move(nodes)),
      _rootNodes(std::move(rootNodes)),
      _primitives(std::move(primitives)),
      _materials(std::move(materials)),
      _textures(std::move(textures)),
      _boundsMin(boundsMin),
      _boundsMax(boundsMax),
      _hasBounds(hasBounds) {
}

GltfAsset::~GltfAsset() noexcept = default;

void GltfAsset::OnUnload(IRenderResourceRecycler& recycler) {
    (void)recycler;
    _nodes.clear();
    _rootNodes.clear();
    _primitives.clear();
    _materials.clear();
    _textures.clear();
    _boundsMin = Eigen::Vector3f::Zero();
    _boundsMax = Eigen::Vector3f::Zero();
    _hasBounds = false;
}

AssetTypeId GltfAsset::GetTypeId() const noexcept {
    return runtime_type_id_v<GltfAsset>;
}

Actor* GltfAsset::SpawnScene(World& world, IStandardMaterialFactory& factory) const {
    Actor* rootActor = world.SpawnActor<Actor>();
    SceneComponent* root = rootActor->AddComponent<SceneComponent>();
    rootActor->SetRootComponent(root);

    // 每个有 mesh 的节点 spawn 一个 Actor, 挂 StaticMeshComponent, 用世界矩阵定位。
    // 材质: 对该节点每个 primitive 调 factory 得 MaterialAsset, 填入 section 材质槽。
    for (const GltfPrimitiveDesc& primitive : _primitives) {
        if (primitive.NodeIndex < 0 || primitive.NodeIndex >= static_cast<int>(_nodes.size())) {
            continue;
        }
        Actor* meshActor = world.SpawnActor<Actor>();
        StaticMeshComponent* component = meshActor->AddComponent<StaticMeshComponent>();
        meshActor->SetRootComponent(component);
        component->AttachTo(root);
        component->SetStaticMesh(primitive.Mesh);

        // 世界矩阵分解为 T/R/S 写入相对变换 (root 在世界原点, 相对=世界)。
        const Eigen::Matrix4f& worldMatrix = _nodes[static_cast<size_t>(primitive.NodeIndex)].WorldMatrix;
        Eigen::Vector3f translation;
        Eigen::Quaternionf rotation;
        Eigen::Vector3f scale;
        DecomposeTransform<float>(worldMatrix, translation, rotation, scale);
        component->SetRelativeLocation(translation);
        component->SetRelativeRotation(rotation);
        component->SetRelativeScale(scale);

        // 材质翻译 (shader-无关: 交给当前管线的标准材质工厂)。单 section 覆盖整个 primitive。
        const uint32_t matIdx = primitive.MaterialIndex < _materials.size() ? primitive.MaterialIndex : 0;
        const GltfMaterialDesc& desc = matIdx < _materials.size() ? _materials[matIdx] : GltfMaterialDesc{};
        StreamingAssetRef<MaterialAsset> material =
            factory.CreateMaterial(desc, std::span<const GltfTextureRef>{_textures});
        if (!material.IsValid()) {
            material = factory.GetDefaultMaterial();  // 翻译失败: 回退到管线默认材质。
        }
        component->SetMaterial(0, std::move(material));
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
            (void)options;
            cgltf_options cgltfOptions{};
            cgltf_data* data = nullptr;
            cgltf_result parseResult = cgltf_parse_file(&cgltfOptions, path.string().c_str(), &data);
            if (parseResult != cgltf_result_success || data == nullptr) {
                co_return AssetLoadResult::Failure(fmt::format("cgltf_parse_file failed: {}", static_cast<int>(parseResult)));
            }
            auto guard = MakeScopeGuard([&]() { cgltf_free(data); });

            parseResult = cgltf_load_buffers(&cgltfOptions, data, path.string().c_str());
            if (parseResult != cgltf_result_success) {
                co_return AssetLoadResult::Failure(fmt::format("cgltf_load_buffers failed: {}", static_cast<int>(parseResult)));
            }
            parseResult = cgltf_validate(data);
            if (parseResult != cgltf_result_success) {
                co_return AssetLoadResult::Failure(fmt::format("cgltf_validate failed: {}", static_cast<int>(parseResult)));
            }

            // ── 材质 (中性描述; 贴图索引此时仍是 image 索引) ──
            vector<GltfMaterialDesc> materials;
            materials.reserve(std::max<cgltf_size>(1, data->materials_count));
            for (cgltf_size i = 0; i < data->materials_count; ++i) {
                GltfMaterialDesc material = ConvertMaterial(data, data->materials[i]);
                if (material.Name == "Default") {
                    material.Name = fmt::format("Material {}", i);
                }
                materials.push_back(std::move(material));
            }
            if (materials.empty()) {
                materials.push_back(GltfMaterialDesc{});
            }

            // ── 纹理加载 + 把材质里的 image 索引重映射到纹理表下标 ──
            vector<GltfTextureRef> textures;
            unordered_map<uint64_t, int> textureCache;
            for (GltfMaterialDesc& mat : materials) {
                mat.BaseColorTexture = AcquireTexture(assetManager, frameUploads, path, data, mat.BaseColorTexture, true, textureCache, textures);
                mat.MetallicRoughnessTexture = AcquireTexture(assetManager, frameUploads, path, data, mat.MetallicRoughnessTexture, false, textureCache, textures);
                mat.NormalTexture = AcquireTexture(assetManager, frameUploads, path, data, mat.NormalTexture, false, textureCache, textures);
                mat.OcclusionTexture = AcquireTexture(assetManager, frameUploads, path, data, mat.OcclusionTexture, false, textureCache, textures);
                mat.EmissiveTexture = AcquireTexture(assetManager, frameUploads, path, data, mat.EmissiveTexture, true, textureCache, textures);
            }
            // 等所有纹理 GPU 上传完成: SpawnScene 时材质工厂需要 GetSrv() 非空 (同步捕获 TextureView*)。
            for (const GltfTextureRef& tex : textures) {
                co_await assetManager.Wait(tex.Texture.AsAny());
            }

            // ── 节点树 (RH->LH) + 世界矩阵 ──
            vector<GltfNodeDesc> nodes;
            vector<int> rootNodes;
            nodes.resize(data->nodes_count);
            for (cgltf_size i = 0; i < data->nodes_count; ++i) {
                const cgltf_node& node = data->nodes[i];
                GltfNodeDesc& out = nodes[i];
                out.Name = node.name != nullptr ? node.name : fmt::format("Node {}", i);
                out.Parent = node.parent != nullptr ? static_cast<int>(cgltf_node_index(data, node.parent)) : -1;
                out.HasMesh = node.mesh != nullptr;
                DecomposeNodeTransform(node, out);
                if (out.Parent < 0) {
                    rootNodes.push_back(static_cast<int>(i));
                }
                for (cgltf_size c = 0; c < node.children_count; ++c) {
                    out.Children.push_back(static_cast<int>(cgltf_node_index(data, node.children[c])));
                }
            }
            for (size_t i = 0; i < nodes.size(); ++i) {
                nodes[i].WorldMatrix = ComputeNodeWorldMatrix(nodes, static_cast<int>(i));
            }

            // ── primitive identity 去重 + 单 VB/IB packing ──
            vector<GltfPrimitiveCpu> uniquePrimitives;
            unordered_map<const cgltf_primitive*, uint32_t> primitiveLookup;
            for (cgltf_size meshIndex = 0; meshIndex < data->meshes_count; ++meshIndex) {
                const cgltf_mesh& sourceMesh = data->meshes[meshIndex];
                const std::string_view meshName = sourceMesh.name != nullptr ? sourceMesh.name : "mesh";
                for (cgltf_size p = 0; p < sourceMesh.primitives_count; ++p) {
                    const cgltf_primitive* identity = &sourceMesh.primitives[p];
                    const string primitiveName = fmt::format("{} / prim {}", meshName, p);
                    auto primitive = ConvertPrimitive(data, *identity, primitiveName);
                    if (!primitive.has_value()) {
                        continue;
                    }
                    const uint32_t uniqueIndex = static_cast<uint32_t>(uniquePrimitives.size());
                    primitiveLookup.emplace(identity, uniqueIndex);
                    uniquePrimitives.push_back(std::move(primitive.value()));
                }
            }
            if (uniquePrimitives.empty()) {
                co_return AssetLoadResult::Failure("glTF has no supported triangle primitives");
            }

            vector<PackedPrimitiveSection> packedSections;
            MeshResource packedResource = MakePackedMeshResource(uniquePrimitives, packedSections);
            FrameUploadScope geometryFrame = co_await frameUploads.BeginUpload();
            auto gpuMeshOpt = geometryFrame.GetUploader().UploadMeshResource(
                geometryFrame.GetCommandBuffer(), packedResource);
            if (!gpuMeshOpt.has_value()) {
                co_return AssetLoadResult::Failure("gltf packed geometry upload failed");
            }
            co_await geometryFrame.WaitGpu();
            auto sharedGpuMesh = make_shared<GpuMesh>(std::move(gpuMeshOpt.value()));

            vector<StreamingAssetRef<StaticMesh>> meshAssets;
            meshAssets.reserve(uniquePrimitives.size());
            for (uint32_t i = 0; i < uniquePrimitives.size(); ++i) {
                const GltfPrimitiveCpu& primitive = uniquePrimitives[i];
                const PackedPrimitiveSection& section = packedSections[i];
                auto staticMesh = make_unique<StaticMesh>(MeshResource{}, sharedGpuMesh);
                staticMesh->SetSections(vector<StaticMeshSection>{StaticMeshSection{
                    /*primitiveIndex*/ 0,
                    section.FirstIndex,
                    section.IndexCount,
                    section.MinVertexIndex,
                    section.MaxVertexIndex,
                    section.VertexOffset}});
                staticMesh->SetBounds(primitive.BoundsMin, primitive.BoundsMax);
                meshAssets.push_back(assetManager.AddReady<StaticMesh>(
                    MakeDerivedAssetId(path, "mesh", i), std::move(staticMesh)));
            }

            vector<GltfPrimitiveDesc> primitives;
            Eigen::Vector3f sceneMin = Eigen::Vector3f::Zero();
            Eigen::Vector3f sceneMax = Eigen::Vector3f::Zero();
            bool sceneHasBounds = false;
            for (cgltf_size n = 0; n < data->nodes_count; ++n) {
                const cgltf_node& node = data->nodes[n];
                if (node.mesh == nullptr) {
                    continue;
                }
                for (cgltf_size p = 0; p < node.mesh->primitives_count; ++p) {
                    const cgltf_primitive* identity = &node.mesh->primitives[p];
                    auto lookup = primitiveLookup.find(identity);
                    if (lookup == primitiveLookup.end()) {
                        continue;
                    }
                    const uint32_t uniqueIndex = lookup->second;
                    const GltfPrimitiveCpu& primitive = uniquePrimitives[uniqueIndex];
                    GltfPrimitiveDesc desc{};
                    desc.Name = fmt::format("{} / prim {}", nodes[n].Name, p);
                    desc.NodeIndex = static_cast<int>(n);
                    desc.MaterialIndex = primitive.MaterialIndex;
                    desc.Mesh = meshAssets[uniqueIndex];
                    desc.BoundsMin = primitive.BoundsMin;
                    desc.BoundsMax = primitive.BoundsMax;
                    desc.HasBounds = primitive.HasBounds;
                    primitives.push_back(std::move(desc));

                    if (primitive.HasBounds) {
                        UpdateBoundsWithTransformedBox(
                            sceneMin,
                            sceneMax,
                            sceneHasBounds,
                            primitive.BoundsMin,
                            primitive.BoundsMax,
                            nodes[n].WorldMatrix);
                    }
                }
            }

            if (primitives.empty()) {
                co_return AssetLoadResult::Failure("glTF has no supported triangle primitives");
            }

            auto asset = make_unique<GltfAsset>(
                path,
                std::move(nodes),
                std::move(rootNodes),
                std::move(primitives),
                std::move(materials),
                std::move(textures),
                sceneMin,
                sceneMax,
                sceneHasBounds);

            RADRAY_INFO_LOG(
                "glTF loaded '{}': primitives={}, materials={}, textures={}, nodes={}",
                path.string(),
                asset->GetPrimitives().size(),
                asset->GetMaterials().size(),
                asset->GetTextures().size(),
                asset->GetNodes().size());

            co_return AssetLoadResult::Success(std::move(asset));
        }(assetManager, frameUploads, path, options),
        .DebugName = path.string()});
}

}  // namespace radray
