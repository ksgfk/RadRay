#include <radray/runtime/static_mesh.h>

#include <array>
#include <cstddef>
#include <limits>
#include <span>
#include <string_view>

#include <radray/file.h>
#include <radray/runtime/asset_bundle_descriptors.h>
#include <radray/runtime/gpu_system.h>
#include <utility>

#include <radray/render/rhi.h>

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
      MaxVertexIndex(0),
      VertexOffset(0) {
}

StaticMeshSection::StaticMeshSection(
    uint32_t primitiveIndex,
    uint32_t firstIndex,
    uint32_t indexCount,
    uint32_t minVertexIndex,
    uint32_t maxVertexIndex,
    int32_t vertexOffset) noexcept
    : PrimitiveIndex(primitiveIndex),
      FirstIndex(firstIndex),
      IndexCount(indexCount),
      MinVertexIndex(minVertexIndex),
      MaxVertexIndex(maxVertexIndex),
      VertexOffset(vertexOffset) {
}

bool IsStaticMeshDataValid(
    const MeshResource& meshResource,
    std::span<const StaticMeshSection> sections) noexcept {
    if (meshResource.Primitives.empty()) {
        return false;
    }
    for (const MeshPrimitive& primitive : meshResource.Primitives) {
        if (!IsPrimitiveValid(primitive, meshResource)) {
            return false;
        }
    }
    for (const StaticMeshSection& section : sections) {
        if (!IsSectionValid(section, meshResource)) {
            return false;
        }
    }
    return true;
}

StaticMesh::StaticMesh(
    MeshResource meshResource,
    vector<StaticMeshSection> sections,
    const Eigen::Vector3f& boundsMin,
    const Eigen::Vector3f& boundsMax,
    GpuMesh renderMesh) noexcept
    : _meshResource(std::move(meshResource)),
      _sections(std::move(sections)),
      _boundsMin(boundsMin),
      _boundsMax(boundsMax),
      _renderMesh(std::move(renderMesh)) {
}

StaticMesh::~StaticMesh() noexcept = default;

bool StaticMesh::IsValid() const noexcept {
    return IsStaticMeshDataValid(_meshResource, _sections);
}

void StaticMesh::OnUnload(AssetManager& manager) {
    // 【必须延迟】: SceneProxy 缓存 GpuMesh::DrawData* 并录进命令列表 (见
    // primitive_scene_proxy.h), 那些 buffer 要活到 fence 之后。
    //
    // 【整包交出】: buffer 之间无相互依赖, 但整包交出的形状让"销毁顺序在哪里表达"这件事
    // 在所有资产上一致, 而不是每种资产各自决定。见 AssetManager::DeferDestroy。
    manager.DeferDestroy([mesh = std::move(_renderMesh)]() noexcept {});
    _renderMesh = GpuMesh{};
}

RuntimeTypeId StaticMesh::GetTypeId() const noexcept {
    return runtime_type_id_v<StaticMesh>;
}

namespace {

class MeshPayloadReader {
public:
    explicit MeshPayloadReader(std::span<const byte> bytes) noexcept : _bytes(bytes) {}

    bool ReadU16(uint16_t& value) noexcept {
        if (_position + 2 > _bytes.size()) {
            return false;
        }
        value = static_cast<uint16_t>(std::to_integer<uint8_t>(_bytes[_position])) |
                (static_cast<uint16_t>(std::to_integer<uint8_t>(_bytes[_position + 1])) << 8u);
        _position += 2;
        return true;
    }

    bool ReadU32(uint32_t& value) noexcept {
        if (_position + 4 > _bytes.size()) {
            return false;
        }
        value = 0;
        for (uint32_t i = 0; i < 4; ++i) {
            value |= static_cast<uint32_t>(std::to_integer<uint8_t>(_bytes[_position + i])) << (8u * i);
        }
        _position += 4;
        return true;
    }

    bool ReadU64(uint64_t& value) noexcept {
        if (_position + 8 > _bytes.size()) {
            return false;
        }
        value = 0;
        for (uint32_t i = 0; i < 8; ++i) {
            value |= static_cast<uint64_t>(std::to_integer<uint8_t>(_bytes[_position + i])) << (8u * i);
        }
        _position += 8;
        return true;
    }

    bool ReadBytes(size_t size, std::span<const byte>& value) noexcept {
        if (size > _bytes.size() - _position) {
            return false;
        }
        value = _bytes.subspan(_position, size);
        _position += size;
        return true;
    }

    bool ReadString(string& value, size_t maximum) {
        uint32_t size = 0;
        if (!ReadU32(size) || size > maximum) {
            return false;
        }
        std::span<const byte> bytes;
        if (!ReadBytes(size, bytes)) {
            return false;
        }
        value.resize(size);
        for (size_t i = 0; i < size; ++i) {
            value[i] = static_cast<char>(std::to_integer<uint8_t>(bytes[i]));
        }
        return true;
    }

    size_t Remaining() const noexcept { return _bytes.size() - _position; }

private:
    std::span<const byte> _bytes;
    size_t _position{0};
};

bool ParseStaticMeshPayload(std::span<const byte> bytes, MeshResource& mesh) {
    constexpr std::array<uint8_t, 8> kMagic{'R', 'R', 'M', 'E', 'S', 'H', '0', '1'};
    constexpr size_t kMaxBins = 4096;
    constexpr size_t kMaxPrimitives = 4096;
    constexpr size_t kMaxAttributesPerPrimitive = 64;
    constexpr size_t kMaxStringBytes = 4096;
    constexpr uint64_t kMaxPayloadBytes = 256ull * 1024ull * 1024ull;

    if (bytes.size() > kMaxPayloadBytes || bytes.size() < kMagic.size()) {
        return false;
    }
    for (size_t i = 0; i < kMagic.size(); ++i) {
        if (std::to_integer<uint8_t>(bytes[i]) != kMagic[i]) {
            return false;
        }
    }

    MeshPayloadReader reader{bytes.subspan(kMagic.size())};
    uint32_t version = 0;
    uint32_t binCount = 0;
    uint32_t primitiveCount = 0;
    if (!reader.ReadU32(version) || version != 1 || !reader.ReadU32(binCount) ||
        !reader.ReadU32(primitiveCount) || binCount == 0 || primitiveCount == 0 ||
        binCount > kMaxBins || primitiveCount > kMaxPrimitives || !reader.ReadString(mesh.Name, kMaxStringBytes)) {
        return false;
    }

    mesh.Bins.reserve(binCount);
    uint64_t totalBinBytes = 0;
    for (uint32_t i = 0; i < binCount; ++i) {
        uint64_t size = 0;
        if (!reader.ReadU64(size) || size == 0 || size > kMaxPayloadBytes ||
            totalBinBytes > kMaxPayloadBytes - size) {
            return false;
        }
        std::span<const byte> bin;
        if (size > static_cast<uint64_t>(std::numeric_limits<size_t>::max()) ||
            !reader.ReadBytes(static_cast<size_t>(size), bin)) {
            return false;
        }
        mesh.Bins.emplace_back(bin);
        totalBinBytes += size;
    }

    mesh.Primitives.reserve(primitiveCount);
    for (uint32_t i = 0; i < primitiveCount; ++i) {
        MeshPrimitive primitive;
        uint32_t attributeCount = 0;
        uint16_t componentCount = 0;
        if (!reader.ReadU32(primitive.VertexCount) || !reader.ReadU32(primitive.IndexBuffer.BufferIndex) ||
            !reader.ReadU32(primitive.IndexBuffer.IndexCount) || !reader.ReadU32(primitive.IndexBuffer.Offset) ||
            !reader.ReadU32(primitive.IndexBuffer.Stride) || !reader.ReadU32(attributeCount) ||
            attributeCount == 0 || attributeCount > kMaxAttributesPerPrimitive) {
            return false;
        }
        primitive.VertexBuffers.reserve(attributeCount);
        for (uint32_t j = 0; j < attributeCount; ++j) {
            VertexBufferEntry attribute;
            uint16_t encodedType = 0;
            if (!reader.ReadString(attribute.Semantic, kMaxStringBytes) ||
                !reader.ReadU32(attribute.SemanticIndex) || !reader.ReadU32(attribute.BufferIndex) ||
                !reader.ReadU16(encodedType) || !reader.ReadU16(componentCount) ||
                !reader.ReadU32(attribute.Offset) || !reader.ReadU32(attribute.Stride) ||
                encodedType > static_cast<uint16_t>(VertexDataType::SINT)) {
                return false;
            }
            attribute.Type = static_cast<VertexDataType>(encodedType);
            attribute.ComponentCount = componentCount;
            primitive.VertexBuffers.push_back(std::move(attribute));
        }
        mesh.Primitives.push_back(std::move(primitive));
    }
    return reader.Remaining() == 0 && IsStaticMeshDataValid(mesh, {});
}

}  // namespace

task<AssetLoadResult> LoadStaticMesh(
    FrameUploadScheduler& frameUploads,
    MeshResource meshResource) {
    // 阶段(均为协程内部事务):
    //  1) CPU 校验网格数据。
    //  2) 两阶段 GPU 上传:co_await FrameUploadScheduler::BeginUpload 挂起至帧顶拿 cmd/uploader,
    //     inline 录制 copy 进当前帧 cmdbuffer,再 co_await WaitGpu 跨帧等 fence。
    //  3) 一次性构造内容与资产。
    // 【校验先于上传】: 无效数据不该占用 upload 带宽, 也不该建出半成品内容。
    if (!IsStaticMeshDataValid(meshResource, {})) {
        co_return AssetLoadResult::Failure("static mesh resource is invalid");
    }

    // GPU 上传:两阶段 await(无 callback)。BeginUpload 挂起至帧顶拿到 cmd/uploader,
    // 在本协程里 inline 录制 copy,再 co_await WaitGpu 等该 flight 的 fence。
    FrameUploadScope frame = co_await frameUploads.BeginUpload();
    std::optional<GpuMesh> renderMesh =
        frame.GetUploader().UploadMeshResource(frame.GetCommandBuffer(), meshResource);
    if (!renderMesh.has_value()) {
        co_return AssetLoadResult::Failure("static mesh upload recording failed");
    }
    co_await frame.WaitGpu();

    co_return AssetLoadResult::Success(make_unique<StaticMesh>(
        std::move(meshResource),
        vector<StaticMeshSection>{},
        Eigen::Vector3f::Zero(),
        Eigen::Vector3f::Zero(),
        std::move(renderMesh.value())));
}

task<AssetLoadResult> LoadStaticMeshAssetPayload(
    FrameUploadScheduler& frameUploads,
    vector<byte> encodedBytes) {
    MeshResource mesh;
    if (!ParseStaticMeshPayload(encodedBytes, mesh)) {
        co_return AssetLoadResult::Failure(
            AssetLoadErrorCode::PayloadFailure,
            "static mesh payload has an invalid RRMESH01 encoding");
    }
    co_return co_await LoadStaticMesh(frameUploads, std::move(mesh));
}

task<AssetLoadResult> LoadStaticMeshBundle(AssetManager& manager, BundleAssetLoadData data) {
    const auto* descriptor = dynamic_cast<const StaticMeshAssetDescriptor*>(data.Entry.Descriptor.get());
    if (descriptor == nullptr || !data.Entry.Locator.has_value()) {
        co_return AssetLoadResult::Failure(
            AssetLoadErrorCode::InvalidDescriptor,
            "StaticMesh Bundle descriptor is missing its locator");
    }
    (void)descriptor;
    const std::filesystem::path path = data.Root / std::filesystem::path{data.Entry.Locator->GetValue()};
    std::optional<vector<byte>> encoded = ReadBinaryFile(path);
    if (!encoded.has_value()) {
        co_return AssetLoadResult::Failure(
            AssetLoadErrorCode::PayloadFailure,
            "StaticMesh payload file could not be read");
    }
    Nullable<FrameUploadScheduler*> frameUploads = manager.GetFrameUploadScheduler();
    if (!frameUploads) {
        co_return AssetLoadResult::Failure(
            AssetLoadErrorCode::CapabilityUnavailable,
            "StaticMesh requires a wired FrameUploadScheduler");
    }
    co_return co_await LoadStaticMeshAssetPayload(*frameUploads, std::move(*encoded));
}

}  // namespace radray
