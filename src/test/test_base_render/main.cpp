#include <radray/logger.h>
#include <radray/utility.h>
#include <radray/render/device.h>
#include <radray/render/dxc.h>
#include <radray/render/spvc.h>
#include <radray/render/command_queue.h>
#include <radray/render/command_pool.h>
#include <radray/render/command_buffer.h>
#include <radray/render/shader.h>

using namespace radray;
using namespace radray::render;

int main() {
    std::shared_ptr<Device> device;
#if defined(RADRAY_PLATFORM_WINDOWS)
    device = CreateDevice(D3D12DeviceDescriptor{}).value();
#elif defined(RADRAY_PLATFORM_MACOS) || defined(RADRAY_PLATFORM_IOS)
    device = CreateDevice(MetalDeviceDescriptor{}).value();
#endif
    auto cmdQueue = device->GetCommandQueue(QueueType::Direct, 0).value();
    auto cmdPool = cmdQueue->CreateCommandPool().value();
    auto cmdBuffer = cmdPool->CreateCommandBuffer().value();
    if (cmdBuffer == nullptr) {
        std::abort();
    }
    auto dxc = CreateDxc().value();
#if defined(RADRAY_PLATFORM_MACOS) || defined(RADRAY_PLATFORM_IOS)
    bool isSpirv = true;
#elif defined(RADRAY_PLATFORM_WINDOWS)
    bool isSpirv = false;
#endif
    radray::shared_ptr<Shader> vs, ps;
    {
        std::string_view includes[] = {std::string_view{"shaders"}};
        auto color = ReadText(std::filesystem::path("shaders") / "DefaultVS.hlsl").value();
        auto outv = dxc->Compile(
            color,
            "main",
            ShaderStage::Vertex,
            HlslShaderModel::SM60,
            true,
            {},
            includes,
            isSpirv);
        auto outp = outv.value();
        RADRAY_INFO_LOG("type={} size={}", outp.category, outp.data.size());
#if defined(RADRAY_PLATFORM_MACOS) || defined(RADRAY_PLATFORM_IOS)
        auto msl = SpirvToMsl(outp.data, MslVersion::MSL24, MslPlatform::Macos).value();
        // RADRAY_INFO_LOG("to msl\n{}", msl.Msl);
        // RADRAY_INFO_LOG("refl\n{}", msl.SpvReflJson);
        std::span<const byte> blob{reinterpret_cast<const byte*>(msl.Msl.data()), msl.Msl.size()};
        radray::string entry = msl.EntryPoints.at(0).Name;
        ShaderReflection refl = MslReflection{};
#elif defined(RADRAY_PLATFORM_WINDOWS)
        std::span<const byte> blob = outp.data;
        std::string_view entry = "outv";
        ShaderReflection refl = dxc->GetDxilReflection(ShaderStage::Vertex, outp.refl).value();
#endif
        auto shader = device->CreateShader(blob, refl, ShaderStage::Vertex, entry, "colorVS").value();
        RADRAY_INFO_LOG("shader name {}", shader->Name);
        vs = std::move(shader);
    }
    {
        std::string_view includes[] = {std::string_view{"shaders"}};
        auto color = ReadText(std::filesystem::path("shaders") / "DefaultPS.hlsl").value();
        auto outv = dxc->Compile(
            color,
            "main",
            ShaderStage::Pixel,
            HlslShaderModel::SM60,
            true,
            {},
            includes,
            isSpirv);
        auto outp = outv.value();
        RADRAY_INFO_LOG("type={} size={}", outp.category, outp.data.size());
#if defined(RADRAY_PLATFORM_MACOS) || defined(RADRAY_PLATFORM_IOS)
        auto msl = SpirvToMsl(outp.data, MslVersion::MSL24, MslPlatform::Macos).value();
        // RADRAY_INFO_LOG("to msl\n{}", msl.Msl);
        // RADRAY_INFO_LOG("refl\n{}", msl.SpvReflJson);
        std::span<const byte> blob{reinterpret_cast<const byte*>(msl.Msl.data()), msl.Msl.size()};
        radray::string entry = msl.EntryPoints.at(0).Name;
        ShaderReflection refl = MslReflection{};
#elif defined(RADRAY_PLATFORM_WINDOWS)
        std::span<const byte> blob = outp.data;
        std::string_view entry = "outv";
        ShaderReflection refl = dxc->GetDxilReflection(ShaderStage::Pixel, outp.refl).value();
#endif
        auto shader = device->CreateShader(blob, refl, ShaderStage::Pixel, entry, "colorPS").value();
        RADRAY_INFO_LOG("shader name {}", shader->Name);
        ps = std::move(shader);
    }
    {
        Shader* shaders[] = {vs.get(), ps.get()};
        auto rootSig = device->CreateRootSignature(shaders);
        RADRAY_INFO_LOG("root sig done? {}", rootSig.has_value());
        GraphicsPipelineStateDescriptor desc{
            "color pso",
            rootSig.value().get(),
            vs.get(),
            ps.get(),
            {VertexBufferLayout{
                56,
                VertexStepMode::Vertex,
                {
                    VertexElement{0, VertexSemantic::Position, 0, VertexFormat::FLOAT32X3, 0},
                    VertexElement{12, VertexSemantic::Normal, 0, VertexFormat::FLOAT32X3, 1},
                    VertexElement{24, VertexSemantic::Tangent, 0, VertexFormat::FLOAT32X4, 2},
                    VertexElement{40, VertexSemantic::Texcoord, 0, VertexFormat::FLOAT32X2, 3},
                    VertexElement{48, VertexSemantic::Texcoord, 1, VertexFormat::FLOAT32X2, 4},
                }}},
            PrimitiveState{
                PrimitiveTopology::TriangleList,
                IndexFormat::UINT32,
                FrontFace::CCW,
                CullMode::Back,
                PolygonMode::Fill,
                false,
                false},
            DepthStencilState{
                TextureFormat::D32_FLOAT,
                CompareFunction::LessEqual,
                StencilState{
                    StencilFaceState{
                        CompareFunction::Always,
                        StencilOperation::Keep,
                        StencilOperation::Keep,
                        StencilOperation::Keep},
                    StencilFaceState{
                        CompareFunction::Always,
                        StencilOperation::Keep,
                        StencilOperation::Keep,
                        StencilOperation::Keep},
                    0xFF,
                    0xFF},
                DepthBiasState{0, 0.0f, 0.0f},
                true,
                false},
            MultiSampleState{
                1,
                0,
                false},
            {ColorTargetState{
                TextureFormat::BGRA8_UNORM,
                {{BlendFactor::One,
                  BlendFactor::Zero,
                  BlendOperation::Add},
                 {BlendFactor::One,
                  BlendFactor::Zero,
                  BlendOperation::Add}},
                static_cast<ColorWrites>(ColorWrite::All),
                false}},
            true};
        auto pso = device->CreateGraphicsPipeline(desc);
        RADRAY_INFO_LOG("pso done? {}", pso.has_value());
    }
    return 0;
}
