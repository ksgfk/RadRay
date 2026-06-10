#include <radray/runtime/static_mesh.h>

#include <limits>
#include <utility>

namespace radray {
namespace {

bool IsByteRangeValid(size_t offset, size_t stride, uint32_t count, size_t elementSize, size_t bufferSize) noexcept {
    if (count == 0 || stride == 0 || elementSize == 0) {
        return false;
    }
    if (offset > bufferSize) {
        return false;
    }
    if (count > 1 && stride > (std::numeric_limits<size_t>::max() - offset) / static_cast<size_t>(count - 1)) {
        return false;
    }

    const size_t lastOffset = offset + static_cast<size_t>(count - 1) * stride;
    if (lastOffset > bufferSize) {
        return false;
    }
    return elementSize <= bufferSize - lastOffset;
}

bool IsVertexBufferEntryValid(const VertexBufferEntry& entry, const MeshResource& meshResource, uint32_t vertexCount) noexcept {
    if (entry.BufferIndex >= meshResource.Bins.size()) {
        return false;
    }

    const uint32_t elementSize = GetVertexDataSizeInBytes(entry.Type, entry.ComponentCount);
    if (elementSize == 0 || entry.Stride == 0) {
        return false;
    }
    if (entry.Offset > entry.Stride || elementSize > entry.Stride - entry.Offset) {
        return false;
    }

    return IsByteRangeValid(
        static_cast<size_t>(entry.Offset),
        static_cast<size_t>(entry.Stride),
        vertexCount,
        static_cast<size_t>(elementSize),
        meshResource.Bins[entry.BufferIndex].GetSize());
}

bool IsIndexBufferEntryValid(const IndexBufferEntry& entry, const MeshResource& meshResource) noexcept {
    if (entry.BufferIndex >= meshResource.Bins.size()) {
        return false;
    }
    if (entry.Stride != sizeof(uint16_t) && entry.Stride != sizeof(uint32_t)) {
        return false;
    }

    return IsByteRangeValid(
        static_cast<size_t>(entry.Offset),
        static_cast<size_t>(entry.Stride),
        entry.IndexCount,
        static_cast<size_t>(entry.Stride),
        meshResource.Bins[entry.BufferIndex].GetSize());
}

bool IsPrimitiveValid(const MeshPrimitive& primitive, const MeshResource& meshResource) noexcept {
    if (primitive.VertexCount == 0 || primitive.VertexBuffers.empty()) {
        return false;
    }
    if (!IsIndexBufferEntryValid(primitive.IndexBuffer, meshResource)) {
        return false;
    }

    for (const VertexBufferEntry& entry : primitive.VertexBuffers) {
        if (!IsVertexBufferEntryValid(entry, meshResource, primitive.VertexCount)) {
            return false;
        }
    }

    return true;
}

bool IsSectionValid(const StaticMeshSection& section, const MeshResource& meshResource) noexcept {
    if (section.PrimitiveIndex >= meshResource.Primitives.size()) {
        return false;
    }
    if (section.IndexCount == 0 || section.MinVertexIndex > section.MaxVertexIndex) {
        return false;
    }

    const MeshPrimitive& primitive = meshResource.Primitives[section.PrimitiveIndex];
    if (section.FirstIndex > primitive.IndexBuffer.IndexCount) {
        return false;
    }
    if (section.IndexCount > primitive.IndexBuffer.IndexCount - section.FirstIndex) {
        return false;
    }
    return section.MaxVertexIndex < primitive.VertexCount;
}

}  // namespace

StaticMeshSection::StaticMeshSection() noexcept
    : PrimitiveIndex(0),
      FirstIndex(0),
      IndexCount(0),
      MinVertexIndex(0),
      MaxVertexIndex(0) {
}

StaticMeshSection::StaticMeshSection(
    uint32_t primitiveIndex,
    uint32_t firstIndex,
    uint32_t indexCount,
    uint32_t minVertexIndex,
    uint32_t maxVertexIndex) noexcept
    : PrimitiveIndex(primitiveIndex),
      FirstIndex(firstIndex),
      IndexCount(indexCount),
      MinVertexIndex(minVertexIndex),
      MaxVertexIndex(maxVertexIndex) {
}

StaticMesh::StaticMesh() noexcept
    : _boundsMin(Eigen::Vector3f::Zero()),
      _boundsMax(Eigen::Vector3f::Zero()) {
}

StaticMesh::~StaticMesh() noexcept = default;

void StaticMesh::OnLoad() {
}

void StaticMesh::OnUnload() {
    ClearCPUData();
}

AssetTypeId StaticMesh::GetTypeId() const noexcept {
    return asset_type_id_v<StaticMesh>;
}

MeshResource& StaticMesh::GetMeshResource() noexcept {
    return _meshResource;
}

const MeshResource& StaticMesh::GetMeshResource() const noexcept {
    return _meshResource;
}

void StaticMesh::SetMeshResource(MeshResource meshResource) {
    _meshResource = std::move(meshResource);
}

vector<StaticMeshSection>& StaticMesh::GetSections() noexcept {
    return _sections;
}

const vector<StaticMeshSection>& StaticMesh::GetSections() const noexcept {
    return _sections;
}

void StaticMesh::SetSections(vector<StaticMeshSection> sections) {
    _sections = std::move(sections);
}

const Eigen::Vector3f& StaticMesh::GetBoundsMin() const noexcept {
    return _boundsMin;
}

const Eigen::Vector3f& StaticMesh::GetBoundsMax() const noexcept {
    return _boundsMax;
}

void StaticMesh::SetBounds(const Eigen::Vector3f& boundsMin, const Eigen::Vector3f& boundsMax) noexcept {
    _boundsMin = boundsMin;
    _boundsMax = boundsMax;
}

bool StaticMesh::IsValid() const noexcept {
    if (_meshResource.Primitives.empty()) {
        return false;
    }

    for (const MeshPrimitive& primitive : _meshResource.Primitives) {
        if (!IsPrimitiveValid(primitive, _meshResource)) {
            return false;
        }
    }

    for (const StaticMeshSection& section : _sections) {
        if (!IsSectionValid(section, _meshResource)) {
            return false;
        }
    }

    return true;
}

void StaticMesh::ClearCPUData() noexcept {
    _meshResource = MeshResource{};
    _sections.clear();
    _boundsMin = Eigen::Vector3f::Zero();
    _boundsMax = Eigen::Vector3f::Zero();
}

}  // namespace radray
