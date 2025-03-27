#include <radray/triangle_mesh.h>

#include <new>
#include <numbers>
#include <limits>
#include <radray/vertex_data.h>

namespace radray {

bool TriangleMesh::IsValid() const noexcept {
    return Indices.size() > 0 &&
           Positions.size() > 0 &&
           Positions.size() <= std::numeric_limits<uint32_t>::max() &&
           (Normals.size() == 0 || Normals.size() == Positions.size()) &&
           (UV0.size() == 0 || UV0.size() == Positions.size()) &&
           (Tangents.size() == 0 || Tangents.size() == Positions.size()) &&
           (Color0.size() == 0 || Color0.size() == Positions.size());
}

uint64_t TriangleMesh::GetVertexByteSize() const noexcept {
    return Positions.size() * 12 +
           Normals.size() * 12 +
           UV0.size() * 8 +
           Tangents.size() * 16 +
           Color0.size() * 16;
}

void TriangleMesh::ToVertexData(VertexData* data) const noexcept {
    {
        data->Layouts.emplace_back(VertexLayout{radray::string{VertexSemantic::POSITION}, 0, 12, 0});
        uint32_t byteOffset = 12;
        if (Normals.size() > 0) {
            data->Layouts.emplace_back(VertexLayout{radray::string{VertexSemantic::NORMAL}, 0, 12, byteOffset});
            byteOffset += 12;
        }
        if (UV0.size() > 0) {
            data->Layouts.emplace_back(VertexLayout{radray::string{VertexSemantic::TEXCOORD}, 0, 8, byteOffset});
            byteOffset += 8;
        }
        if (Tangents.size() > 0) {
            data->Layouts.emplace_back(VertexLayout{radray::string{VertexSemantic::TANGENT}, 0, 16, byteOffset});
            byteOffset += 16;
        }
        if (Color0.size() > 0) {
            data->Layouts.emplace_back(VertexLayout{radray::string{VertexSemantic::COLOR}, 0, 16, byteOffset});
            byteOffset += 16;
        }
        uint64_t byteSize = byteOffset * Positions.size();
        RADRAY_ASSERT(byteSize == GetVertexByteSize());
        if (byteSize > std::numeric_limits<uint32_t>::max()) {
            RADRAY_ABORT("too large mesh {}", byteSize);
        }
        data->VertexData = radray::make_unique<byte[]>(byteSize);
        data->VertexSize = (uint32_t)byteSize;
        float* target = std::launder(reinterpret_cast<float*>(data->VertexData.get()));
        for (size_t i = 0; i < Positions.size(); i++) {
            std::copy(Positions[i].begin(), Positions[i].end(), target);
            target += 3;
            if (Normals.size() > 0) {
                std::copy(Normals[i].begin(), Normals[i].end(), target);
                target += 3;
            }
            if (UV0.size() > 0) {
                std::copy(UV0[i].begin(), UV0[i].end(), target);
                target += 2;
            }
            if (Tangents.size() > 0) {
                std::copy(Tangents[i].begin(), Tangents[i].end(), target);
                target += 4;
            }
            if (Color0.size() > 0) {
                std::copy(Color0[i].begin(), Color0[i].end(), target);
                target += 4;
            }
        }
    }
    {
        RADRAY_ASSERT(Indices.size() <= std::numeric_limits<uint32_t>::max());
        if (Positions.size() <= std::numeric_limits<uint16_t>::max()) {
            uint64_t byteSize = Indices.size() * sizeof(uint16_t);
            if (byteSize > std::numeric_limits<uint32_t>::max()) {
                RADRAY_ABORT("too large mesh {}", byteSize);
            }
            data->IndexType = VertexIndexType::UInt16;
            data->IndexSize = (uint32_t)byteSize;
            data->IndexData = radray::make_unique<byte[]>(data->IndexSize);
            data->IndexCount = (uint32_t)Indices.size();
            uint16_t* target = std::launder(reinterpret_cast<uint16_t*>(data->IndexData.get()));
            for (auto&& i : Indices) {
                *target = static_cast<uint16_t>(i);
                target++;
            }
        } else if (Positions.size() <= std::numeric_limits<uint32_t>::max()) {
            uint64_t byteSize = Indices.size() * sizeof(uint32_t);
            if (byteSize > std::numeric_limits<uint32_t>::max()) {
                RADRAY_ABORT("too large mesh {}", byteSize);
            }
            data->IndexType = VertexIndexType::UInt32;
            data->IndexSize = (uint32_t)byteSize;
            data->IndexData = radray::make_unique<byte[]>(data->IndexSize);
            data->IndexCount = (uint32_t)Indices.size();
            std::memcpy(data->IndexData.get(), Indices.data(), data->IndexSize);
        } else {
            RADRAY_ABORT("too large mesh {}", Positions.size());
        }
    }
}

void TriangleMesh::InitAsCube(float halfExtend) noexcept {
    Positions = radray::vector<Eigen::Vector3f>{
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
    Normals = radray::vector<Eigen::Vector3f>{
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
    UV0 = radray::vector<Eigen::Vector2f>{
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
    Tangents = radray::vector<Eigen::Vector4f>{
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
    Indices = radray::vector<uint32_t>{0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 8, 9, 10, 8, 10, 11, 12, 15, 14, 12, 14, 13, 16, 17, 18, 16, 18, 19, 20, 23, 22, 20, 22, 21};
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

}  // namespace radray
