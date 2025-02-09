#include "d3d12_device.h"

#include <radray/basic_math.h>
#include "d3d12_shader.h"
#include "d3d12_root_sig.h"
#include "d3d12_pso.h"
#include "d3d12_swapchain.h"
#include "d3d12_texture.h"
#include "d3d12_buffer.h"
#include "d3d12_cmd_allocator.h"
#include "d3d12_cmd_list.h"
#include "d3d12_fence.h"

namespace radray::render::d3d12 {

CmdQueueD3D12* Underlying(CommandQueue* v) noexcept { return static_cast<CmdQueueD3D12*>(v); }
RootSigD3D12* Underlying(RootSignature* v) noexcept { return static_cast<RootSigD3D12*>(v); }
Dxil* Underlying(Shader* v) noexcept { return static_cast<Dxil*>(v); }
GraphicsPsoD3D12* Underlying(GraphicsPipelineState* v) noexcept { return static_cast<GraphicsPsoD3D12*>(v); }
SwapChainD3D12* Underlying(SwapChain* v) noexcept { return static_cast<SwapChainD3D12*>(v); }
TextureD3D12* Underlying(Texture* v) noexcept { return static_cast<TextureD3D12*>(v); }
BufferD3D12* Underlying(Buffer* v) noexcept { return static_cast<BufferD3D12*>(v); }
CmdAllocatorD3D12* Underlying(CommandPool* v) noexcept { return static_cast<CmdAllocatorD3D12*>(v); }
CmdListD3D12* Underlying(CommandBuffer* v) noexcept { return static_cast<CmdListD3D12*>(v); }
FenceD3D12* Underlying(Fence* v) noexcept { return static_cast<FenceD3D12*>(v); }

static void DestroyImpl(DeviceD3D12* d) noexcept {
    for (auto&& i : d->_queues) {
        for (auto&& j : i) {
            j->Wait();
        }
    }
    for (auto&& i : d->_queues) {
        i.clear();
    }

    d->_cbvSrvUavHeap = nullptr;
    d->_rtvHeap = nullptr;
    d->_dsvHeap = nullptr;
    d->_gpuHeap = nullptr;
    d->_gpuSamplerHeap = nullptr;

    d->_mainAlloc = nullptr;

    d->_device = nullptr;
    d->_dxgiAdapter = nullptr;
    d->_dxgiFactory = nullptr;
}

DeviceD3D12::DeviceD3D12(
    ComPtr<ID3D12Device> device,
    ComPtr<IDXGIFactory4> dxgiFactory,
    ComPtr<IDXGIAdapter1> dxgiAdapter,
    ComPtr<D3D12MA::Allocator> mainAlloc) noexcept
    : _device(std::move(device)),
      _dxgiFactory(std::move(dxgiFactory)),
      _dxgiAdapter(std::move(dxgiAdapter)),
      _mainAlloc(std::move(mainAlloc)) {
    _features.Init(_device.Get());
}

DeviceD3D12::~DeviceD3D12() noexcept { DestroyImpl(this); }

void DeviceD3D12::Destroy() noexcept { DestroyImpl(this); }

Nullable<CommandQueue> DeviceD3D12::GetCommandQueue(QueueType type, uint32_t slot) noexcept {
    uint32_t index = static_cast<size_t>(type);
    RADRAY_ASSERT(index >= 0 && index < 3);
    auto& queues = _queues[index];
    if (queues.size() <= slot) {
        queues.reserve(slot + 1);
        for (size_t i = queues.size(); i <= slot; i++) {
            queues.emplace_back(radray::unique_ptr<CmdQueueD3D12>{nullptr});
        }
    }
    radray::unique_ptr<CmdQueueD3D12>& q = queues[slot];
    if (q == nullptr) {
        Nullable<radray::shared_ptr<Fence>> fence = CreateFence();
        if (!fence) {
            return nullptr;
        }
        ComPtr<ID3D12CommandQueue> queue;
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type = MapType(type);
        if (HRESULT hr = _device->CreateCommandQueue(&desc, IID_PPV_ARGS(queue.GetAddressOf()));
            SUCCEEDED(hr)) {
            radray::shared_ptr<FenceD3D12> f = std::static_pointer_cast<FenceD3D12>(fence.Ptr);
            auto ins = radray::make_unique<CmdQueueD3D12>(std::move(queue), desc.Type, std::move(f));
            radray::string debugName = radray::format("Queue {} {}", type, slot);
            SetObjectName(debugName, ins->_queue.Get());
            q = std::move(ins);
        } else {
            RADRAY_ERR_LOG("cannot create ID3D12CommandQueue, reason={} (code:{})", GetErrorName(hr), hr);
        }
    }
    return q->IsValid() ? q.get() : nullptr;
}

Nullable<radray::shared_ptr<CommandPool>> DeviceD3D12::CreateCommandPool(CommandQueue* queue) noexcept {
    auto q = Underlying(queue);
    ComPtr<ID3D12CommandAllocator> alloc;
    if (HRESULT hr = _device->CreateCommandAllocator(q->_type, IID_PPV_ARGS(alloc.GetAddressOf()));
        SUCCEEDED(hr)) {
        return radray::make_shared<CmdAllocatorD3D12>(std::move(alloc), q->_type);
    } else {
        RADRAY_ERR_LOG("cannot create ID3D12CommandAllocator, reason={} (code:{})", GetErrorName(hr), hr);
        return nullptr;
    }
}

Nullable<radray::shared_ptr<CommandBuffer>> DeviceD3D12::CreateCommandBuffer(CommandPool* pool) noexcept {
    auto p = Underlying(pool);
    ComPtr<ID3D12GraphicsCommandList> list;
    if (HRESULT hr = _device->CreateCommandList(0, p->_type, p->_cmdAlloc.Get(), nullptr, IID_PPV_ARGS(list.GetAddressOf()));
        SUCCEEDED(hr)) {
        RADRAY_DX_CHECK(list->Close());
        return radray::make_shared<CmdListD3D12>(
            std::move(list),
            p->_cmdAlloc.Get(),
            p->_type,
            GetGpuHeap(),
            GetGpuSamplerHeap());
    } else {
        RADRAY_ERR_LOG("cannot create ID3D12GraphicsCommandList, reason={} (code:{})", GetErrorName(hr), hr);
        return nullptr;
    }
}

Nullable<radray::shared_ptr<Fence>> DeviceD3D12::CreateFence() noexcept {
    ComPtr<ID3D12Fence> fence;
    if (HRESULT hr = _device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf()));
        FAILED(hr)) {
        RADRAY_ERR_LOG("cannot create ID3D12Fence, reason={} (code:{})", GetErrorName(hr), hr);
        return nullptr;
    }
    std::optional<Win32Event> e = MakeWin32Event();
    if (!e.has_value()) {
        return nullptr;
    }
    return radray::make_shared<FenceD3D12>(std::move(fence), std::move(e.value()));
}

Nullable<radray::shared_ptr<Shader>> DeviceD3D12::CreateShader(
    std::span<const byte> blob,
    const ShaderReflection& refl,
    ShaderStage stage,
    std::string_view entryPoint,
    std::string_view name) noexcept {
    auto dxilRefl = std::get_if<DxilReflection>(&refl);
    if (dxilRefl == nullptr) {
        RADRAY_ERR_LOG("d3d12 can only use dxil shader");
        return nullptr;
    }
    return radray::make_shared<Dxil>(blob, *dxilRefl, entryPoint, name, stage);
}

Nullable<radray::shared_ptr<RootSignature>> DeviceD3D12::CreateRootSignature(std::span<Shader*> shaders) noexcept {
    class StageResource : public DxilReflection::BindResource {
    public:
        ShaderStages Stages;
    };
    radray::unordered_map<radray::string, DxilReflection::CBuffer> cbufferMap{};
    radray::unordered_map<radray::string, DxilReflection::StaticSampler> staticSamplerMap{};
    for (Shader* i : shaders) {
        Dxil* dxil = Underlying(i);
        for (const DxilReflection::CBuffer& j : dxil->_refl.CBuffers) {
            auto [iter, isInsert] = cbufferMap.emplace(j.Name, DxilReflection::CBuffer{});
            if (isInsert) {
                iter->second = j;
            } else {
                if (iter->second != j) {
                    if (j.Size > iter->second.Size) {
                        iter->second = j;
                        RADRAY_DEBUG_LOG("cbuffer has different layout but same name {}. maybe reinterpret?", j.Name);
                    }
                }
            }
        }
        for (const DxilReflection::StaticSampler& j : dxil->_refl.StaticSamplers) {
            auto [iter, isInsert] = staticSamplerMap.emplace(j.Name, DxilReflection::StaticSampler{});
            if (isInsert) {
                iter->second = j;
            } else {
                if (iter->second != j) {
                    RADRAY_ERR_LOG("static sampler has different layout but same name {}", j.Name);
                    return nullptr;
                }
            }
        }
    }
    // 收集所有 bind resource
    radray::vector<StageResource> resources;
    radray::vector<StageResource> samplers;
    radray::vector<StageResource> staticSamplers;
    ShaderStages shaderStages{ShaderStage::UNKNOWN};
    for (Shader* i : shaders) {
        Dxil* dxil = Underlying(i);
        shaderStages |= dxil->Stage;
        const auto& refl = dxil->_refl;
        for (const DxilReflection::BindResource& j : refl.Binds) {
            StageResource res{j, ShaderStages{dxil->Stage}};
            if (j.Type == ShaderResourceType::Sampler) {
                const auto& stat = refl.StaticSamplers;
                auto iter = std::find_if(stat.begin(), stat.end(), [&](auto&& v) noexcept { return j.Name == v.Name; });
                if (iter == stat.end()) {
                    samplers.emplace_back(std::move(res));
                } else {
                    staticSamplers.emplace_back(std::move(res));
                }
            } else {
                resources.emplace_back(std::move(res));
            }
        }
    }
    // 合并不同 stage 所需相同资源, 也就是 Space 和 Bind 一致的资源. 把 cbuffer 放前面, 其他类型资源放后面, 再按 Space, BindPoint 排序
    auto check = [](const radray::vector<StageResource>& res) noexcept {
        radray::vector<StageResource> temp;
        for (const StageResource& i : res) {
            auto iter = std::find_if(temp.begin(), temp.end(), [&](auto&& v) noexcept {
                return v.Space == i.Space && v.BindPoint == i.BindPoint && v.Type == i.Type;
            });
            if (iter == temp.end()) {
                temp.emplace_back(i);
            } else {
                if (iter->Name != i.Name) {
                    RADRAY_ERR_LOG("resource {} and {} has different name but same space and bind point", iter->Name, i.Name);
                    return false;
                }
            }
        }
        return true;
    };
    auto merge = [](const radray::vector<StageResource>& res) noexcept {
        radray::vector<StageResource> result;
        for (const StageResource& i : res) {
            auto iter = std::find_if(result.begin(), result.end(), [&](auto&& v) noexcept {
                return v.Space == i.Space && v.BindPoint == i.BindPoint && v.Type == i.Type;
            });
            if (iter == result.end()) {
                result.emplace_back(i);
            } else {
                iter->Stages |= i.Stages;
            }
        }
        return result;
    };
    auto&& resCmp = [](const auto& lhs, const auto& rhs) noexcept {
        if (lhs.Type == ShaderResourceType::CBuffer && rhs.Type != ShaderResourceType::CBuffer) {
            return true;
        }
        if (lhs.Type != ShaderResourceType::CBuffer && rhs.Type == ShaderResourceType::CBuffer) {
            return false;
        }
        if (lhs.Space == rhs.Space) {
            return lhs.BindPoint < rhs.BindPoint;
        } else {
            return lhs.Space < rhs.Space;
        }
    };
    radray::vector<StageResource> mergedCbuffers, mergedResources;
    radray::unordered_set<uint32_t> cbufferSpaces, resourceSpaces;
    {
        if (!check(resources)) {
            return nullptr;
        }
        radray::vector<StageResource> mergeBinds = merge(resources);
        std::sort(mergeBinds.begin(), mergeBinds.end(), resCmp);
        for (StageResource& i : mergeBinds) {
            if (i.Type == ShaderResourceType::CBuffer) {
                cbufferSpaces.emplace(i.Space);
                mergedCbuffers.emplace_back(std::move(i));
            } else {
                resourceSpaces.emplace(i.Space);
                mergedResources.emplace_back(std::move(i));
            }
        }
    }
    for (const StageResource& i : mergedCbuffers) {  // 检查下 cbuffer 反射数据
        auto iter = cbufferMap.find(i.Name);
        if (iter == cbufferMap.end()) {
            RADRAY_ERR_LOG("cannot find cbuffer {}", i.Name);
            return nullptr;
        }
    }
    if (!check(samplers)) {
        return nullptr;
    }
    if (!check(staticSamplers)) {
        return nullptr;
    }
    radray::vector<StageResource> mergeSamplers = merge(samplers), mergeStaticSamplers = merge(staticSamplers);
    radray::unordered_set<uint32_t> samplersSpaces;
    std::sort(mergeSamplers.begin(), mergeSamplers.end(), resCmp);
    std::sort(mergeStaticSamplers.begin(), mergeStaticSamplers.end(), resCmp);
    for (const StageResource& i : mergeSamplers) {
        samplersSpaces.emplace(i.Space);
    }
    for (const StageResource& i : mergeStaticSamplers) {  // 检查下 static sampler 反射数据
        auto iter = staticSamplerMap.find(i.Name);
        if (iter == staticSamplerMap.end()) {
            RADRAY_ERR_LOG("cannot find static sampler {}", i.Name);
            return nullptr;
        }
    }
    // https://learn.microsoft.com/en-us/windows/win32/direct3d12/root-signature-limits
    // DWORD = 4 bytes = 32 bits
    // root sig 最大可存 64 DWORD
    // - 1 Descriptor Table 消耗 1 DWORD
    // - 1 Root Constant 消耗 1 DWORD
    // - 1 Root Descriptor 消耗 2 DWORD
    enum class RootSigStrategy {
        CBufferRootConst,
        CBufferRootDesc,
        DescTable
    };
    uint64_t useRC = 0, useRD = 0, useDT = 0;
    {
        // 找 push constant, 找不到就尝试第一个 cbuffer 用 root constant, 其他 cbuffer 用 root descriptor
        auto pcIter = std::find_if(
            mergedCbuffers.begin(), mergedCbuffers.end(),
            [](auto&& v) noexcept { return v.Type == ShaderResourceType::PushConstant; });
        if (pcIter == mergedCbuffers.end()) {
            pcIter = mergedCbuffers.begin();
        }
        for (auto i = mergedCbuffers.begin(); i != mergedCbuffers.end(); i++) {
            if (i == pcIter) {
                const DxilReflection::CBuffer& cbuffer = cbufferMap.find(i->Name)->second;
                useRC += CalcAlign(cbuffer.Size, 4);
            } else {
                useRD += 4 * 2;
            }
        }
        useRC += resourceSpaces.size() * 4;
        useRC += samplersSpaces.size() * 4;
    }
    RADRAY_DEBUG_LOG("all cbuffers use root constant. root sig size: {} DWORDs", useRC / 4);
    {
        // 尝试将 cbuffer 全用 root descriptor 储存
        useRD += mergedCbuffers.size() * 4 * 2;
        useRD += resourceSpaces.size() * 4;
        useRD += samplersSpaces.size() * 4;
    }
    RADRAY_DEBUG_LOG("all cbuffers use root descriptor. root sig size: {} DWORDs", useRD / 4);
    {
        // 按 space 划分 descriptor table
        radray::unordered_set<uint32_t> resSpaces;
        std::merge(
            cbufferSpaces.begin(), cbufferSpaces.end(),
            resourceSpaces.begin(), resourceSpaces.end(),
            std::inserter(resSpaces, resSpaces.begin()));
        useDT += resSpaces.size() * 4;
        useDT += samplersSpaces.size() * 4;
    }
    RADRAY_DEBUG_LOG("all use descriptor table. split by space. root sig size: {} DWORDs", useDT / 4);
    RootSigStrategy strategy = RootSigStrategy::CBufferRootConst;
    if (useRC > 256) {
        strategy = RootSigStrategy::CBufferRootDesc;
        if (useRD > 256) {
            strategy = RootSigStrategy::DescTable;
        }
    }
    radray::vector<D3D12_ROOT_PARAMETER1> rootParmas{};
    radray::vector<radray::vector<D3D12_DESCRIPTOR_RANGE1>> descRanges;
    auto&& setupTableRes = [&rootParmas, &descRanges](
                               const radray::unordered_set<uint32_t>& spaces,
                               const radray::vector<StageResource>& res,
                               radray::vector<DescTable>& tbls) noexcept {
        for (uint32_t space : spaces) {
            auto& ranges = descRanges.emplace_back(radray::vector<D3D12_DESCRIPTOR_RANGE1>{});
            ShaderStages tableStages{};
            DescTable tbl{};
            for (const StageResource& r : res) {
                if (r.Space == space) {
                    auto&& range = ranges.emplace_back(D3D12_DESCRIPTOR_RANGE1{});
                    CD3DX12_DESCRIPTOR_RANGE1::Init(
                        range,
                        MapDescRangeType(r.Type),
                        r.BindCount,
                        r.BindPoint,
                        r.Space,
                        r.Type == ShaderResourceType::Sampler ? D3D12_DESCRIPTOR_RANGE_FLAG_NONE : D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC);
                    tableStages |= r.Stages;
                    tbl._elems.emplace_back(DescElem{r.Name, r.Type, r.BindPoint, r.Space, r.BindCount});
                }
            }
            auto& p = rootParmas.emplace_back(D3D12_ROOT_PARAMETER1{});
            CD3DX12_ROOT_PARAMETER1::InitAsDescriptorTable(
                p,
                (UINT)ranges.size(),
                ranges.data(),
                MapShaderStages(tableStages));
            tbls.emplace_back(std::move(tbl));
        }
    };
    radray::vector<RootConst> rootConsts;
    radray::vector<CBufferView> cbufferViews;
    radray::vector<DescTable> resDescTables;
    radray::vector<DescTable> samplerDescTables;
    if (strategy == RootSigStrategy::CBufferRootConst || strategy == RootSigStrategy::CBufferRootDesc) {
        auto pcIter = std::find_if(
            mergedCbuffers.begin(), mergedCbuffers.end(),
            [](auto&& v) noexcept { return v.Type == ShaderResourceType::PushConstant; });
        if (pcIter == mergedCbuffers.end()) {
            pcIter = mergedCbuffers.begin();
        }
        for (auto i = mergedCbuffers.begin(); i != mergedCbuffers.end(); i++) {
            const DxilReflection::CBuffer& cbuffer = cbufferMap.find(i->Name)->second;
            auto&& p = rootParmas.emplace_back(D3D12_ROOT_PARAMETER1{});
            if (strategy == RootSigStrategy::CBufferRootConst && i == pcIter) {
                CD3DX12_ROOT_PARAMETER1::InitAsConstants(
                    p,
                    (UINT)(CalcAlign(cbuffer.Size, 4) / 4),
                    i->BindPoint,
                    i->Space,
                    MapShaderStages(i->Stages));
                rootConsts.emplace_back(RootConst{i->Name, i->BindPoint, i->Space});
            } else {
                CD3DX12_ROOT_PARAMETER1::InitAsConstantBufferView(
                    p,
                    i->BindPoint,
                    i->Space,
                    D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC,
                    MapShaderStages(i->Stages));
                cbufferViews.emplace_back(CBufferView{i->Name, i->BindPoint, i->Space});
            }
        }
        setupTableRes(resourceSpaces, mergedResources, resDescTables);
        setupTableRes(samplersSpaces, mergeSamplers, samplerDescTables);
    } else {
        setupTableRes(resourceSpaces, mergedResources, resDescTables);
        setupTableRes(samplersSpaces, mergeSamplers, samplerDescTables);
    }
    radray::vector<D3D12_STATIC_SAMPLER_DESC> staticSamplerDescs;
    staticSamplerDescs.reserve(mergeStaticSamplers.size());
    for (const StageResource& i : mergeStaticSamplers) {
        const DxilReflection::StaticSampler& rs = staticSamplerMap.find(i.Name)->second;
        auto& ssd = staticSamplerDescs.emplace_back(D3D12_STATIC_SAMPLER_DESC{});
        ssd.Filter = MapType(rs.MigFilter, rs.MagFilter, rs.MipmapFilter, rs.HasCompare, rs.AnisotropyClamp);
        ssd.AddressU = MapType(rs.AddressS);
        ssd.AddressV = MapType(rs.AddressT);
        ssd.AddressW = MapType(rs.AddressR);
        ssd.MipLODBias = 0;
        ssd.MaxAnisotropy = rs.AnisotropyClamp;
        ssd.ComparisonFunc = rs.HasCompare ? MapType(rs.Compare) : D3D12_COMPARISON_FUNC_ALWAYS;
        ssd.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
        ssd.MinLOD = rs.LodMin;
        ssd.MaxLOD = rs.LodMax;
        ssd.ShaderRegister = i.BindPoint;
        ssd.RegisterSpace = i.Space;
        ssd.ShaderVisibility = MapShaderStages(i.Stages);
    }
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC versionDesc{};
    versionDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    D3D12_ROOT_SIGNATURE_DESC1& rcDesc = versionDesc.Desc_1_1;
    rcDesc.NumParameters = (UINT)rootParmas.size();
    rcDesc.pParameters = rootParmas.data();
    rcDesc.NumStaticSamplers = (UINT)staticSamplerDescs.size();
    rcDesc.pStaticSamplers = staticSamplerDescs.data();
    {
        D3D12_ROOT_SIGNATURE_FLAGS flag =
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_AMPLIFICATION_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_MESH_SHADER_ROOT_ACCESS;
        if (!HasFlag(shaderStages, ShaderStage::Vertex)) {
            flag |= D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS;
        }
        if (!HasFlag(shaderStages, ShaderStage::Pixel)) {
            flag |= D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS;
        }
        rcDesc.Flags = flag;
    }
    ComPtr<ID3DBlob> rootSigBlob, errorBlob;
    if (HRESULT hr = D3DX12SerializeVersionedRootSignature(
            &versionDesc,
            D3D_ROOT_SIGNATURE_VERSION_1_1,
            rootSigBlob.GetAddressOf(),
            errorBlob.GetAddressOf());
        FAILED(hr)) {
        const char* errInfoBegin = errorBlob ? reinterpret_cast<const char*>(errorBlob->GetBufferPointer()) : nullptr;
        const char* errInfoEnd = errInfoBegin + (errorBlob ? errorBlob->GetBufferSize() : 0);
        auto reason = errInfoBegin == nullptr ? GetErrorName(hr) : std::string_view{errInfoBegin, errInfoEnd};
        RADRAY_ERR_LOG("d3d12 cannot serialize root sig\n{}", reason);
        return nullptr;
    }
    ComPtr<ID3D12RootSignature> rootSig;
    if (HRESULT hr = _device->CreateRootSignature(
            0,
            rootSigBlob->GetBufferPointer(),
            rootSigBlob->GetBufferSize(),
            IID_PPV_ARGS(rootSig.GetAddressOf()));
        FAILED(hr)) {
        RADRAY_ERR_LOG("d3d12 cannot create root sig. reason={}, (code:{})", GetErrorName(hr), hr);
        return nullptr;
    }
    auto result = radray::make_shared<RootSigD3D12>(std::move(rootSig));
    result->_rootConsts = std::move(rootConsts);
    result->_cbufferViews = std::move(cbufferViews);
    result->_resDescTables = std::move(resDescTables);
    result->_samplerDescTables = std::move(samplerDescTables);
    return result;
}

Nullable<radray::shared_ptr<GraphicsPipelineState>> DeviceD3D12::CreateGraphicsPipeline(
    const GraphicsPipelineStateDescriptor& desc) noexcept {
    auto [topoClass, topo] = MapType(desc.Primitive.Topology);
    radray::vector<D3D12_INPUT_ELEMENT_DESC> inputElements;
    radray::vector<uint64_t> arrayStrides(desc.VertexBuffers.size(), 0);
    for (size_t index = 0; index < desc.VertexBuffers.size(); index++) {
        const VertexBufferLayout& i = desc.VertexBuffers[index];
        arrayStrides[index] = i.ArrayStride;
        D3D12_INPUT_CLASSIFICATION inputClass = MapType(i.StepMode);
        for (const VertexElement& j : i.Elements) {
            auto& ied = inputElements.emplace_back(D3D12_INPUT_ELEMENT_DESC{});
            ied.SemanticName = format_as(j.Semantic).data();
            ied.SemanticIndex = j.SemanticIndex;
            ied.Format = MapType(j.Format);
            ied.InputSlot = (UINT)index;
            ied.AlignedByteOffset = (UINT)j.Offset;
            ied.InputSlotClass = inputClass;
            ied.InstanceDataStepRate = inputClass == D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA ? 1 : 0;
        }
    }
    DepthBiasState depBias = desc.DepthStencilEnable ? desc.DepthStencil.DepthBias : DepthBiasState{0, 0, 0};
    D3D12_RASTERIZER_DESC rawRaster{};
    if (auto fillMode = MapType(desc.Primitive.Poly);
        fillMode.has_value()) {
        rawRaster.FillMode = fillMode.value();
    } else {
        RADRAY_ERR_LOG("d3d12 cannot set fill mode {}", desc.Primitive.Poly);
        return nullptr;
    }
    rawRaster.CullMode = MapType(desc.Primitive.Cull);
    rawRaster.FrontCounterClockwise = desc.Primitive.FaceClockwise == FrontFace::CCW;
    rawRaster.DepthBias = depBias.Constant;
    rawRaster.DepthBiasClamp = depBias.Clamp;
    rawRaster.SlopeScaledDepthBias = depBias.SlopScale;
    rawRaster.DepthClipEnable = !desc.Primitive.UnclippedDepth;
    rawRaster.MultisampleEnable = desc.MultiSample.Count > 1;
    rawRaster.AntialiasedLineEnable = false;
    rawRaster.ForcedSampleCount = 0;
    rawRaster.ConservativeRaster = desc.Primitive.Conservative ? D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON : D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    D3D12_BLEND_DESC rawBlend{};
    rawBlend.AlphaToCoverageEnable = desc.MultiSample.AlphaToCoverageEnable;
    rawBlend.IndependentBlendEnable = true;
    {
        for (size_t i = 0; i < ArrayLength(rawBlend.RenderTarget); i++) {
            D3D12_RENDER_TARGET_BLEND_DESC& rtb = rawBlend.RenderTarget[i];
            if (i < desc.ColorTargets.size()) {
                const ColorTargetState& ct = desc.ColorTargets[i];
                rtb.BlendEnable = ct.BlendEnable;
                if (rtb.BlendEnable) {
                    rtb.SrcBlend = MapBlendColor(ct.Blend.Color.Src);
                    rtb.DestBlend = MapBlendColor(ct.Blend.Color.Dst);
                    rtb.BlendOp = MapType(ct.Blend.Color.Op);
                    rtb.SrcBlendAlpha = MapBlendAlpha(ct.Blend.Alpha.Src);
                    rtb.DestBlendAlpha = MapBlendAlpha(ct.Blend.Alpha.Dst);
                    rtb.BlendOpAlpha = MapType(ct.Blend.Alpha.Op);
                }
                if (auto writeMask = MapColorWrites(ct.WriteMask);
                    writeMask.has_value()) {
                    rtb.RenderTargetWriteMask = (UINT8)writeMask.value();
                } else {
                    RADRAY_ERR_LOG("d3d12 cannot set color write mask {}", ct.WriteMask);
                    return nullptr;
                }
            } else {
                rtb.BlendEnable = false;
                rtb.LogicOpEnable = false;
                rtb.LogicOp = D3D12_LOGIC_OP_CLEAR;
                rtb.RenderTargetWriteMask = 0;
            }
        }
    }
    D3D12_DEPTH_STENCIL_DESC dsDesc{};
    if (desc.DepthStencilEnable) {
        dsDesc.DepthEnable = true;
        dsDesc.DepthWriteMask = desc.DepthStencil.DepthWriteEnable ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        dsDesc.DepthFunc = MapType(desc.DepthStencil.DepthCompare);
        dsDesc.StencilEnable = desc.DepthStencil.StencilEnable;
        if (dsDesc.StencilEnable) {
            auto ToDsd = [](StencilFaceState v) noexcept {
                D3D12_DEPTH_STENCILOP_DESC result{};
                result.StencilFailOp = MapType(v.FailOp);
                result.StencilDepthFailOp = MapType(v.DepthFailOp);
                result.StencilPassOp = MapType(v.PassOp);
                result.StencilFunc = MapType(v.Compare);
                return result;
            };
            dsDesc.StencilReadMask = (UINT8)desc.DepthStencil.Stencil.ReadMask;
            dsDesc.StencilWriteMask = (UINT8)desc.DepthStencil.Stencil.WriteMask;
            dsDesc.FrontFace = ToDsd(desc.DepthStencil.Stencil.Front);
            dsDesc.BackFace = ToDsd(desc.DepthStencil.Stencil.Back);
        }
    } else {
        dsDesc.DepthEnable = false;
        dsDesc.StencilEnable = false;
    }
    DXGI_SAMPLE_DESC sampleDesc{desc.MultiSample.Count, 0};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC rawPsoDesc{};
    rawPsoDesc.pRootSignature = Underlying(desc.RootSig)->_rootSig.Get();
    rawPsoDesc.VS = desc.VS ? Underlying(desc.VS)->ToByteCode() : D3D12_SHADER_BYTECODE{};
    rawPsoDesc.PS = desc.PS ? Underlying(desc.PS)->ToByteCode() : D3D12_SHADER_BYTECODE{};
    rawPsoDesc.DS = D3D12_SHADER_BYTECODE{};
    rawPsoDesc.HS = D3D12_SHADER_BYTECODE{};
    rawPsoDesc.GS = D3D12_SHADER_BYTECODE{};
    rawPsoDesc.StreamOutput = D3D12_STREAM_OUTPUT_DESC{};
    rawPsoDesc.BlendState = rawBlend;
    rawPsoDesc.SampleMask = (UINT)desc.MultiSample.Mask;
    rawPsoDesc.RasterizerState = rawRaster;
    rawPsoDesc.DepthStencilState = dsDesc;
    rawPsoDesc.InputLayout = {inputElements.data(), static_cast<uint32_t>(inputElements.size())};
    rawPsoDesc.IBStripCutValue = MapType(desc.Primitive.StripIndexFormat);
    rawPsoDesc.PrimitiveTopologyType = topoClass;
    rawPsoDesc.NumRenderTargets = std::min(static_cast<uint32_t>(desc.ColorTargets.size()), (uint32_t)D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT);
    for (size_t i = 0; i < rawPsoDesc.NumRenderTargets; i++) {
        rawPsoDesc.RTVFormats[i] = i < desc.ColorTargets.size() ? MapType(desc.ColorTargets[i].Format) : DXGI_FORMAT_UNKNOWN;
    }
    rawPsoDesc.DSVFormat = desc.DepthStencilEnable ? MapType(desc.DepthStencil.Format) : DXGI_FORMAT_UNKNOWN;
    rawPsoDesc.SampleDesc = sampleDesc;
    rawPsoDesc.NodeMask = 0;
    rawPsoDesc.CachedPSO = D3D12_CACHED_PIPELINE_STATE{};
    rawPsoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    ComPtr<ID3D12PipelineState> pso;
    if (HRESULT hr = _device->CreateGraphicsPipelineState(&rawPsoDesc, IID_PPV_ARGS(pso.GetAddressOf()));
        FAILED(hr)) {
        RADRAY_ERR_LOG("d3d12 cannot create graphics pipeline state. reason={} (code:{})", GetErrorName(hr), hr);
        return nullptr;
    }
    return radray::make_shared<GraphicsPsoD3D12>(std::move(pso), std::move(arrayStrides), topo);
}

Nullable<radray::shared_ptr<SwapChain>> DeviceD3D12::CreateSwapChain(
    CommandQueue* presentQueue,
    const void* nativeWindow,
    uint32_t width,
    uint32_t height,
    uint32_t backBufferCount,
    TextureFormat format,
    bool enableSync) noexcept {
    // https://learn.microsoft.com/zh-cn/windows/win32/api/dxgi1_2/ns-dxgi1_2-dxgi_swap_chain_desc1
    DXGI_SWAP_CHAIN_DESC1 scDesc{};
    scDesc.Width = width;
    scDesc.Height = height;
    scDesc.Format = MapType(format);
    if (scDesc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT &&
        scDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
        scDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
        scDesc.Format != DXGI_FORMAT_R10G10B10A2_UNORM) {
        RADRAY_ERR_LOG("d3d12 IDXGISwapChain doesn't support format {}", format);
        return nullptr;
    }
    scDesc.Stereo = false;
    scDesc.SampleDesc.Count = 1;
    scDesc.SampleDesc.Quality = 0;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.BufferCount = backBufferCount;
    if (scDesc.BufferCount < 2 || scDesc.BufferCount > 16) {
        RADRAY_ERR_LOG("d3d12 IDXGISwapChain buffer count must >= 2 and <= 16, cannot be {}", backBufferCount);
        return nullptr;
    }
    scDesc.Scaling = DXGI_SCALING_STRETCH;
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    scDesc.Flags = 0;
    scDesc.Flags |= _isAllowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    CmdQueueD3D12* queue = Underlying(presentQueue);
    HWND hwnd = reinterpret_cast<HWND>(const_cast<void*>(nativeWindow));
    ComPtr<IDXGISwapChain1> temp;
    if (HRESULT hr = _dxgiFactory->CreateSwapChainForHwnd(queue->_queue.Get(), hwnd, &scDesc, nullptr, nullptr, temp.GetAddressOf());
        FAILED(hr)) {
        RADRAY_ERR_LOG("d3d12 cannot create IDXGISwapChain1 for HWND, reason={} (code:{})", GetErrorName(hr), hr);
        return nullptr;
    }
    if (HRESULT hr = _dxgiFactory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);  // 阻止 Alt + Enter 进全屏
        FAILED(hr)) {
        RADRAY_WARN_LOG("d3d12 cannot make window association no alt enter, reason={} (code:{})", GetErrorName(hr), hr);
        return nullptr;
    }
    ComPtr<IDXGISwapChain3> swapchain;
    if (HRESULT hr = temp->QueryInterface(IID_PPV_ARGS(swapchain.GetAddressOf()));
        FAILED(hr)) {
        RADRAY_ERR_LOG("d3d12 doesn't support IDXGISwapChain3, reason={} (code:{})", GetErrorName(hr), hr);
        return nullptr;
    }
    radray::vector<radray::shared_ptr<TextureD3D12>> colors;
    colors.reserve(scDesc.BufferCount);
    for (size_t i = 0; i < scDesc.BufferCount; i++) {
        ComPtr<ID3D12Resource> color;
        if (HRESULT hr = swapchain->GetBuffer((UINT)i, IID_PPV_ARGS(color.GetAddressOf()));
            FAILED(hr)) {
            RADRAY_ERR_LOG("d3d12 cannot get back buffer in IDXGISwapChain1, reason={} (code:{})", GetErrorName(hr), hr);
            return nullptr;
        }
        auto tex = radray::make_shared<TextureD3D12>(
            std::move(color),
            ComPtr<D3D12MA::Allocation>{},
            D3D12_RESOURCE_STATE_PRESENT,
            ResourceType::RenderTarget);
        colors.emplace_back(std::move(tex));
    }
    UINT presentFlags = (!enableSync && _isAllowTearing) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    return radray::make_shared<SwapChainD3D12>(swapchain, std::move(colors), presentFlags);
}

Nullable<radray::shared_ptr<Buffer>> DeviceD3D12::CreateBuffer(
    uint64_t size,
    ResourceType type,
    ResourceUsage usage,
    ResourceStates initState,
    ResourceMemoryTips tips,
    std::string_view name) noexcept {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    // Alignment must be 64KB (D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT) or 0, which is effectively 64KB.
    // https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_resource_desc
    desc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    // D3D12 要求 cbuffer 是 256 字节对齐
    // https://github.com/d3dcoder/d3d12book/blob/master/Common/d3dUtil.h#L99
    desc.Width = type == ResourceType::CBuffer ? CalcAlign(size, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT) : size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    if (type == ResourceType::BufferRW) {
        desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }
    if (usage == ResourceUsage::Readback) {
        desc.Flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    }
    if (usage == ResourceUsage::Upload) {
        initState = (ResourceStates)ResourceState::GenericRead;
    } else if (usage == ResourceUsage::Readback) {
        initState = (ResourceStates)ResourceState::CopyDestination;
    }
    D3D12_RESOURCE_STATES rawInitState = MapTypeResStates(initState);
    D3D12MA::ALLOCATION_DESC allocDesc{};
    allocDesc.HeapType = MapType(usage);
    allocDesc.Flags = D3D12MA::ALLOCATION_FLAG_NONE;
    if (HasFlag(tips, ResourceMemoryTip::Dedicated)) {
        allocDesc.Flags = static_cast<D3D12MA::ALLOCATION_FLAGS>(allocDesc.Flags | D3D12MA::ALLOCATION_FLAG_COMMITTED);
    }
    ComPtr<ID3D12Resource> buffer;
    ComPtr<D3D12MA::Allocation> allocRes;
    if (allocDesc.HeapType != D3D12_HEAP_TYPE_DEFAULT && (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) {
        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_CUSTOM;
        heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE;
        heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
        heapProps.VisibleNodeMask = 0;
        heapProps.CreationNodeMask = 0;
        if (rawInitState == D3D12_RESOURCE_STATE_GENERIC_READ) {
            rawInitState = D3D12_RESOURCE_STATE_COMMON;
        }
        if (HRESULT hr = _device->CreateCommittedResource(
                &heapProps,
                allocDesc.ExtraHeapFlags,
                &desc,
                rawInitState,
                nullptr,
                IID_PPV_ARGS(buffer.GetAddressOf()));
            FAILED(hr)) {
            RADRAY_ERR_LOG("d3d12 cannot create buffer, reason={} (code:{})", GetErrorName(hr), hr);
            return nullptr;
        }
    } else {
        if (HRESULT hr = _mainAlloc->CreateResource(
                &allocDesc,
                &desc,
                rawInitState,
                nullptr,
                allocRes.GetAddressOf(),
                IID_PPV_ARGS(buffer.GetAddressOf()));
            FAILED(hr)) {
            RADRAY_ERR_LOG("d3d12 cannot create buffer, reason={} (code:{})", GetErrorName(hr), hr);
            return nullptr;
        }
    }
    SetObjectName(name, buffer.Get(), allocRes.Get());
    RADRAY_DEBUG_LOG("d3d12 create buffer, size={}, type={}, usage={}", size, type, usage);
    return radray::make_shared<BufferD3D12>(std::move(buffer), std::move(allocRes), rawInitState, type);
}

Nullable<radray::shared_ptr<Texture>> DeviceD3D12::CreateTexture(
    uint64_t width,
    uint64_t height,
    uint64_t depth,
    uint32_t arraySize,
    TextureFormat format,
    uint32_t mipLevels,
    uint32_t sampleCount,
    uint32_t sampleQuality,
    ClearValue clearValue,
    ResourceType type,
    ResourceStates initState,
    ResourceMemoryTips tips,
    std::string_view name) noexcept {
    DXGI_FORMAT rawFormat = MapType(format);
    D3D12_RESOURCE_DESC desc{};
    if (depth > 1) {
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    } else if (depth > 1) {
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    } else {
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D;
    }
    desc.Alignment = sampleCount > 1 ? D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT : 0;
    desc.Width = width;
    if (height > std::numeric_limits<decltype(desc.Height)>::max()) {
        RADRAY_ERR_LOG("d3d12 cannot create texture, height {} too large", height);
        return nullptr;
    }
    desc.Height = (UINT)height;
    if (depth > std::numeric_limits<decltype(desc.DepthOrArraySize)>::max()) {
        RADRAY_ERR_LOG("d3d12 cannot create texture, depth {} too large", depth);
        return nullptr;
    }
    if (arraySize > std::numeric_limits<decltype(desc.DepthOrArraySize)>::max()) {
        RADRAY_ERR_LOG("d3d12 cannot create texture, array size {} too large", arraySize);
        return nullptr;
    }
    desc.DepthOrArraySize = (UINT16)(arraySize != 1 ? arraySize : depth);
    if (mipLevels > std::numeric_limits<decltype(desc.MipLevels)>::max()) {
        RADRAY_ERR_LOG("d3d12 cannot create texture, mip levels {} too large", mipLevels);
        return nullptr;
    }
    desc.MipLevels = (UINT16)mipLevels;
    desc.Format = FormatToTypeless(rawFormat);
    desc.SampleDesc.Count = sampleCount ? sampleCount : 1;
    desc.SampleDesc.Quality = sampleQuality;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    if (type == ResourceType::TextureRW) {
        desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }
    if (type == ResourceType::RenderTarget) {
        desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    }
    if (type == ResourceType::DepthStencil) {
        desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    }
    D3D12_RESOURCE_STATES startState = MapTypeResStates(initState);
    if (HasFlag(initState, ResourceState::CopyDestination)) {
        startState = D3D12_RESOURCE_STATE_COMMON;
    }
    D3D12_CLEAR_VALUE clear{};
    clear.Format = rawFormat;
    if (auto ccv = std::get_if<ColorClearValue>(&clearValue)) {
        clear.Color[0] = ccv->R;
        clear.Color[1] = ccv->G;
        clear.Color[2] = ccv->B;
        clear.Color[3] = ccv->A;
    } else if (auto dcv = std::get_if<DepthStencilClearValue>(&clearValue)) {
        clear.DepthStencil.Depth = dcv->Depth;
        clear.DepthStencil.Stencil = (UINT8)dcv->Stencil;
    }
    const D3D12_CLEAR_VALUE* clearPtr = nullptr;
    if ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) || (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) {
        clearPtr = &clear;
    }
    D3D12MA::ALLOCATION_DESC allocDesc{};
    allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    if (HasFlag(tips, ResourceMemoryTip::Dedicated)) {
        allocDesc.Flags = static_cast<D3D12MA::ALLOCATION_FLAGS>(allocDesc.Flags | D3D12MA::ALLOCATION_FLAG_COMMITTED);
    }
    ComPtr<ID3D12Resource> texture;
    ComPtr<D3D12MA::Allocation> allocRes;
    if (HRESULT hr = _mainAlloc->CreateResource(
            &allocDesc,
            &desc,
            startState,
            clearPtr,
            allocRes.GetAddressOf(),
            IID_PPV_ARGS(texture.GetAddressOf()));
        FAILED(hr)) {
        RADRAY_ERR_LOG("d3d12 cannot create texture, reason={} (code:{})", GetErrorName(hr), hr);
        return nullptr;
    }
    SetObjectName(name, texture.Get(), allocRes.Get());
    return radray::make_shared<TextureD3D12>(std::move(texture), std::move(allocRes), startState, type);
}

Nullable<radray::shared_ptr<BufferView>> DeviceD3D12::CreateBufferView(
    Buffer* buffer,
    ResourceType type,
    TextureFormat format,
    uint64_t offset,
    uint32_t count,
    uint32_t stride) noexcept {
    auto buf = Underlying(buffer);
    DescriptorHeap* heap = GetCbvSrvUavHeap();
    UINT heapIndex = heap->Allocate();
    auto guard = radray::MakeScopeGuard([=]() noexcept { heap->Recycle(heapIndex); });
    DXGI_FORMAT dxgiFormat;
    if (type == ResourceType::CBuffer || type == ResourceType::PushConstant) {
        D3D12_CONSTANT_BUFFER_VIEW_DESC desc{};
        desc.BufferLocation = buf->_buf->GetGPUVirtualAddress() + offset;
        desc.SizeInBytes = count;
        _device->CreateConstantBufferView(&desc, heap->HandleCpu(heapIndex));
        dxgiFormat = DXGI_FORMAT_UNKNOWN;
    } else if (type == ResourceType::Buffer) {
        D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
        desc.Format = MapType(format);
        desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.Buffer.FirstElement = offset;
        desc.Buffer.NumElements = count;
        desc.Buffer.StructureByteStride = stride;
        desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
        _device->CreateShaderResourceView(buf->_buf.Get(), &desc, heap->HandleCpu(heapIndex));
        dxgiFormat = desc.Format;
    } else if (type == ResourceType::BufferRW) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
        desc.Format = MapType(format);
        desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        desc.Buffer.FirstElement = offset;
        desc.Buffer.NumElements = count;
        desc.Buffer.StructureByteStride = stride;
        desc.Buffer.CounterOffsetInBytes = 0;
        desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
        _device->CreateUnorderedAccessView(buf->_buf.Get(), nullptr, &desc, heap->HandleCpu(heapIndex));
        dxgiFormat = desc.Format;
    } else {
        RADRAY_ERR_LOG("d3d12 cannot create buffer view, type={}", type);
        return nullptr;
    }
    BufferViewD3D12Desc bv{
        buf,
        heap,
        heapIndex,
        type,
        dxgiFormat,
        count,
        offset,
        stride};
    auto result = std::make_shared<BufferViewD3D12>(bv);
    guard.Dismiss();
    return result;
}

Nullable<radray::shared_ptr<TextureView>> DeviceD3D12::CreateTextureView(
    Texture* texture,
    ResourceType type,
    TextureFormat format,
    TextureDimension dim,
    uint32_t baseArrayLayer,
    uint32_t arrayLayerCount,
    uint32_t baseMipLevel,
    uint32_t mipLevelCount) noexcept {
    // https://learn.microsoft.com/zh-cn/windows/win32/direct3d12/subresources
    // 三种 slice: mip 横向, array 纵向, plane 看起来更像是通道
    auto tex = Underlying(texture);
    DescriptorHeap* heap = nullptr;
    UINT heapIndex = std::numeric_limits<UINT>::max();
    auto guard = radray::MakeScopeGuard([&]() noexcept {
        if (heap != nullptr && heapIndex != std::numeric_limits<UINT>::max()) {
            heap->Recycle(heapIndex);
        }
    });
    DXGI_FORMAT dxgiFormat;
    if (type == ResourceType::Texture) {
        heap = GetCbvSrvUavHeap();
        heapIndex = heap->Allocate();
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = MapShaderResourceType(format);
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        switch (dim) {
            case TextureDimension::Dim1D:
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
                srvDesc.Texture1D.MostDetailedMip = baseMipLevel;
                srvDesc.Texture1D.MipLevels = mipLevelCount;
                break;
            case TextureDimension::Dim1DArray:
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
                srvDesc.Texture1DArray.MostDetailedMip = baseMipLevel;
                srvDesc.Texture1DArray.MipLevels = mipLevelCount;
                srvDesc.Texture1DArray.FirstArraySlice = baseArrayLayer;
                srvDesc.Texture1DArray.ArraySize = arrayLayerCount;
                break;
            case TextureDimension::Dim2D:
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srvDesc.Texture2D.MostDetailedMip = baseMipLevel;
                srvDesc.Texture2D.MipLevels = mipLevelCount;
                srvDesc.Texture2D.PlaneSlice = 0;
                break;
            case TextureDimension::Dim2DArray:
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                srvDesc.Texture2DArray.MostDetailedMip = baseMipLevel;
                srvDesc.Texture2DArray.MipLevels = mipLevelCount;
                srvDesc.Texture2DArray.FirstArraySlice = baseArrayLayer;
                srvDesc.Texture2DArray.ArraySize = arrayLayerCount;
                srvDesc.Texture2DArray.PlaneSlice = 0;
                break;
            case TextureDimension::Dim3D:
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
                srvDesc.Texture3D.MostDetailedMip = baseMipLevel;
                srvDesc.Texture3D.MipLevels = mipLevelCount;
                break;
            case TextureDimension::Cube:
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                srvDesc.TextureCube.MostDetailedMip = baseMipLevel;
                srvDesc.TextureCube.MipLevels = mipLevelCount;
                break;
            case TextureDimension::CubeArray:
                // https://learn.microsoft.com/zh-cn/windows/win32/api/d3d12/ns-d3d12-d3d12_texcube_array_srv
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
                srvDesc.TextureCubeArray.MostDetailedMip = baseMipLevel;
                srvDesc.TextureCubeArray.MipLevels = mipLevelCount;
                srvDesc.TextureCubeArray.First2DArrayFace = baseArrayLayer;
                srvDesc.TextureCubeArray.NumCubes = arrayLayerCount;
                break;
            default:
                RADRAY_ERR_LOG("d3d12 cannot create texture view, type={} dim={}", type, dim);
                return nullptr;
        }
        heap->Create(tex->_tex.Get(), srvDesc, heapIndex);
        dxgiFormat = srvDesc.Format;
    } else if (type == ResourceType::RenderTarget) {
        heap = GetRtvHeap();
        heapIndex = heap->Allocate();
        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
        rtvDesc.Format = MapType(format);
        switch (dim) {
            case TextureDimension::Dim1D:
                rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1D;
                rtvDesc.Texture1D.MipSlice = baseMipLevel;
                break;
            case TextureDimension::Dim1DArray:
                rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1DARRAY;
                rtvDesc.Texture1DArray.MipSlice = baseMipLevel;
                rtvDesc.Texture1DArray.FirstArraySlice = baseArrayLayer;
                rtvDesc.Texture1DArray.ArraySize = arrayLayerCount;
                break;
            case TextureDimension::Dim2D:
                rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
                rtvDesc.Texture2D.MipSlice = baseMipLevel;
                rtvDesc.Texture2D.PlaneSlice = 0;
                break;
            case TextureDimension::Dim2DArray:
                rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                rtvDesc.Texture2DArray.MipSlice = baseMipLevel;
                rtvDesc.Texture2DArray.FirstArraySlice = baseArrayLayer;
                rtvDesc.Texture2DArray.ArraySize = arrayLayerCount;
                rtvDesc.Texture2DArray.PlaneSlice = 0;
                break;
            case TextureDimension::Dim3D:
                rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
                rtvDesc.Texture3D.MipSlice = baseMipLevel;
                rtvDesc.Texture3D.FirstWSlice = baseArrayLayer;
                rtvDesc.Texture3D.WSize = arrayLayerCount;
            default:
                RADRAY_ERR_LOG("d3d12 cannot create texture view, type={} dim={}", type, dim);
                return nullptr;
        }
        heap->Create(tex->_tex.Get(), rtvDesc, heapIndex);
        dxgiFormat = rtvDesc.Format;
    } else if (type == ResourceType::DepthStencil) {
        heap = GetDsvHeap();
        heapIndex = heap->Allocate();
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format = MapType(format);
        switch (dim) {
            case TextureDimension::Dim1D:
                dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1D;
                dsvDesc.Texture1D.MipSlice = baseMipLevel;
                break;
            case TextureDimension::Dim1DArray:
                dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1DARRAY;
                dsvDesc.Texture1DArray.MipSlice = baseMipLevel;
                dsvDesc.Texture1DArray.FirstArraySlice = baseArrayLayer;
                dsvDesc.Texture1DArray.ArraySize = arrayLayerCount;
                break;
            case TextureDimension::Dim2D:
                dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
                dsvDesc.Texture2D.MipSlice = baseMipLevel;
                break;
            case TextureDimension::Dim2DArray:
                dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
                dsvDesc.Texture2DArray.MipSlice = baseMipLevel;
                dsvDesc.Texture2DArray.FirstArraySlice = baseArrayLayer;
                dsvDesc.Texture2DArray.ArraySize = arrayLayerCount;
                break;
            default:
                RADRAY_ERR_LOG("d3d12 cannot create texture view, type={} dim={}", type, dim);
                return nullptr;
        }
        heap->Create(tex->_tex.Get(), dsvDesc, heapIndex);
        dxgiFormat = dsvDesc.Format;
    } else if (type == ResourceType::TextureRW) {
        heap = GetCbvSrvUavHeap();
        heapIndex = heap->Allocate();
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = MapShaderResourceType(format);
        switch (dim) {
            case TextureDimension::Dim1D:
                uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D;
                uavDesc.Texture1D.MipSlice = baseMipLevel;
                break;
            case TextureDimension::Dim1DArray:
                uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1DARRAY;
                uavDesc.Texture1DArray.MipSlice = baseMipLevel;
                uavDesc.Texture1DArray.FirstArraySlice = baseArrayLayer;
                uavDesc.Texture1DArray.ArraySize = arrayLayerCount;
                break;
            case TextureDimension::Dim2D:
                uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                uavDesc.Texture2D.MipSlice = baseMipLevel;
                uavDesc.Texture2D.PlaneSlice = 0;
                break;
            case TextureDimension::Dim2DArray:
                uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
                uavDesc.Texture2DArray.MipSlice = baseMipLevel;
                uavDesc.Texture2DArray.FirstArraySlice = baseArrayLayer;
                uavDesc.Texture2DArray.ArraySize = arrayLayerCount;
                uavDesc.Texture2DArray.PlaneSlice = 0;
                break;
            case TextureDimension::Dim3D:
                uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
                uavDesc.Texture3D.MipSlice = baseMipLevel;
                uavDesc.Texture3D.FirstWSlice = baseArrayLayer;
                uavDesc.Texture3D.WSize = arrayLayerCount;
                break;
            default:
                RADRAY_ERR_LOG("d3d12 cannot create texture view, type={} dim={}", type, dim);
                return nullptr;
        }
        heap->Create(tex->_tex.Get(), uavDesc, heapIndex);
        dxgiFormat = uavDesc.Format;
    } else {
        RADRAY_ERR_LOG("d3d12 cannot create texture view, type={} dim={}", type, dim);
        return nullptr;
    }
    TextureViewD3D12Desc tvd{
        tex,
        heap,
        heapIndex,
        type,
        dxgiFormat,
        dim,
        baseArrayLayer,
        arrayLayerCount,
        baseMipLevel,
        mipLevelCount};
    auto result = radray::make_shared<TextureViewD3D12>(tvd);
    guard.Dismiss();
    return result;
}

Nullable<radray::shared_ptr<DescriptorSet>> DeviceD3D12::CreateDescriptorSet(
    RootSignature* rootSignature,
    uint32_t set) noexcept {
    auto rs = Underlying(rootSignature);

    return nullptr;
}

DescriptorHeap* DeviceD3D12::GetCbvSrvUavHeap() noexcept {
    if (_cbvSrvUavHeap == nullptr) {
        _cbvSrvUavHeap = radray::make_unique<DescriptorHeap>(
            _device.Get(),
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
            1 << 14,
            false);
    }
    return _cbvSrvUavHeap.get();
}

DescriptorHeap* DeviceD3D12::GetRtvHeap() noexcept {
    if (_rtvHeap == nullptr) {
        _rtvHeap = radray::make_unique<DescriptorHeap>(
            _device.Get(),
            D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
            1 << 8,
            false);
    }
    return _rtvHeap.get();
}

DescriptorHeap* DeviceD3D12::GetDsvHeap() noexcept {
    if (_dsvHeap == nullptr) {
        _dsvHeap = radray::make_unique<DescriptorHeap>(
            _device.Get(),
            D3D12_DESCRIPTOR_HEAP_TYPE_DSV,
            1 << 8,
            false);
    }
    return _dsvHeap.get();
}

DescriptorHeap* DeviceD3D12::GetGpuHeap() noexcept {
    if (_gpuHeap == nullptr) {
        _gpuHeap = radray::make_unique<DescriptorHeap>(
            _device.Get(),
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
            1 << 16,
            true);
    }
    return _gpuHeap.get();
}

DescriptorHeap* DeviceD3D12::GetGpuSamplerHeap() noexcept {
    if (_gpuSamplerHeap == nullptr) {
        _gpuSamplerHeap = radray::make_unique<DescriptorHeap>(
            _device.Get(),
            D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
            1 << 8,
            true);
    }
    return _gpuSamplerHeap.get();
}

Nullable<radray::shared_ptr<DeviceD3D12>> CreateDevice(const D3D12DeviceDescriptor& desc) {
    uint32_t dxgiFactoryFlags = 0;
    if (desc.IsEnableDebugLayer) {
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
            debugController->EnableDebugLayer();
            dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
            if (desc.IsEnableGpuBasedValid) {
                ComPtr<ID3D12Debug1> debug1;
                if (SUCCEEDED(debugController.As(&debug1))) {
                    debug1->SetEnableGPUBasedValidation(true);
                } else {
                    RADRAY_WARN_LOG("cannot get ID3D12Debug1. cannot enable gpu based validation");
                }
            }
        } else {
            RADRAY_WARN_LOG("cannot find ID3D12Debug");
        }
    }
    ComPtr<IDXGIFactory4> dxgiFactory;
    if (HRESULT hr = CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(dxgiFactory.GetAddressOf()));
        FAILED(hr)) {
        RADRAY_ERR_LOG("cannot create IDXGIFactory4, reason={} (code:{})", GetErrorName(hr), hr);
        return nullptr;
    }
    ComPtr<IDXGIAdapter1> adapter;
    if (desc.AdapterIndex.has_value()) {
        uint32_t index = desc.AdapterIndex.value();
        if (HRESULT hr = dxgiFactory->EnumAdapters1(index, adapter.GetAddressOf());
            FAILED(hr)) {
            RADRAY_ERR_LOG("cannot get IDXGIAdapter1 at index {}, reason={} (code:{})", index, GetErrorName(hr), hr);
            return nullptr;
        }
    } else {
        ComPtr<IDXGIFactory6> factory6;
        if (SUCCEEDED(dxgiFactory->QueryInterface(IID_PPV_ARGS(factory6.GetAddressOf())))) {
            ComPtr<IDXGIAdapter1> temp;
            for (
                auto adapterIndex = 0u;
                factory6->EnumAdapterByGpuPreference(adapterIndex, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(temp.ReleaseAndGetAddressOf())) != DXGI_ERROR_NOT_FOUND;
                adapterIndex++) {
                DXGI_ADAPTER_DESC1 adapDesc;
                temp->GetDesc1(&adapDesc);
                if ((adapDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
                    radray::wstring s{adapDesc.Description};
                    RADRAY_INFO_LOG("D3D12 find device: {}", ToMultiByte(s).value());
                }
            }
            for (
                auto adapterIndex = 0u;
                factory6->EnumAdapterByGpuPreference(adapterIndex, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(temp.GetAddressOf())) != DXGI_ERROR_NOT_FOUND;
                adapterIndex++) {
                DXGI_ADAPTER_DESC1 adapDesc;
                temp->GetDesc1(&adapDesc);
                if ((adapDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
                    if (SUCCEEDED(D3D12CreateDevice(temp.Get(), D3D_FEATURE_LEVEL_11_0, _uuidof(ID3D12Device), nullptr))) {
                        break;
                    }
                }
                temp = nullptr;
            }
            adapter = temp;
        } else {
            if (dxgiFactory->EnumAdapters1(0, adapter.GetAddressOf())) {
                DXGI_ADAPTER_DESC1 adapDesc;
                adapter->GetDesc1(&adapDesc);
                if ((adapDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 ||
                    FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, _uuidof(ID3D12Device), nullptr))) {
                    adapter = nullptr;
                }
            }
        }
    }
    if (adapter == nullptr) {
        RADRAY_ERR_LOG("cannot get IDXGIAdapter1");
        return nullptr;
    }
    ComPtr<ID3D12Device> device;
    if (HRESULT hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.GetAddressOf()));
        FAILED(hr)) {
        RADRAY_ERR_LOG("cannot create ID3D12Device, reason={} (code:{})", GetErrorName(hr), hr);
        return nullptr;
    }
    {
        DXGI_ADAPTER_DESC1 adapDesc{};
        adapter->GetDesc1(&adapDesc);
        radray::wstring s{adapDesc.Description};
        RADRAY_INFO_LOG("select device: {}", ToMultiByte(s).value());
    }
    ComPtr<D3D12MA::Allocator> alloc;
    {
        D3D12MA::ALLOCATOR_DESC allocDesc{};
        allocDesc.Flags = D3D12MA::ALLOCATOR_FLAG_NONE;
        allocDesc.pDevice = device.Get();
        allocDesc.pAdapter = adapter.Get();
#ifdef RADRAY_ENABLE_MIMALLOC
        D3D12MA::ALLOCATION_CALLBACKS allocationCallbacks{};
        allocationCallbacks.pAllocate = [](size_t Size, size_t Alignment, void* pPrivateData) {
            RADRAY_UNUSED(pPrivateData);
            return mi_malloc_aligned(Size, Alignment);
        };
        allocationCallbacks.pFree = [](void* pMemory, void* pPrivateData) {
            RADRAY_UNUSED(pPrivateData);
            mi_free(pMemory);
        };
        allocDesc.pAllocationCallbacks = &allocationCallbacks;
#endif
        allocDesc.Flags = D3D12MA::ALLOCATOR_FLAG_MSAA_TEXTURES_ALWAYS_COMMITTED;
        if (HRESULT hr = D3D12MA::CreateAllocator(&allocDesc, alloc.GetAddressOf());
            FAILED(hr)) {
            RADRAY_ERR_LOG("cannot create D3D12MA::Allocator, reason={} (code:{})", GetErrorName(hr), hr);
            return nullptr;
        }
    }
    auto result = radray::make_shared<DeviceD3D12>(device, dxgiFactory, adapter, alloc);
    RADRAY_INFO_LOG("========== Feature ==========");
    {
        LARGE_INTEGER l;
        HRESULT hr = adapter->CheckInterfaceSupport(IID_IDXGIDevice, &l);
        if (SUCCEEDED(hr)) {
            const int64_t mask = 0xFFFF;
            auto quad = l.QuadPart;
            auto ver = radray::format(
                "{}.{}.{}.{}",
                quad >> 48,
                (quad >> 32) & mask,
                (quad >> 16) & mask,
                quad & mask);
            RADRAY_INFO_LOG("Driver Version: {}", ver);
        } else {
            RADRAY_WARN_LOG("get driver version failed");
        }
    }
    {
        BOOL allowTearing = FALSE;
        ComPtr<IDXGIFactory6> factory6;
        if (SUCCEEDED(dxgiFactory.As(&factory6))) {
            if (HRESULT hr = factory6->CheckFeatureSupport(
                    DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                    &allowTearing,
                    sizeof(allowTearing));
                FAILED(hr)) {
                RADRAY_DEBUG_LOG("query IDXGIFactory6 feature DXGI_FEATURE_PRESENT_ALLOW_TEARING failed, reason={} (code={})", GetErrorName(hr), hr);
            }
        }
        RADRAY_INFO_LOG("Allow Tearing: {}", static_cast<bool>(allowTearing));
        result->_isAllowTearing = allowTearing;
    }
    const CD3DX12FeatureSupport& fs = result->GetFeatures();
    if (SUCCEEDED(fs.GetStatus())) {
        RADRAY_INFO_LOG("Feature Level: {}", fs.MaxSupportedFeatureLevel());
        RADRAY_INFO_LOG("Shader Model: {}", fs.HighestShaderModel());
        RADRAY_INFO_LOG("TBR: {}", static_cast<bool>(fs.TileBasedRenderer()));
        RADRAY_INFO_LOG("UMA: {}", static_cast<bool>(fs.UMA()));
    } else {
        RADRAY_WARN_LOG("check d3d12 feature failed");
    }
    RADRAY_INFO_LOG("=============================");
    return result;
}

}  // namespace radray::render::d3d12
