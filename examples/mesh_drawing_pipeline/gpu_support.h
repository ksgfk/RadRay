#pragma once

#include <filesystem>
#include <span>
#include <string_view>

#include <radray/runtime/gpu_env.h>
#include <radray/runtime/gpu_device.h>
#include <radray/runtime/vertex_input.h>
#include <radray/runtime/material_resource.h>

namespace radray::examples::mesh_drawing {

// ID 只在这张应用拥有的表中有意义；Processor 不按字符串查标签。
struct MaterialTagFilter {
    const MaterialTagTable* Table;
    MaterialTag Required;
    std::string_view Description;
};

// 同一材质资源仍需结合顶点输入、Pass 附件和深度状态生成不同 PSO。
struct MaterialPipelines {
    const MaterialResource* Resource;
    Nullable<unique_ptr<render::GraphicsPipelineState>> Depth{nullptr};
    Nullable<unique_ptr<render::GraphicsPipelineState>> Color{nullptr};
    Nullable<unique_ptr<render::GraphicsPipelineState>> Outline{nullptr};
};

// 资源按依赖顺序声明，析构时逆序释放；仅供本教学样例使用。
struct GpuResources {
    GpuResources() noexcept = default;
    ~GpuResources() noexcept;
    GpuResources(const GpuResources&) = delete;
    GpuResources& operator=(const GpuResources&) = delete;

    bool Initialize(render::RenderBackend backend, const std::filesystem::path& shaderDirectory);
    bool UploadGeometry(std::span<const byte> vertices, std::span<const uint32_t> indices);
    bool CreatePipelines(const VertexInput& input, const MaterialResource& resource);
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
    MaterialTagTable MaterialTags;
    MaterialTag OpaqueTag;
    MaterialTag TransparentTag;
    MaterialTag OutlineTag;
    MaterialResource OpaqueResource;
    MaterialResource TwoSidedResource;
    MaterialResource TransparentResource;
    vector<MaterialPipelines> Pipelines;
    Nullable<unique_ptr<render::CommandAllocator>> Allocator{nullptr};
    Nullable<unique_ptr<render::CommandBuffer>> Command{nullptr};
};

bool PrepareOutputDirectory(const std::filesystem::path& output);
void TransitionTexture(render::CommandBuffer& command, render::Texture& texture,
                       render::TextureStates before, render::TextureStates after);

}  // namespace radray::examples::mesh_drawing
