#include "forward_capture.h"
#include <fstream>
#include <radray/image_data.h>
#include <radray/utility.h>
#include <radray/runtime/render_framework/render_graph_blit.h>

namespace radray::forward_detail {
void ForwardCapture::CaptureReport(const RenderGraphExecutionReport& report) {
    Report.clear();
    Dot.clear();
    if (Name.empty() || Directory.empty()) return;
    Report = report.ToJson();
    Dot = report.ToDot();
}

bool ForwardCapture::Build(RenderGraph& graph, RenderPipelineContext& context, render::Device& device, RenderSystem& renderer, std::span<RenderGraphOutputBinding> outputs) {
    Pending = false;
    if (Name.empty() || Directory.empty()) return true;
    for (const auto& family : context.ViewFamilies()) {
        if (!family.OutputAvailable || family.SampleCount != 1) continue;
        if (family.OutputFormat != render::TextureFormat::RGBA8_UNORM && family.OutputFormat != render::TextureFormat::BGRA8_UNORM &&
            family.OutputFormat != render::TextureFormat::RGBA8_UNORM_SRGB && family.OutputFormat != render::TextureFormat::BGRA8_UNORM_SRGB) return false;
        Size = family.OutputSize;
        Format = family.OutputFormat;
        Pitch = Align(uint64_t{Size.Width} * 4, device.GetDetail().TextureDataPitchAlignment);
        const auto source = FindGraphOutput(outputs, family.OutputId);
        if (!source) return false;
        const auto descriptor = graph.GetTextureDescriptor(source->Texture);
        if (!descriptor) return false;
        auto value = source->Texture;
        if (descriptor->Format != Format) {
            if (descriptor->Format != render::TextureFormat::RGBA16_FLOAT) return false;
            auto target = *descriptor;
            target.Format = Format;
            target.Hints = render::ResourceHint::None;
            target.Usage = render::TextureUse::RenderTarget | render::TextureUse::CopySource;
            const bool srgb = Format == render::TextureFormat::RGBA8_UNORM_SRGB || Format == render::TextureFormat::BGRA8_UNORM_SRGB;
            value = AddRenderGraphBlit(graph, renderer, device.GetBackend(), value, graph.CreateTexture(target, "Capture.Encoded"), false, !srgb);
        }
        Readback = graph.ReadbackTexture("Forward.Capture", value);
        if (!Readback.IsValid()) return false;
        Pending = true;
        return true;
    }
    return false;
}
bool ForwardCapture::Complete() {
    if (!Pending) return true;
    Pending = false;
    vector<byte> bytes;
    if (!Readback.Read(bytes)) return false;
    ImageData image;
    image.Width = Size.Width;
    image.Height = Size.Height;
    image.Format = ImageFormat::RGBA8_BYTE;
    image.Data = make_unique<byte[]>(uint64_t{Size.Width} * Size.Height * 4);
    const bool bgra = Format == render::TextureFormat::BGRA8_UNORM || Format == render::TextureFormat::BGRA8_UNORM_SRGB;
    for (uint32_t y = 0; y < Size.Height; ++y) {
        const auto* source = bytes.data() + y * Pitch;
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
