#include <radray/d3d12/shader.h>

namespace radray::d3d12 {

Shader::Shader(Device* device) noexcept : device(device) {}

}  // namespace radray::d3d12

auto fmt::formatter<radray::d3d12::ShaderVariableType>::format(radray::d3d12::ShaderVariableType const& val, format_context& ctx) const -> decltype(ctx.out()) {
    switch (val) {
        case radray::d3d12::ShaderVariableType::ConstantBuffer: return fmt::format_to(ctx.out(), "ConstantBuffer");
        case radray::d3d12::ShaderVariableType::StructuredBuffer: return fmt::format_to(ctx.out(), "StructuredBuffer");
        case radray::d3d12::ShaderVariableType::RWStructuredBuffer: return fmt::format_to(ctx.out(), "RWStructuredBuffer");
        case radray::d3d12::ShaderVariableType::SamplerHeap: return fmt::format_to(ctx.out(), "SamplerHeap");
        case radray::d3d12::ShaderVariableType::CBVBufferHeap: return fmt::format_to(ctx.out(), "CBVBufferHeap");
        case radray::d3d12::ShaderVariableType::SRVBufferHeap: return fmt::format_to(ctx.out(), "SRVBufferHeap");
        case radray::d3d12::ShaderVariableType::UAVBufferHeap: return fmt::format_to(ctx.out(), "UAVBufferHeap");
        case radray::d3d12::ShaderVariableType::SRVTextureHeap: return fmt::format_to(ctx.out(), "SRVTextureHeap");
        case radray::d3d12::ShaderVariableType::UAVTextureHeap: return fmt::format_to(ctx.out(), "UAVTextureHeap");
        default: return fmt::format_to(ctx.out(), "{}", static_cast<int>(val));
    }
}
