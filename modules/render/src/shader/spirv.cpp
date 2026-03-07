#include <radray/render/shader/spirv.h>

namespace radray::render {

// ResourceBindType SpirvResourceBinding::MapResourceBindType() const noexcept {
//     const bool isBufferImage = ImageInfo.has_value() && ImageInfo->Dim == SpirvImageDim::Buffer;
//     switch (Kind) {
//         case SpirvResourceKind::UniformBuffer:
//             return ResourceBindType::CBuffer;
//         case SpirvResourceKind::StorageBuffer:
//             return (ReadOnly && !WriteOnly) ? ResourceBindType::Buffer : ResourceBindType::RWBuffer;
//         case SpirvResourceKind::SampledImage:
//         case SpirvResourceKind::SeparateImage:
//             return isBufferImage ? ResourceBindType::TexelBuffer : ResourceBindType::Texture;
//         case SpirvResourceKind::SeparateSampler:
//             return ResourceBindType::Sampler;
//         case SpirvResourceKind::StorageImage:
//             return isBufferImage ? ResourceBindType::RWTexelBuffer : ResourceBindType::RWTexture;
//         case SpirvResourceKind::AccelerationStructure:
//             return ResourceBindType::AccelerationStructure;
//         default:
//             return ResourceBindType::UNKNOWN;
//     }
// }

}  // namespace radray::render
