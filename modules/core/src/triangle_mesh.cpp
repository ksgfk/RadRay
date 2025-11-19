#include <radray/triangle_mesh.h>

#include <new>
#include <numbers>
#include <limits>
#include <type_traits>
#include <utility>
#include <cstring>

#include <radray/errors.h>
#include <radray/vertex_data.h>

namespace radray {

bool TriangleMesh::IsValid() const noexcept {
    return Indices.size() > 0 &&
           Indices.size() % 3 == 0 &&
           Positions.size() > 0 &&
           Positions.size() <= std::numeric_limits<uint32_t>::max() &&
           (Normals.size() == 0 || Normals.size() == Positions.size()) &&
           (UV0.size() == 0 || UV0.size() == Positions.size()) &&
           (Tangents.size() == 0 || Tangents.size() == Positions.size()) &&
           (Color0.size() == 0 || Color0.size() == Positions.size());
}

void TriangleMesh::ToSimpleMeshResource(MeshResource* outResource) const noexcept {
    if (outResource == nullptr) {
        return;
    }
    if (!IsValid()) {
        return;
    }
    MeshResource resource;
    resource.Name = outResource->Name;
    MeshPrimitive primitive;
    primitive.VertexCount = static_cast<uint32_t>(Positions.size());
    auto pushBuffer = [&](std::span<const byte> data) -> uint32_t {
        resource.Bins.emplace_back(data);
        return static_cast<uint32_t>(resource.Bins.size() - 1);
    };
    struct AttributeDesc {
        std::string_view Semantic;
        uint32_t SemanticIndex;
        size_t ElementSize;
        size_t Offset;
        const byte* BasePtr;
    };

    vector<AttributeDesc> attributes;
    size_t vertexStride = 0;

    auto queueAttribute = [&](auto const& container, std::string_view semantic, uint32_t semanticIndex = 0U) {
        if (container.empty()) {
            return;
        }
        using Container = std::decay_t<decltype(container)>;
        using ValueType = typename Container::value_type;

        AttributeDesc desc{};
        desc.Semantic = semantic;
        desc.SemanticIndex = semanticIndex;
        desc.ElementSize = sizeof(ValueType);
        desc.Offset = vertexStride;
        desc.BasePtr = reinterpret_cast<const byte*>(container.data());
        vertexStride += desc.ElementSize;
        attributes.emplace_back(desc);
    };

    queueAttribute(Positions, VertexSemantics::POSITION);
    queueAttribute(Normals, VertexSemantics::NORMAL);
    queueAttribute(UV0, VertexSemantics::TEXCOORD, 0U);
    queueAttribute(Tangents, VertexSemantics::TANGENT);
    queueAttribute(Color0, VertexSemantics::COLOR);

    uint32_t vertexBufferIndex = UINT32_MAX;
    if (!attributes.empty()) {
        vector<byte> interleaved(vertexStride * primitive.VertexCount, byte{0});
        for (uint32_t v = 0; v < primitive.VertexCount; ++v) {
            byte* dst = interleaved.data() + static_cast<size_t>(v) * vertexStride;
            for (const auto& attr : attributes) {
                const byte* src = attr.BasePtr + static_cast<size_t>(v) * attr.ElementSize;
                std::memcpy(dst + attr.Offset, src, attr.ElementSize);
            }
        }

        std::span<const byte> vertexBytes{interleaved.data(), interleaved.size()};
        vertexBufferIndex = pushBuffer(vertexBytes);

        const uint32_t stride = static_cast<uint32_t>(vertexStride);
        for (const auto& attr : attributes) {
            VertexBufferEntry entry{};
            entry.Semantic = string{attr.Semantic};
            entry.SemanticIndex = attr.SemanticIndex;
            entry.BufferIndex = vertexBufferIndex;
            entry.Offset = static_cast<uint32_t>(attr.Offset);
            entry.Stride = stride;
            primitive.VertexBuffers.emplace_back(entry);
        }
    }

    if (!Indices.empty()) {
        std::span<const byte> indexBytes{
            reinterpret_cast<const byte*>(Indices.data()),
            Indices.size() * sizeof(uint32_t)};
        primitive.IndexBuffer.BufferIndex = pushBuffer(indexBytes);
        primitive.IndexBuffer.IndexCount = static_cast<uint32_t>(Indices.size());
        primitive.IndexBuffer.Offset = 0;
        primitive.IndexBuffer.Stride = sizeof(uint32_t);
    }

    resource.Primitives.clear();
    resource.Primitives.emplace_back(std::move(primitive));

    *outResource = std::move(resource);
}

void TriangleMesh::InitAsCube(float halfExtend) noexcept {
    Positions = vector<Eigen::Vector3f>{
        {-1.0f, -1.0f, -1.0f},
        {-1.0f, -1.0f, 1.0f},
        {1.0f, -1.0f, 1.0f},
        {1.0f, -1.0f, -1.0f},
        {-1.0f, 1.0f, -1.0f},
        {-1.0f, 1.0f, 1.0f},
        {1.0f, 1.0f, 1.0f},
        {1.0f, 1.0f, -1.0f},
        {-1.0f, -1.0f, -1.0f},
        {-1.0f, 1.0f, -1.0f},
        {1.0f, 1.0f, -1.0f},
        {1.0f, -1.0f, -1.0f},
        {-1.0f, -1.0f, 1.0f},
        {-1.0f, 1.0f, 1.0f},
        {1.0f, 1.0f, 1.0f},
        {1.0f, -1.0f, 1.0f},
        {-1.0f, -1.0f, -1.0f},
        {-1.0f, -1.0f, 1.0f},
        {-1.0f, 1.0f, 1.0f},
        {-1.0f, 1.0f, -1.0f},
        {1.0f, -1.0f, -1.0f},
        {1.0f, -1.0f, 1.0f},
        {1.0f, 1.0f, 1.0f},
        {1.0f, 1.0f, -1.0f}};
    Normals = vector<Eigen::Vector3f>{
        {0.0f, -1.0f, 0.0f},
        {0.0f, -1.0f, 0.0f},
        {0.0f, -1.0f, 0.0f},
        {0.0f, -1.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, -1.0f},
        {0.0f, 0.0f, -1.0f},
        {0.0f, 0.0f, -1.0f},
        {0.0f, 0.0f, -1.0f},
        {0.0f, 0.0f, 1.0f},
        {0.0f, 0.0f, 1.0f},
        {0.0f, 0.0f, 1.0f},
        {0.0f, 0.0f, 1.0f},
        {-1.0f, 0.0f, 0.0f},
        {-1.0f, 0.0f, 0.0f},
        {-1.0f, 0.0f, 0.0f},
        {-1.0f, 0.0f, 0.0f},
        {1.0f, 0.0f, 0.0f},
        {1.0f, 0.0f, 0.0f},
        {1.0f, 0.0f, 0.0f},
        {1.0f, 0.0f, 0.0f}};
    UV0 = vector<Eigen::Vector2f>{
        {0.0f, 0.0f},
        {0.0f, 1.0f},
        {1.0f, 1.0f},
        {1.0f, 0.0f},
        {0.0f, 1.0f},
        {0.0f, 0.0f},
        {1.0f, 0.0f},
        {1.0f, 1.0f},
        {1.0f, 0.0f},
        {1.0f, 1.0f},
        {0.0f, 1.0f},
        {0.0f, 0.0f},
        {0.0f, 0.0f},
        {0.0f, 1.0f},
        {1.0f, 1.0f},
        {1.0f, 0.0f},
        {0.0f, 0.0f},
        {1.0f, 0.0f},
        {1.0f, 1.0f},
        {0.0f, 1.0f},
        {1.0f, 0.0f},
        {0.0f, 0.0f},
        {0.0f, 1.0f},
        {1.0f, 1.0f}};
    Tangents = vector<Eigen::Vector4f>{
        {1.0f, 0.0f, 0.0f, 1.0f},
        {1.0f, 0.0f, 0.0f, 1.0f},
        {1.0f, 0.0f, 0.0f, 1.0f},
        {1.0f, 0.0f, 0.0f, 1.0f},
        {1.0f, 0.0f, 0.0f, 1.0f},
        {1.0f, 0.0f, 0.0f, 1.0f},
        {1.0f, 0.0f, 0.0f, 1.0f},
        {1.0f, 0.0f, 0.0f, 1.0f},
        {-1.0f, 0.0f, 0.0f, 1.0f},
        {-1.0f, 0.0f, 0.0f, 1.0f},
        {-1.0f, 0.0f, 0.0f, 1.0f},
        {-1.0f, 0.0f, 0.0f, 1.0f},
        {1.0f, 0.0f, 0.0f, 1.0f},
        {1.0f, 0.0f, 0.0f, 1.0f},
        {1.0f, 0.0f, 0.0f, 1.0f},
        {1.0f, 0.0f, 0.0f, 1.0f},
        {0.0f, 0.0f, 1.0f, 1.0f},
        {0.0f, 0.0f, 1.0f, 1.0f},
        {0.0f, 0.0f, 1.0f, 1.0f},
        {0.0f, 0.0f, 1.0f, 1.0f},
        {0.0f, 0.0f, -1.0f, 1.0f},
        {0.0f, 0.0f, -1.0f, 1.0f},
        {0.0f, 0.0f, -1.0f, 1.0f},
        {0.0f, 0.0f, -1.0f, 1.0f}};
    Indices = vector<uint32_t>{0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 8, 9, 10, 8, 10, 11, 12, 15, 14, 12, 14, 13, 16, 17, 18, 16, 18, 19, 20, 23, 22, 20, 22, 21};
    if (halfExtend != 1) {
        for (auto&& i : Positions) {
            i *= halfExtend;
        }
    }
}

void TriangleMesh::InitAsUVSphere(float radius, uint32_t numberSlices) noexcept {
    uint32_t numberParallels = numberSlices / 2;
    uint32_t numberVertices = (numberParallels + 1) * (numberSlices + 1);
    uint32_t numberIndices = numberParallels * numberSlices * 6;
    float angleStep = (2.0f * std::numbers::pi_v<float>) / numberSlices;
    Positions.resize(numberVertices);
    Normals.resize(numberVertices);
    UV0.resize(numberVertices);
    Tangents.resize(numberVertices);
    for (uint32_t i = 0; i < numberParallels + 1; i++) {
        for (uint32_t j = 0; j < (numberSlices + 1); j++) {
            uint32_t vertexIndex = (i * (numberSlices + 1) + j);
            uint32_t normalIndex = (i * (numberSlices + 1) + j);
            uint32_t texCoordsIndex = (i * (numberSlices + 1) + j);
            uint32_t tangentIndex = (i * (numberSlices + 1) + j);
            float px = radius * std::sin(angleStep * (float)i) * std::sin(angleStep * (float)j);
            float py = radius * std::cos(angleStep * (float)i);
            float pz = radius * std::sin(angleStep * (float)i) * std::cos(angleStep * (float)j);
            Positions[vertexIndex] = {px, py, pz};
            float nx = Positions[vertexIndex].x() / radius;
            float ny = Positions[vertexIndex].y() / radius;
            float nz = Positions[vertexIndex].z() / radius;
            Normals[normalIndex] = Eigen::Vector3f{nx, ny, nz}.normalized();
            float tx = (float)j / (float)numberSlices;
            float ty = 1.0f - (float)i / (float)numberParallels;
            UV0[texCoordsIndex] = {tx, ty};
            Eigen::AngleAxisf mat(Radian(360.0f * UV0[texCoordsIndex].x()), Eigen::Vector3f{0, 1.0f, 0});
            Eigen::Vector3f tan = mat.matrix() * Eigen::Vector3f{1, 0, 0};
            Tangents[tangentIndex] = Eigen::Vector4f{tan.x(), tan.y(), tan.z(), 1.0f};
        }
    }
    uint32_t indexIndices = 0;
    Indices.resize(numberIndices);
    for (uint32_t i = 0; i < numberParallels; i++) {
        for (uint32_t j = 0; j < (numberSlices); j++) {
            Indices[indexIndices++] = i * (numberSlices + 1) + j;
            Indices[indexIndices++] = (i + 1) * (numberSlices + 1) + j;
            Indices[indexIndices++] = (i + 1) * (numberSlices + 1) + (j + 1);
            Indices[indexIndices++] = i * (numberSlices + 1) + j;
            Indices[indexIndices++] = (i + 1) * (numberSlices + 1) + (j + 1);
            Indices[indexIndices++] = i * (numberSlices + 1) + (j + 1);
        }
    }
}

void TriangleMesh::InitAsRectXY(float width, float height) noexcept {
    Positions = {
        {-width / 2, -height / 2, 0},
        {width / 2, -height / 2, 0},
        {width / 2, height / 2, 0},
        {-width / 2, height / 2, 0}};
    Normals = {
        {0, 0, 1},
        {0, 0, 1},
        {0, 0, 1},
        {0, 0, 1}};
    UV0 = {
        {0, 0},
        {1, 0},
        {1, 1},
        {0, 1}};
    Tangents = {
        {1, 0, 0, 1},
        {1, 0, 0, 1},
        {1, 0, 0, 1},
        {1, 0, 0, 1}};
    Indices = {
        0, 1, 2,
        0, 2, 3};
}

void TriangleMesh::CalculateTangent() noexcept {
    if (Normals.size() == 0 || UV0.size() == 0) {
        Tangents.resize(Positions.size(), Eigen::Vector4f{1.0f, 0.0f, 0.0f, 1.0f});
        return;
    }
    if (Indices.size() % 3 != 0) {
        return;
    }
    // https://terathon.com/blog/tangent-space.html
    // Foundations of Game Engine Development, Volume 2: Rendering, 2019.
    vector<Eigen::Vector3f> tan1, tan2;
    tan1.resize(Positions.size(), Eigen::Vector3f{0.0f, 0.0f, 0.0f});
    tan2.resize(Positions.size(), Eigen::Vector3f{0.0f, 0.0f, 0.0f});
    vector<Eigen::Vector4f> tangent;
    tangent.resize(Positions.size(), Eigen::Vector4f{0.0f, 0.0f, 0.0f, 0.0f});
    for (size_t a = 0; a < Indices.size(); a += 3) {
        uint32_t i1 = Indices[a];
        uint32_t i2 = Indices[a + 1];
        uint32_t i3 = Indices[a + 2];

        Eigen::Vector3f v1 = Positions[i1];
        Eigen::Vector3f v2 = Positions[i2];
        Eigen::Vector3f v3 = Positions[i3];

        Eigen::Vector2f w1 = UV0[i1];
        Eigen::Vector2f w2 = UV0[i2];
        Eigen::Vector2f w3 = UV0[i3];

        float x1 = v2.x() - v1.x();
        float x2 = v3.x() - v1.x();
        float y1 = v2.y() - v1.y();
        float y2 = v3.y() - v1.y();
        float z1 = v2.z() - v1.z();
        float z2 = v3.z() - v1.z();

        float s1 = w2.x() - w1.x();
        float s2 = w3.x() - w1.x();
        float t1 = w2.y() - w1.y();
        float t2 = w3.y() - w1.y();

        float denom = s1 * t2 - s2 * t1;
        if (std::abs(denom) <= std::numeric_limits<float>::epsilon()) {
            continue;
        }
        float r = 1.0f / denom;
        Eigen::Vector3f sdir{(t2 * x1 - t1 * x2) * r, (t2 * y1 - t1 * y2) * r,
                             (t2 * z1 - t1 * z2) * r};
        Eigen::Vector3f tdir{(s1 * x2 - s2 * x1) * r, (s1 * y2 - s2 * y1) * r,
                             (s1 * z2 - s2 * z1) * r};

        tan1[i1] += sdir;
        tan1[i2] += sdir;
        tan1[i3] += sdir;

        tan2[i1] += tdir;
        tan2[i2] += tdir;
        tan2[i3] += tdir;
    }
    size_t vertexCount = Positions.size();
    for (size_t a = 0; a < vertexCount; a++) {
        const Eigen::Vector3f& n = Normals[a];
        const Eigen::Vector3f& t = tan1[a];

        Eigen::Vector3f xyz = (t - n * n.dot(t)).normalized();
        float w = n.cross(t).dot(tan2[a]);

        tangent[a] = Eigen::Vector4f{xyz.x(), xyz.y(), xyz.z(), w < 0.0f ? -1.0f : 1.0f};
    }
    this->Tangents = std::move(tangent);
}

}  // namespace radray
