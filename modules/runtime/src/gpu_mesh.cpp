#include <radray/runtime/gpu_mesh.h>
#include <radray/runtime/gpu_resource.h>
#include <radray/runtime/static_mesh.h>
#include <algorithm>

namespace radray {
Nullable<unique_ptr<GpuMesh>> AllocateRuntimeMesh(render::Device& device, const MeshResource& source, uint64_t byteLimit) {
    if (!IsStaticMeshDataValid(source, {})) return nullptr;
    auto result = make_unique<GpuMesh>();
    uint64_t total = 0;
    for (const auto& bin : source.Bins) {
        total = (total + 3) & ~uint64_t{3};
        if (total > byteLimit || bin.GetSize() > byteLimit - total) return nullptr;
        result->BinRanges.push_back({total, bin.GetSize()});
        total += bin.GetSize();
    }
    if (!total) return nullptr;
    auto buffer = device.CreateBuffer({total, render::MemoryType::Device,
                                       render::BufferUse::Vertex | render::BufferUse::Index | render::BufferUse::CopyDestination | render::BufferUse::CopySource});
    if (!buffer) return nullptr;
    result->Buffers.push_back(buffer.Release());
    for (const auto& primitive : source.Primitives) {
        GpuMesh::DrawData draw;
        draw.Topology = primitive.Topology;
        vector<std::pair<uint32_t, uint32_t>> streams;
        for (const auto& input : primitive.VertexBuffers) {
            if (input.ComponentCount < 1 || input.ComponentCount > 4) return nullptr;
            const auto key = std::pair{input.BufferIndex, input.Stride};
            auto found = std::find(streams.begin(), streams.end(), key);
            if (found == streams.end()) {
                streams.push_back(key);
                found = streams.end() - 1;
                const uint32_t slot = static_cast<uint32_t>(found - streams.begin());
                draw.Layout.Streams.push_back({slot, input.Stride, render::VertexStepMode::Vertex});
                const auto& range = result->BinRanges[input.BufferIndex];
                draw.VertexBuffers.push_back({slot, {result->Buffers[0].get(), range.Offset, range.Size}});
            }
            const render::VertexFormat floats[]{render::VertexFormat::FLOAT32, render::VertexFormat::FLOAT32X2, render::VertexFormat::FLOAT32X3, render::VertexFormat::FLOAT32X4};
            const render::VertexFormat uints[]{render::VertexFormat::UINT32, render::VertexFormat::UINT32X2, render::VertexFormat::UINT32X3, render::VertexFormat::UINT32X4};
            const render::VertexFormat ints[]{render::VertexFormat::SINT32, render::VertexFormat::SINT32X2, render::VertexFormat::SINT32X3, render::VertexFormat::SINT32X4};
            const auto format = input.Type == VertexDataType::FLOAT ? floats[input.ComponentCount - 1] : input.Type == VertexDataType::UINT ? uints[input.ComponentCount - 1]
                                                                                                                                            : ints[input.ComponentCount - 1];
            draw.Layout.Attributes.push_back({input.Semantic, input.SemanticIndex, format, uint32_t(found - streams.begin()), input.Offset, uint32_t(draw.Layout.Attributes.size())});
        }
        if (!ValidateGeometryVertexLayout(draw.Layout)) return nullptr;
        const auto offset = result->BinRanges[primitive.IndexBuffer.BufferIndex].Offset + primitive.IndexBuffer.Offset;
        if (offset > UINT32_MAX) return nullptr;
        draw.Ibv = {result->Buffers[0].get(), uint32_t(offset), primitive.IndexBuffer.Stride};
        result->Draws.push_back(std::move(draw));
    }
    return result;
}
bool TryUploadRuntimeMesh(ResourceUploader& uploader, render::CommandBuffer* command, const MeshResource& source, const GpuMesh& mesh) {
    if (mesh.Buffers.size() != 1 || mesh.BinRanges.size() != source.Bins.size()) return false;
    vector<BufferUploadRequest> uploads;
    for (size_t i = 0; i < source.Bins.size(); ++i) {
        if (source.Bins[i].GetSize() != mesh.BinRanges[i].Size) return false;
        uploads.push_back({source.Bins[i].GetData(), mesh.Buffers[0].get(), mesh.BinRanges[i].Offset,
                           render::BufferState::Common, render::BufferState::Vertex | render::BufferState::Index});
    }
    return uploader.TryUploadBufferRanges(command, uploads);
}
}  // namespace radray
