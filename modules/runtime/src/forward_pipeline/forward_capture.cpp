#include "forward_capture.h"
#include <fstream>
#include <radray/image_data.h>
#include <radray/utility.h>

namespace radray::forward_detail {
bool ForwardCapture::Build(RenderGraph& graph, RenderPipelineContext& context, render::Device& device) {
    Pending = false;
    if (Name.empty() || Directory.empty()) return true;
    for (const auto& family : context.ViewFamilies()) {
        if (!family.OutputAvailable || family.SampleCount != 1) continue;
        if (family.OutputFormat != render::TextureFormat::RGBA8_UNORM && family.OutputFormat != render::TextureFormat::BGRA8_UNORM &&
            family.OutputFormat != render::TextureFormat::RGBA8_UNORM_SRGB && family.OutputFormat != render::TextureFormat::BGRA8_UNORM_SRGB) return false;
        Size = family.OutputSize;
        Format = family.OutputFormat;
        Pitch = Align(uint64_t{Size.Width} * 4, device.GetDetail().TextureDataPitchAlignment);
        const uint64_t bytes = Pitch * Size.Height;
        if (!Readback || Readback->GetDesc().Size != bytes) {
            auto buffer = device.CreateBuffer({bytes, render::MemoryType::ReadBack, render::BufferUse::MapRead | render::BufferUse::CopyDestination, {}});
            if (!buffer) return false;
            Readback = buffer.Release();
        }
        Import = {Readback.get(), Readback->GetDesc(), render::BufferState::CopyDestination};
        const auto output = context.ImportOutput(graph, family.OutputId);
        const auto host = graph.ImportBuffer(Import, "Forward.Capture", RenderGraphExternalAccess::ObservableOutput);
        graph.AddCopyTextureToBufferPass("Forward.Capture", output, host);
        graph.AddComputePass<uint32_t>("Forward.CaptureHostVisibility", [=](uint32_t&, RenderGraphComputeBuilder& builder) {
            builder.ReadBuffer(host, RgBufferAccess::HostRead); builder.SetSideEffect(); }, +[](const uint32_t&, RenderGraphComputeContext&) {});
        Pending = true;
        return true;
    }
    return false;
}
bool ForwardCapture::Complete() {
    if (!Pending) return true;
    Pending = false;
    ScopedBufferMap map{Readback.get(), {0, Pitch * Size.Height}};
    if (!map) return false;
    ImageData image;
    image.Width = Size.Width;
    image.Height = Size.Height;
    image.Format = ImageFormat::RGBA8_BYTE;
    image.Data = make_unique<byte[]>(uint64_t{Size.Width} * Size.Height * 4);
    const bool bgra = Format == render::TextureFormat::BGRA8_UNORM || Format == render::TextureFormat::BGRA8_UNORM_SRGB;
    for (uint32_t y = 0; y < Size.Height; ++y) {
        const auto* source = static_cast<const byte*>(map.Data()) + y * Pitch;
        auto* destination = image.Data.get() + uint64_t{y} * Size.Width * 4;
        std::memcpy(destination, source, uint64_t{Size.Width} * 4);
        if (bgra)
            for (uint32_t x = 0; x < Size.Width; ++x) std::swap(destination[x * 4], destination[x * 4 + 2]);
    }
    std::error_code error;
    std::filesystem::create_directories(Directory, error);
    if (error) return false;
    if (!image.WritePNG({(Directory / (Name + ".png")).string(), false})) return false;
    std::ofstream report{Directory / (Name + ".json")};
    report << Report;
    std::ofstream dot{Directory / (Name + ".dot")};
    dot << Dot;
    Name.clear();
    return bool(report) && bool(dot);
}
}  // namespace radray::forward_detail
