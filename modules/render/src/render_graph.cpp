#include <radray/render/render_graph.h>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <memory>

#include <radray/logger.h>

namespace radray::render::rg {

// ============================================================
// Internal resource type tag
// ============================================================

enum class RGResourceType : uint8_t {
    Texture,
    Buffer
};

// ============================================================
// Per-resource usage record within a pass
// ============================================================

struct RGPassResourceUsage {
    uint32_t ResourceIndex;
    RGResourceType ResType;
    RGAccessFlags Access;
    RGResourceUsage Usage;
};

// ============================================================
// Color/DepthStencil attachment declaration (from PassBuilder)
// ============================================================

struct RGColorAttachmentDecl {
    uint32_t Slot;
    uint32_t TextureResourceIndex;
    LoadAction Load;
    StoreAction Store;
    ColorClearValue ClearValue;
};

struct RGDepthStencilAttachmentDecl {
    uint32_t TextureResourceIndex;
    LoadAction DepthLoad;
    StoreAction DepthStore;
    LoadAction StencilLoad;
    StoreAction StencilStore;
    DepthStencilClearValue ClearValue;
};

// ============================================================
// RGPassData — full data for a single pass
// ============================================================

struct RGPassData {
    std::string_view Name;
    RGPassType Type;
    RGPassFlags Flags{RGPassFlags::None};

    // Resources used by this pass
    radray::vector<RGPassResourceUsage> ResourceUsages;

    // Raster pass attachment declarations
    radray::vector<RGColorAttachmentDecl> ColorAttachments;
    std::optional<RGDepthStencilAttachmentDecl> DepthStencilAttachment;

    // User data + callbacks
    radray::vector<std::byte> PassDataStorage;  // aligned storage for PassData
    size_t PassDataOffset{0};                   // offset to aligned data within storage
    std::function<void(RGPassBuilder&, void*)> SetupFn;
    std::function<void(const void*, RGPassContext&)> ExecFn;
};

// ============================================================
// RGResourceEntry — per-resource metadata
// ============================================================

struct RGResourceEntry {
    RGResourceType Type;
    bool IsImported{false};
    uint32_t CurrentVersion{0};

    // For transient texture
    std::optional<RGTextureDescriptor> TexDesc;
    // For transient buffer
    std::optional<RGBufferDescriptor> BufDesc;

    // For imported resources
    Texture* ImportedTexture{nullptr};
    Buffer* ImportedBuffer{nullptr};
    SwapChain* ImportedSwapChain{nullptr};

    // Initial state (imported resources)
    TextureStates InitialTextureState{TextureState::UNKNOWN};
    BufferStates InitialBufferState{BufferState::UNKNOWN};

    // Accumulated usage flags (for transient resources)
    TextureUses AccumulatedTexUsage{TextureUse::UNKNOWN};
    BufferUses AccumulatedBufUsage{BufferUse::UNKNOWN};
};

// ============================================================
// RenderGraph::Impl
// ============================================================

struct RenderGraph::Impl {
    Device* Dev{nullptr};
    radray::vector<RGResourceEntry> Resources;
    radray::vector<RGPassData> Passes;
};

// ============================================================
// State mapping helpers
// ============================================================

static TextureStates MapUsageToTextureState(RGResourceUsage usage, [[maybe_unused]] RGAccessFlags access) {
    switch (usage) {
        case RGResourceUsage::ShaderResource:
            return TextureState::ShaderRead;
        case RGResourceUsage::UnorderedAccess:
            return TextureState::UnorderedAccess;
        case RGResourceUsage::CopySource:
            return TextureState::CopySource;
        case RGResourceUsage::CopyDestination:
            return TextureState::CopyDestination;
        case RGResourceUsage::ColorAttachment:
            return TextureState::RenderTarget;
        case RGResourceUsage::DepthStencilRead:
            return TextureState::DepthRead;
        case RGResourceUsage::DepthStencilWrite:
            return TextureState::DepthWrite;
        default:
            return TextureState::UNKNOWN;
    }
}

static BufferStates MapUsageToBufferState(RGResourceUsage usage, [[maybe_unused]] RGAccessFlags access) {
    switch (usage) {
        case RGResourceUsage::ShaderResource:
            return BufferState::ShaderRead;
        case RGResourceUsage::UnorderedAccess:
            return BufferState::UnorderedAccess;
        case RGResourceUsage::CopySource:
            return BufferState::CopySource;
        case RGResourceUsage::CopyDestination:
            return BufferState::CopyDestination;
        case RGResourceUsage::Vertex:
            return BufferState::Vertex;
        case RGResourceUsage::Index:
            return BufferState::Index;
        case RGResourceUsage::Indirect:
            return BufferState::Indirect;
        case RGResourceUsage::AccelerationStructureInput:
            return BufferState::AccelerationStructureBuildInput;
        case RGResourceUsage::AccelerationStructureRead:
            return BufferState::AccelerationStructureRead;
        default:
            return BufferState::UNKNOWN;
    }
}

static TextureUses MapUsageToTextureUse(RGResourceUsage usage) {
    switch (usage) {
        case RGResourceUsage::ShaderResource:
            return TextureUse::Resource;
        case RGResourceUsage::UnorderedAccess:
            return TextureUse::UnorderedAccess;
        case RGResourceUsage::CopySource:
            return TextureUse::CopySource;
        case RGResourceUsage::CopyDestination:
            return TextureUse::CopyDestination;
        case RGResourceUsage::ColorAttachment:
            return TextureUse::RenderTarget;
        case RGResourceUsage::DepthStencilRead:
            return TextureUse::DepthStencilRead;
        case RGResourceUsage::DepthStencilWrite:
            return TextureUse::DepthStencilWrite;
        default:
            return TextureUse::UNKNOWN;
    }
}

static BufferUses MapUsageToBufferUse(RGResourceUsage usage) {
    switch (usage) {
        case RGResourceUsage::ShaderResource:
            return BufferUse::Resource;
        case RGResourceUsage::UnorderedAccess:
            return BufferUse::UnorderedAccess;
        case RGResourceUsage::CopySource:
            return BufferUse::CopySource;
        case RGResourceUsage::CopyDestination:
            return BufferUse::CopyDestination;
        case RGResourceUsage::Vertex:
            return BufferUse::Vertex;
        case RGResourceUsage::Index:
            return BufferUse::Index;
        case RGResourceUsage::Indirect:
            return BufferUse::Indirect;
        case RGResourceUsage::AccelerationStructureInput:
            return BufferUse::AccelerationStructure;
        case RGResourceUsage::AccelerationStructureRead:
            return BufferUse::AccelerationStructure;
        default:
            return BufferUse::UNKNOWN;
    }
}

// ============================================================
// RenderGraph
// ============================================================

RenderGraph::RenderGraph(Device* device) : _impl(make_unique<Impl>()) {
    _impl->Dev = device;
}

RenderGraph::~RenderGraph() = default;

RGTextureHandle RenderGraph::CreateTexture(const RGTextureDescriptor& desc) {
    uint32_t index = static_cast<uint32_t>(_impl->Resources.size());
    auto& entry = _impl->Resources.emplace_back();
    entry.Type = RGResourceType::Texture;
    entry.IsImported = false;
    entry.CurrentVersion = 0;
    entry.TexDesc = desc;
    entry.AccumulatedTexUsage = desc.Usage;
    return RGTextureHandle{index, 0};
}

RGBufferHandle RenderGraph::CreateBuffer(const RGBufferDescriptor& desc) {
    uint32_t index = static_cast<uint32_t>(_impl->Resources.size());
    auto& entry = _impl->Resources.emplace_back();
    entry.Type = RGResourceType::Buffer;
    entry.IsImported = false;
    entry.CurrentVersion = 0;
    entry.BufDesc = desc;
    entry.AccumulatedBufUsage = desc.Usage;
    return RGBufferHandle{index, 0};
}

RGTextureHandle RenderGraph::ImportTexture(Texture* texture, TextureStates currentState) {
    uint32_t index = static_cast<uint32_t>(_impl->Resources.size());
    auto& entry = _impl->Resources.emplace_back();
    entry.Type = RGResourceType::Texture;
    entry.IsImported = true;
    entry.CurrentVersion = 0;
    entry.ImportedTexture = texture;
    entry.InitialTextureState = currentState;
    return RGTextureHandle{index, 0};
}

RGBufferHandle RenderGraph::ImportBuffer(Buffer* buffer, BufferStates currentState) {
    uint32_t index = static_cast<uint32_t>(_impl->Resources.size());
    auto& entry = _impl->Resources.emplace_back();
    entry.Type = RGResourceType::Buffer;
    entry.IsImported = true;
    entry.CurrentVersion = 0;
    entry.ImportedBuffer = buffer;
    entry.InitialBufferState = currentState;
    return RGBufferHandle{index, 0};
}

RGTextureHandle RenderGraph::ImportSwapChain(SwapChain* swapChain, TextureStates currentState) {
    uint32_t index = static_cast<uint32_t>(_impl->Resources.size());
    auto& entry = _impl->Resources.emplace_back();
    entry.Type = RGResourceType::Texture;
    entry.IsImported = true;
    entry.CurrentVersion = 0;
    entry.ImportedSwapChain = swapChain;
    entry.InitialTextureState = currentState;
    return RGTextureHandle{index, 0};
}

uint32_t RenderGraph::AddPassInternal(
    std::string_view name, RGPassType type,
    size_t passDataSize, size_t passDataAlign,
    std::function<void(RGPassBuilder&, void*)> setupFn,
    std::function<void(const void*, RGPassContext&)> execFn) {
    uint32_t passIndex = static_cast<uint32_t>(_impl->Passes.size());
    auto& pass = _impl->Passes.emplace_back();
    pass.Name = name;
    pass.Type = type;
    pass.SetupFn = std::move(setupFn);
    pass.ExecFn = std::move(execFn);

    // Allocate aligned storage for user PassData
    size_t alignedSize = (passDataSize + passDataAlign - 1) & ~(passDataAlign - 1);
    pass.PassDataStorage.resize(alignedSize + passDataAlign);
    // Find aligned pointer within storage
    void* rawPtr = pass.PassDataStorage.data();
    size_t space = pass.PassDataStorage.size();
    std::align(passDataAlign, passDataSize, rawPtr, space);
    pass.PassDataOffset = static_cast<size_t>(static_cast<std::byte*>(rawPtr) - pass.PassDataStorage.data());
    // Zero-initialize (default construct via memset)
    std::memset(rawPtr, 0, passDataSize);

    // Run setup
    RGPassBuilder builder(pass, *this);
    pass.SetupFn(builder, rawPtr);

    return passIndex;
}

void* RenderGraph::GetPassData(uint32_t passIndex) {
    auto& pass = _impl->Passes[passIndex];
    return pass.PassDataStorage.data() + pass.PassDataOffset;
}

const void* RenderGraph::GetPassData(uint32_t passIndex) const {
    auto& pass = _impl->Passes[passIndex];
    return pass.PassDataStorage.data() + pass.PassDataOffset;
}

void RenderGraph::InvokePassExec(uint32_t passIndex, RGPassContext& ctx) const {
    auto& pass = _impl->Passes[passIndex];
    pass.ExecFn(pass.PassDataStorage.data() + pass.PassDataOffset, ctx);
}

// ============================================================
// RGPassBuilder
// ============================================================

RGPassBuilder::RGPassBuilder(RGPassData& passData, RenderGraph& graph) noexcept
    : _passData(passData), _graph(graph) {}

RGTextureHandle RGPassBuilder::UseTexture(RGTextureHandle handle, RGAccessFlags access, RGResourceUsage usage) {
    assert(handle.IsValid());
    auto& resources = _graph._impl->Resources;
    assert(handle.Index < resources.size());
    auto& entry = resources[handle.Index];
    assert(entry.Type == RGResourceType::Texture);

    // Record usage
    _passData.ResourceUsages.push_back({handle.Index, RGResourceType::Texture, access, usage});

    // Accumulate usage flags for transient resources
    if (!entry.IsImported) {
        entry.AccumulatedTexUsage = entry.AccumulatedTexUsage | MapUsageToTextureUse(usage);
    }

    // Write creates a new version (SSA)
    if ((static_cast<uint8_t>(access) & static_cast<uint8_t>(RGAccessFlags::Write)) != 0) {
        entry.CurrentVersion++;
        return RGTextureHandle{handle.Index, entry.CurrentVersion};
    }
    return handle;
}

RGBufferHandle RGPassBuilder::UseBuffer(RGBufferHandle handle, RGAccessFlags access, RGResourceUsage usage) {
    assert(handle.IsValid());
    auto& resources = _graph._impl->Resources;
    assert(handle.Index < resources.size());
    auto& entry = resources[handle.Index];
    assert(entry.Type == RGResourceType::Buffer);

    _passData.ResourceUsages.push_back({handle.Index, RGResourceType::Buffer, access, usage});

    if (!entry.IsImported) {
        entry.AccumulatedBufUsage = entry.AccumulatedBufUsage | MapUsageToBufferUse(usage);
    }

    if ((static_cast<uint8_t>(access) & static_cast<uint8_t>(RGAccessFlags::Write)) != 0) {
        entry.CurrentVersion++;
        return RGBufferHandle{handle.Index, entry.CurrentVersion};
    }
    return handle;
}

void RGPassBuilder::SetColorAttachment(uint32_t index, RGTextureHandle handle,
                                       LoadAction load, StoreAction store,
                                       ColorClearValue clearValue) {
    assert(handle.IsValid());
    // Ensure the vector is large enough
    if (index >= _passData.ColorAttachments.size()) {
        _passData.ColorAttachments.resize(index + 1);
    }
    _passData.ColorAttachments[index] = {
        index,
        handle.Index,
        load,
        store,
        clearValue};
}

void RGPassBuilder::SetDepthStencilAttachment(RGTextureHandle handle,
                                              LoadAction depthLoad, StoreAction depthStore,
                                              LoadAction stencilLoad, StoreAction stencilStore,
                                              DepthStencilClearValue clearValue) {
    assert(handle.IsValid());
    _passData.DepthStencilAttachment = {
        handle.Index,
        depthLoad,
        depthStore,
        stencilLoad,
        stencilStore,
        clearValue};
}

void RGPassBuilder::SetFlags(RGPassFlags flags) {
    _passData.Flags = flags;
}

// ============================================================
// RGPassContext
// ============================================================

Texture* RGPassContext::GetTexture(RGTextureHandle handle) const {
    assert(handle.IsValid());
    assert(handle.Index < _textureCount);
    return _textures[handle.Index];
}

Buffer* RGPassContext::GetBuffer(RGBufferHandle handle) const {
    assert(handle.IsValid());
    assert(handle.Index < _bufferCount);
    return _buffers[handle.Index];
}

// ============================================================
// Compile
// ============================================================

RGCompiledGraph RenderGraph::Compile() {
    RGCompiledGraph result;
    result._graph = this;

    auto& resources = _impl->Resources;
    auto& passes = _impl->Passes;
    uint32_t passCount = static_cast<uint32_t>(passes.size());

    if (passCount == 0) {
        return result;
    }

    // --------------------------------------------------------
    // 1. Build dependency graph
    //    Track which pass last wrote each resource (by index).
    //    If pass B reads/writes a resource that was last written by pass A,
    //    then B depends on A.
    // --------------------------------------------------------

    // lastWriter[resourceIndex] = passIndex that last wrote it, or ~0u if none
    radray::vector<uint32_t> lastWriter(resources.size(), ~0u);
    // adjacency: inDegree[pass] and edges
    radray::vector<uint32_t> inDegree(passCount, 0);
    radray::vector<radray::vector<uint32_t>> successors(passCount);

    for (uint32_t pi = 0; pi < passCount; ++pi) {
        auto& pass = passes[pi];
        for (auto& ru : pass.ResourceUsages) {
            uint32_t ri = ru.ResourceIndex;
            uint32_t writer = lastWriter[ri];
            // If someone wrote this resource before, this pass depends on them
            if (writer != ~0u && writer != pi) {
                // Avoid duplicate edges
                auto& succ = successors[writer];
                if (std::find(succ.begin(), succ.end(), pi) == succ.end()) {
                    succ.push_back(pi);
                    inDegree[pi]++;
                }
            }
            // If this pass writes, update lastWriter
            if ((static_cast<uint8_t>(ru.Access) & static_cast<uint8_t>(RGAccessFlags::Write)) != 0) {
                lastWriter[ri] = pi;
            }
        }
    }

    // --------------------------------------------------------
    // 2. Topological sort (Kahn's algorithm, stable by declaration order)
    // --------------------------------------------------------

    radray::vector<uint32_t> sortedPasses;
    sortedPasses.reserve(passCount);

    // Use a simple queue that processes in index order for stability
    radray::vector<uint32_t> readyQueue;
    for (uint32_t i = 0; i < passCount; ++i) {
        if (inDegree[i] == 0) {
            readyQueue.push_back(i);
        }
    }
    // Sort to ensure stability (process lower indices first)
    std::sort(readyQueue.begin(), readyQueue.end());

    while (!readyQueue.empty()) {
        uint32_t current = readyQueue.front();
        readyQueue.erase(readyQueue.begin());
        sortedPasses.push_back(current);

        radray::vector<uint32_t> newReady;
        for (uint32_t succ : successors[current]) {
            inDegree[succ]--;
            if (inDegree[succ] == 0) {
                newReady.push_back(succ);
            }
        }
        // Insert newly ready passes in sorted order
        if (!newReady.empty()) {
            std::sort(newReady.begin(), newReady.end());
            // Merge into readyQueue maintaining sorted order
            radray::vector<uint32_t> merged;
            merged.reserve(readyQueue.size() + newReady.size());
            std::merge(readyQueue.begin(), readyQueue.end(),
                       newReady.begin(), newReady.end(),
                       std::back_inserter(merged));
            readyQueue = std::move(merged);
        }
    }

    if (sortedPasses.size() != passCount) {
        RADRAY_ERR_LOG("Render graph has cyclic dependencies");
        return result;
    }

    // --------------------------------------------------------
    // 3. Build transient resource descriptors with accumulated usage
    // --------------------------------------------------------

    for (uint32_t ri = 0; ri < static_cast<uint32_t>(resources.size()); ++ri) {
        auto& entry = resources[ri];
        if (entry.IsImported) {
            continue;
        }
        if (entry.Type == RGResourceType::Texture && entry.TexDesc.has_value()) {
            auto& rgDesc = entry.TexDesc.value();
            TextureDescriptor gpuDesc{};
            gpuDesc.Dim = rgDesc.Dim;
            gpuDesc.Width = rgDesc.Width;
            gpuDesc.Height = rgDesc.Height;
            gpuDesc.DepthOrArraySize = rgDesc.DepthOrArraySize;
            gpuDesc.MipLevels = rgDesc.MipLevels;
            gpuDesc.SampleCount = rgDesc.SampleCount;
            gpuDesc.Format = rgDesc.Format;
            gpuDesc.Memory = MemoryType::Device;
            gpuDesc.Usage = entry.AccumulatedTexUsage;
            gpuDesc.Hints = ResourceHint::None;
            gpuDesc.Name = rgDesc.Name;
            result.TransientTextures.push_back({ri, gpuDesc});
        } else if (entry.Type == RGResourceType::Buffer && entry.BufDesc.has_value()) {
            auto& rgDesc = entry.BufDesc.value();
            BufferDescriptor gpuDesc{};
            gpuDesc.Size = rgDesc.Size;
            gpuDesc.Memory = MemoryType::Device;
            gpuDesc.Usage = entry.AccumulatedBufUsage;
            gpuDesc.Hints = ResourceHint::None;
            gpuDesc.Name = rgDesc.Name;
            result.TransientBuffers.push_back({ri, gpuDesc});
        }
    }

    // --------------------------------------------------------
    // 4. Infer barriers — track per-resource state across sorted passes
    // --------------------------------------------------------

    // Current state tracking
    struct ResourceState {
        TextureStates TexState{TextureState::UNKNOWN};
        BufferStates BufState{BufferState::UNKNOWN};
    };

    radray::vector<ResourceState> currentState(resources.size());
    // Initialize states
    for (uint32_t ri = 0; ri < static_cast<uint32_t>(resources.size()); ++ri) {
        auto& entry = resources[ri];
        if (entry.IsImported) {
            currentState[ri].TexState = entry.InitialTextureState;
            currentState[ri].BufState = entry.InitialBufferState;
        } else {
            currentState[ri].TexState = TextureState::Undefined;
            currentState[ri].BufState = BufferState::Undefined;
        }
    }

    result.Passes.reserve(sortedPasses.size());

    for (uint32_t sortedIdx = 0; sortedIdx < static_cast<uint32_t>(sortedPasses.size()); ++sortedIdx) {
        uint32_t pi = sortedPasses[sortedIdx];
        auto& pass = passes[pi];

        RGCompiledPass compiledPass{};
        compiledPass.OriginalPassIndex = pi;
        compiledPass.Type = pass.Type;

        // Collect required states for this pass and generate barriers
        for (auto& ru : pass.ResourceUsages) {
            uint32_t ri = ru.ResourceIndex;
            auto& entry = resources[ri];

            if (entry.Type == RGResourceType::Texture) {
                TextureStates requiredState = MapUsageToTextureState(ru.Usage, ru.Access);
                TextureStates curState = currentState[ri].TexState;

                if (curState.value() != requiredState.value()) {
                    BarrierTextureDescriptor barrier{};
                    // For imported resources, Target is the real texture
                    // For transient resources, Target will be resolved at execute time
                    // We store nullptr for transient; executor must patch it
                    barrier.Target = entry.IsImported ? entry.ImportedTexture : nullptr;
                    barrier.Before = curState;
                    barrier.After = requiredState;
                    compiledPass.PreBarriers.push_back(barrier);
                    currentState[ri].TexState = requiredState;
                }
            } else {
                BufferStates requiredState = MapUsageToBufferState(ru.Usage, ru.Access);
                BufferStates curState = currentState[ri].BufState;

                if (curState.value() != requiredState.value()) {
                    BarrierBufferDescriptor barrier{};
                    barrier.Target = entry.IsImported ? entry.ImportedBuffer : nullptr;
                    barrier.Before = curState;
                    barrier.After = requiredState;
                    compiledPass.PreBarriers.push_back(barrier);
                    currentState[ri].BufState = requiredState;
                }
            }
        }

        // --------------------------------------------------------
        // 5. Build raster pass attachment descriptors
        // --------------------------------------------------------

        if (pass.Type == RGPassType::Raster) {
            for (auto& ca : pass.ColorAttachments) {
                RGCompiledPass::RGColorAttachmentDesc desc{};
                desc.TextureResourceIndex = ca.TextureResourceIndex;
                desc.Load = ca.Load;
                desc.Store = ca.Store;
                desc.ClearValue = ca.ClearValue;
                compiledPass.ColorAttachments.push_back(desc);
            }
            if (pass.DepthStencilAttachment.has_value()) {
                auto& dsa = pass.DepthStencilAttachment.value();
                RGCompiledPass::RGDepthStencilAttachmentDesc desc{};
                desc.TextureResourceIndex = dsa.TextureResourceIndex;
                desc.DepthLoad = dsa.DepthLoad;
                desc.DepthStore = dsa.DepthStore;
                desc.StencilLoad = dsa.StencilLoad;
                desc.StencilStore = dsa.StencilStore;
                desc.ClearValue = dsa.ClearValue;
                compiledPass.DepthStencilAttachment = desc;
            }
        }

        result.Passes.push_back(std::move(compiledPass));
    }

    return result;
}

}  // namespace radray::render::rg
