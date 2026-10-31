#pragma once

#include <filesystem>
#include <span>

#include <radray/runtime/gpu_env.h>
#include <radray/runtime/gpu_device.h>
#include <radray/runtime/vertex_input.h>

namespace radray::examples::mesh_drawing {

// 资源按依赖顺序声明，析构时逆序释放；仅供本教学样例使用。
struct GpuResources {
    GpuResources() noexcept = default;
    ~GpuResources() noexcept;
    GpuResources(const GpuResources&) = delete;
    GpuResources& operator=(const GpuResources&) = delete;

    bool Initialize(render::RenderBackend backend, const std::filesystem::path& shaderDirectory);
    bool UploadGeometry(std::span<const byte> vertices, std::span<const uint32_t> indices);
    bool CreatePipelines(const VertexInput& input);
    // 前提：颜色已复制到 Readback，队列已等待完成。
    bool SaveImage(const std::filesystem::path& output) const;

    uint32_t Width{800};
    uint32_t Height{600};
    uint64_t RowPitch{0};
    Nullable<unique_ptr<Environment>> Env{nullptr};
    Nullable<unique_ptr<DeviceContext>> Device{nullptr};
    Nullable<unique_ptr<render::Buffer>> VertexBuffer{nullptr};
    Nullable<unique_ptr<render::Buffer>> IndexBuffer{nullptr};
    Nullable<unique_ptr<render::Texture>> ColorTexture{nullptr};
    Nullable<unique_ptr<render::Texture>> DepthTexture{nullptr};
    Nullable<unique_ptr<render::Buffer>> Readback{nullptr};
    Nullable<unique_ptr<render::TextureView>> ColorView{nullptr};
    Nullable<unique_ptr<render::TextureView>> DepthWriteView{nullptr};
    Nullable<unique_ptr<render::Shader>> VertexShader{nullptr};
    Nullable<unique_ptr<render::Shader>> PixelShader{nullptr};
    Nullable<unique_ptr<render::PipelineLayout>> Layout{nullptr};
    Nullable<unique_ptr<render::GraphicsPipelineState>> DepthPipeline{nullptr};
    Nullable<unique_ptr<render::GraphicsPipelineState>> ColorPipeline{nullptr};
    Nullable<unique_ptr<render::CommandAllocator>> Allocator{nullptr};
    Nullable<unique_ptr<render::CommandBuffer>> Command{nullptr};
};

bool PrepareOutputDirectory(const std::filesystem::path& output);
void TransitionTexture(render::CommandBuffer& command, render::Texture& texture,
                       render::TextureStates before, render::TextureStates after);

}  // namespace radray::examples::mesh_drawing
