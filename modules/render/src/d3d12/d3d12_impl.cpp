#include <radray/render/backend/d3d12_impl.h>

#include <bit>
#include <cstring>
#include <algorithm>
#include <limits>
#include <unordered_set>

#include <radray/text_encoding.h>

namespace radray::render::d3d12 {

static LogLevel _MapD3D12ValidationLogLevel(D3D12_MESSAGE_SEVERITY severity) noexcept {
    switch (severity) {
        case D3D12_MESSAGE_SEVERITY_CORRUPTION: return LogLevel::Critical;
        case D3D12_MESSAGE_SEVERITY_ERROR: return LogLevel::Err;
        case D3D12_MESSAGE_SEVERITY_WARNING: return LogLevel::Warn;
        case D3D12_MESSAGE_SEVERITY_INFO: return LogLevel::Info;
        case D3D12_MESSAGE_SEVERITY_MESSAGE: return LogLevel::Debug;
        default: return LogLevel::Info;
    }
}

static void _LogD3D12ValidationMessage(
    DeviceD3D12* device,
    D3D12_MESSAGE_CATEGORY category,
    D3D12_MESSAGE_SEVERITY severity,
    D3D12_MESSAGE_ID id,
    const char* description) noexcept {
    if (device == nullptr) {
        return;
    }
    try {
        const LogLevel lvl = _MapD3D12ValidationLogLevel(severity);
        const auto message = fmt::format(
            "d3d12 validation message. category:{} id:{}\n{}",
            category,
            static_cast<uint32_t>(id),
            description == nullptr ? "" : description);
        if (device->_logCallback) {
            device->_logCallback.Get()(lvl, message, device->_logUserData.Get());
            return;
        }
        switch (lvl) {
            case LogLevel::Critical: RADRAY_ERR_LOG("{}", message); break;
            case LogLevel::Err: RADRAY_ERR_LOG("{}", message); break;
            case LogLevel::Warn: RADRAY_WARN_LOG("{}", message); break;
            case LogLevel::Info: RADRAY_INFO_LOG("{}", message); break;
            case LogLevel::Debug: RADRAY_DEBUG_LOG("{}", message); break;
            default: RADRAY_INFO_LOG("{}", message); break;
        }
    } catch (...) {
    }
}

static void WINAPI _D3D12ValidationMessageCallback(
    D3D12_MESSAGE_CATEGORY category,
    D3D12_MESSAGE_SEVERITY severity,
    D3D12_MESSAGE_ID id,
    LPCSTR pDescription,
    void* pContext) noexcept {
    auto* device = static_cast<DeviceD3D12*>(pContext);
    if (device == nullptr || !device->_isDebugLayerEnabled) {
        return;
    }
    _LogD3D12ValidationMessage(device, category, severity, id, pDescription);
}

static std::optional<D3D12_DESCRIPTOR_RANGE_TYPE> _MapDescriptorRangeType(ResourceBindType type) noexcept {
    switch (type) {
        case ResourceBindType::CBuffer:
            return D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
        case ResourceBindType::Buffer:
        case ResourceBindType::TexelBuffer:
        case ResourceBindType::Texture:
        case ResourceBindType::AccelerationStructure:
            return D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        case ResourceBindType::RWBuffer:
        case ResourceBindType::RWTexelBuffer:
        case ResourceBindType::RWTexture:
            return D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        case ResourceBindType::Sampler:
            return D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        case ResourceBindType::UNKNOWN:
            return std::nullopt;
    }
    return std::nullopt;
}

static std::optional<ResourceBindType> _GetResourceViewBindType(ResourceView* view) noexcept {
    if (view == nullptr) {
        return std::nullopt;
    }
    const auto tag = view->GetTag();
    if (tag.HasFlag(RenderObjectTag::BufferView)) {
        const auto* bufferView = static_cast<BufferViewD3D12*>(view);
        switch (bufferView->_desc.Usage) {
            case BufferViewUsage::CBuffer: return ResourceBindType::CBuffer;
            case BufferViewUsage::ReadOnlyStorage: return ResourceBindType::Buffer;
            case BufferViewUsage::ReadWriteStorage: return ResourceBindType::RWBuffer;
            case BufferViewUsage::TexelReadOnly: return ResourceBindType::TexelBuffer;
            case BufferViewUsage::TexelReadWrite: return ResourceBindType::RWTexelBuffer;
        }
        Unreachable();
    }
    if (tag.HasFlag(RenderObjectTag::TextureView)) {
        const auto* textureView = static_cast<TextureViewD3D12*>(view);
        if (textureView->_desc.Usage == TextureViewUsage::UnorderedAccess) {
            return ResourceBindType::RWTexture;
        }
        return ResourceBindType::Texture;
    }
    if (tag.HasFlag(RenderObjectTag::AccelerationStructureView)) {
        return ResourceBindType::AccelerationStructure;
    }
    return std::nullopt;
}

static bool _CopyResourceViewToGpuHeap(
    ResourceView* view,
    GpuDescriptorHeapViewRAII& dstHeap,
    uint32_t dstIndex) noexcept {
    if (view == nullptr || !dstHeap.IsValid()) {
        return false;
    }
    const auto tag = view->GetTag();
    if (tag.HasFlag(RenderObjectTag::BufferView)) {
        static_cast<BufferViewD3D12*>(view)->_heapView.CopyTo(0, 1, dstHeap, dstIndex);
        return true;
    }
    if (tag.HasFlag(RenderObjectTag::TextureView)) {
        static_cast<TextureViewD3D12*>(view)->_heapView.CopyTo(0, 1, dstHeap, dstIndex);
        return true;
    }
    if (tag.HasFlag(RenderObjectTag::AccelerationStructureView)) {
        static_cast<AccelerationStructureViewD3D12*>(view)->_heapView.CopyTo(0, 1, dstHeap, dstIndex);
        return true;
    }
    return false;
}

struct _StaticSamplerSelectionD3D12 {
    vector<BindingParameterLayout> PublicParameters{};
    vector<StaticSamplerLayout> StaticSamplers{};
    vector<D3D12_STATIC_SAMPLER_DESC> RawStaticSamplers{};
    vector<uint8_t> IsStaticParameter{};
};

static D3D12_STATIC_SAMPLER_DESC _ToD3D12StaticSamplerDesc(
    const SamplerDescriptor& desc,
    uint32_t shaderRegister,
    uint32_t registerSpace,
    ShaderStages stages) noexcept {
    D3D12_STATIC_SAMPLER_DESC rawDesc{};
    rawDesc.Filter = MapType(desc.MinFilter, desc.MagFilter, desc.MipmapFilter, desc.Compare.has_value(), desc.AnisotropyClamp);
    rawDesc.AddressU = MapType(desc.AddressS);
    rawDesc.AddressV = MapType(desc.AddressT);
    rawDesc.AddressW = MapType(desc.AddressR);
    rawDesc.MipLODBias = 0.0f;
    rawDesc.MaxAnisotropy = std::max(desc.AnisotropyClamp, 1u);
    rawDesc.ComparisonFunc = desc.Compare.has_value() ? MapType(desc.Compare.value()) : D3D12_COMPARISON_FUNC_NEVER;
    rawDesc.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    rawDesc.MinLOD = desc.LodMin;
    rawDesc.MaxLOD = desc.LodMax;
    rawDesc.ShaderRegister = shaderRegister;
    rawDesc.RegisterSpace = registerSpace;
    rawDesc.ShaderVisibility = MapShaderStages(stages);
    return rawDesc;
}

static std::optional<_StaticSamplerSelectionD3D12> _SelectStaticSamplersD3D12(
    const BindingLayout& layout,
    std::span<const D3D12BindingParameterInfo> lowering,
    std::span<const StaticSamplerDescriptor> staticSamplers) noexcept {
    const auto parameters = layout.GetParameters();
    if (lowering.size() != parameters.size()) {
        RADRAY_ERR_LOG("internal error: static sampler selection metadata size mismatch");
        return std::nullopt;
    }

    _StaticSamplerSelectionD3D12 result{};
    result.IsStaticParameter.resize(parameters.size(), 0);
    result.PublicParameters.reserve(parameters.size());
    result.StaticSamplers.reserve(staticSamplers.size());
    result.RawStaticSamplers.reserve(staticSamplers.size());

    unordered_set<uint64_t> usedBindings{};
    for (const auto& staticSampler : staticSamplers) {
        const uint64_t key = (static_cast<uint64_t>(staticSampler.Set.Value) << 32) | static_cast<uint64_t>(staticSampler.Binding);
        if (!usedBindings.insert(key).second) {
            RADRAY_ERR_LOG(
                "duplicate static sampler declaration at set {} binding {}",
                staticSampler.Set.Value,
                staticSampler.Binding);
            return std::nullopt;
        }

        size_t matchedIndex = parameters.size();
        for (size_t i = 0; i < parameters.size(); ++i) {
            const auto& parameter = parameters[i];
            if (parameter.Kind != BindingParameterKind::Sampler) {
                continue;
            }
            const auto& abi = std::get<ResourceBindingAbi>(parameter.Abi);
            if (abi.Set == staticSampler.Set && abi.Binding == staticSampler.Binding) {
                matchedIndex = i;
                break;
            }
        }
        if (matchedIndex == parameters.size()) {
            RADRAY_ERR_LOG(
                "static sampler at set {} binding {} does not match any shader sampler binding",
                staticSampler.Set.Value,
                staticSampler.Binding);
            return std::nullopt;
        }

        const auto& parameter = parameters[matchedIndex];
        const auto& abi = std::get<ResourceBindingAbi>(parameter.Abi);
        const auto& d3d12 = lowering[matchedIndex];
        if (abi.Type != ResourceBindType::Sampler || abi.IsBindless) {
            RADRAY_ERR_LOG(
                "static sampler '{}' must target a non-bindless sampler binding",
                parameter.Name);
            return std::nullopt;
        }
        if (abi.Count != 1) {
            RADRAY_ERR_LOG(
                "static sampler '{}' does not support sampler arrays (count={})",
                parameter.Name,
                abi.Count);
            return std::nullopt;
        }
        if (!d3d12.IsAvailable) {
            RADRAY_ERR_LOG("d3d12 lowering metadata is unavailable for static sampler '{}'", parameter.Name);
            return std::nullopt;
        }
        if (staticSampler.Stages != ShaderStage::UNKNOWN && staticSampler.Stages != parameter.Stages) {
            RADRAY_ERR_LOG(
                "static sampler '{}' stage mismatch. shader={}, rootSignature={}",
                parameter.Name,
                parameter.Stages,
                staticSampler.Stages);
            return std::nullopt;
        }

        result.IsStaticParameter[matchedIndex] = 1;
        const ShaderStages effectiveStages = staticSampler.Stages == ShaderStage::UNKNOWN ? parameter.Stages : staticSampler.Stages;
        const string effectiveName = staticSampler.Name.empty() ? parameter.Name : staticSampler.Name;
        result.StaticSamplers.push_back(StaticSamplerLayout{
            .Name = effectiveName,
            .Id = parameter.Id,
            .Set = abi.Set,
            .Binding = abi.Binding,
            .Stages = effectiveStages,
            .Desc = staticSampler.Desc,
        });
        result.RawStaticSamplers.push_back(_ToD3D12StaticSamplerDesc(
            staticSampler.Desc,
            d3d12.ShaderRegister,
            d3d12.RegisterSpace,
            effectiveStages));
    }

    for (size_t i = 0; i < parameters.size(); ++i) {
        if (!result.IsStaticParameter[i]) {
            result.PublicParameters.push_back(parameters[i]);
        }
    }
    return result;
}

static bool _BindDescriptorSetD3D12(
    CmdListD3D12* cmdList,
    RootSigD3D12* boundRootSig,
    DescriptorSetIndex setIndex,
    DescriptorSet* set_,
    bool graphicsRoot) noexcept {
    if (boundRootSig == nullptr) {
        RADRAY_ERR_LOG("bind root signature before CommandEncoder::BindDescriptorSet");
        return false;
    }
    if (set_ == nullptr) {
        RADRAY_ERR_LOG("descriptor set is null");
        return false;
    }
    auto* set = CastD3D12Object(set_);
    if (set == nullptr || !set->IsValid()) {
        RADRAY_ERR_LOG("descriptor set is invalid");
        return false;
    }
    if (set->GetRootSignature() != boundRootSig) {
        RADRAY_ERR_LOG("descriptor set belongs to a different root signature");
        return false;
    }
    if (set->GetSetIndex() != setIndex) {
        RADRAY_ERR_LOG(
            "descriptor set index mismatch expected: {}, actual: {}",
            setIndex.Value,
            set->GetSetIndex().Value);
        return false;
    }
    if (!set->IsFullyWritten()) {
        const auto params = boundRootSig->GetDescriptorSetLayout(setIndex);
        for (const auto& param : params) {
            auto infoOpt = boundRootSig->FindParameterInfo(param.Id);
            if (!infoOpt.HasValue() || infoOpt.Get() == nullptr) {
                continue;
            }
            const auto* info = infoOpt.Get();
            const auto& written = info->Kind == BindingParameterKind::Sampler ? set->_samplerWritten : set->_resourceWritten;
            bool complete = true;
            for (uint32_t i = 0; i < info->DescriptorCount; ++i) {
                const uint32_t index = info->DescriptorHeapOffset + i;
                if (index >= written.size() || !written[index]) {
                    complete = false;
                    break;
                }
            }
            if (!complete) {
                RADRAY_ERR_LOG("descriptor set is missing parameter '{}'", param.Name);
                break;
            }
        }
        return false;
    }
    auto setInfoOpt = boundRootSig->FindDescriptorSetInfo(setIndex);
    if (!setInfoOpt.HasValue() || setInfoOpt.Get() == nullptr) {
        RADRAY_ERR_LOG("descriptor set {} is unavailable in root signature", setIndex.Value);
        return false;
    }
    const auto* setInfo = setInfoOpt.Get();

    ID3D12DescriptorHeap* heaps[] = {
        cmdList->_device->_gpuResHeap->GetNative(),
        cmdList->_device->_gpuSamplerHeap->GetNative()};
    cmdList->_cmdList->SetDescriptorHeaps((UINT)radray::ArrayLength(heaps), heaps);

    auto bindTable = [&](uint32_t rootParameterIndex, D3D12_GPU_DESCRIPTOR_HANDLE handle) noexcept {
        if (graphicsRoot) {
            cmdList->_cmdList->SetGraphicsRootDescriptorTable(rootParameterIndex, handle);
        } else {
            cmdList->_cmdList->SetComputeRootDescriptorTable(rootParameterIndex, handle);
        }
    };

    if (setInfo->ResourceDescriptorCount > 0) {
        bindTable(setInfo->ResourceRootParameterIndex, set->_resHeapView.GetHeap()->HandleGpu(set->_resHeapView.GetStart()));
    }
    if (setInfo->SamplerDescriptorCount > 0) {
        bindTable(setInfo->SamplerRootParameterIndex, set->_samplerHeapView.GetHeap()->HandleGpu(set->_samplerHeapView.GetStart()));
    }
    return true;
}

static bool _BindlessArrayMatchesD3D12(
    const RootSigD3D12::BindlessSetInfo& bindlessInfo,
    const BindlessArrayD3D12* array) noexcept {
    if (array == nullptr || !array->IsValid()) {
        RADRAY_ERR_LOG("bindless array is invalid");
        return false;
    }
    if (array->_slotType != bindlessInfo.SlotType) {
        RADRAY_ERR_LOG(
            "bindless array slot type mismatch for set {} expected: {}, actual: {}",
            bindlessInfo.SetIndex.Value,
            static_cast<uint32_t>(bindlessInfo.SlotType),
            static_cast<uint32_t>(array->_slotType));
        return false;
    }
    for (size_t i = 0; i < array->_slotResourceTypes.size(); ++i) {
        const ResourceBindType slotType = array->_slotResourceTypes[i];
        if (slotType == ResourceBindType::UNKNOWN) {
            continue;
        }
        bool isCompatible = false;
        switch (bindlessInfo.Type) {
            case ResourceBindType::Buffer:
            case ResourceBindType::RWBuffer:
                isCompatible = slotType == bindlessInfo.Type;
                break;
            case ResourceBindType::Texture:
                isCompatible = slotType == ResourceBindType::Texture;
                break;
            default:
                isCompatible = false;
                break;
        }
        if (!isCompatible) {
            RADRAY_ERR_LOG(
                "bindless array slot {} type mismatch for set {} expected: {}, actual: {}",
                i,
                bindlessInfo.SetIndex.Value,
                bindlessInfo.Type,
                slotType);
            return false;
        }
    }
    return true;
}

static bool _BindBindlessArrayD3D12(
    CmdListD3D12* cmdList,
    RootSigD3D12* boundRootSig,
    DescriptorSetIndex setIndex,
    BindlessArray* array_,
    bool graphicsRoot) noexcept {
    if (boundRootSig == nullptr) {
        RADRAY_ERR_LOG("bind root signature before CommandEncoder::BindBindlessArray");
        return false;
    }
    if (array_ == nullptr) {
        RADRAY_ERR_LOG("bindless array is null");
        return false;
    }
    auto* array = static_cast<BindlessArrayD3D12*>(array_);
    if (array == nullptr || !array->IsValid()) {
        RADRAY_ERR_LOG("bindless array is invalid");
        return false;
    }
    auto bindlessInfoOpt = boundRootSig->FindBindlessSetInfo(setIndex);
    if (!bindlessInfoOpt.HasValue() || bindlessInfoOpt.Get() == nullptr) {
        RADRAY_ERR_LOG("set {} is not declared as a bindless set", setIndex.Value);
        return false;
    }
    const auto* bindlessInfo = bindlessInfoOpt.Get();
    if (!_BindlessArrayMatchesD3D12(*bindlessInfo, array)) {
        return false;
    }
    if (!array->_resHeap.IsValid()) {
        RADRAY_ERR_LOG("bindless array does not have a resource descriptor heap slice");
        return false;
    }

    ID3D12DescriptorHeap* heaps[] = {
        cmdList->_device->_gpuResHeap->GetNative(),
        cmdList->_device->_gpuSamplerHeap->GetNative()};
    cmdList->_cmdList->SetDescriptorHeaps((UINT)radray::ArrayLength(heaps), heaps);

    const auto handle = array->_resHeap.GetHeap()->HandleGpu(array->_resHeap.GetStart());
    if (graphicsRoot) {
        cmdList->_cmdList->SetGraphicsRootDescriptorTable(bindlessInfo->RootParameterIndex, handle);
    } else {
        cmdList->_cmdList->SetComputeRootDescriptorTable(bindlessInfo->RootParameterIndex, handle);
    }
    return true;
}

static bool _PushConstantsD3D12(
    CmdListD3D12* cmdList,
    RootSigD3D12* boundRootSig,
    BindingParameterId id,
    const void* data,
    uint32_t size,
    bool graphicsRoot) noexcept {
    if (boundRootSig == nullptr) {
        RADRAY_ERR_LOG("bind root signature before CommandEncoder::PushConstants");
        return false;
    }
    if (data == nullptr) {
        RADRAY_ERR_LOG("push constant data is null");
        return false;
    }
    auto infoOpt = boundRootSig->FindParameterInfo(id);
    if (!infoOpt.HasValue() || infoOpt.Get() == nullptr) {
        RADRAY_ERR_LOG("binding parameter id {} is out of range", id.Value);
        return false;
    }
    const auto* info = infoOpt.Get();
    if (info->Kind != BindingParameterKind::PushConstant) {
        RADRAY_ERR_LOG("binding parameter id {} is not a push constant parameter", id.Value);
        return false;
    }
    if (size != info->PushConstantSize) {
        RADRAY_ERR_LOG(
            "push constant size mismatch for binding parameter id {} expected: {}, actual: {}",
            id.Value,
            info->PushConstantSize,
            size);
        return false;
    }
    if (graphicsRoot) {
        cmdList->_cmdList->SetGraphicsRoot32BitConstants(info->RootParameterIndex, size / 4, data, 0);
    } else {
        cmdList->_cmdList->SetComputeRoot32BitConstants(info->RootParameterIndex, size / 4, data, 0);
    }
    return true;
}

DescriptorHeap::DescriptorHeap(
    ID3D12Device* device,
    D3D12_DESCRIPTOR_HEAP_DESC desc) noexcept
    : _device(device) {
    if (HRESULT hr = _device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(_heap.GetAddressOf()));
        FAILED(hr)) {
        RADRAY_ABORT("ID3D12Device::CreateDescriptorHeap failed: {} {}", GetErrorName(hr), hr);
    }
    _desc = _heap->GetDesc();
    _cpuStart = _heap->GetCPUDescriptorHandleForHeapStart();
    _gpuStart = IsShaderVisible() ? _heap->GetGPUDescriptorHandleForHeapStart() : D3D12_GPU_DESCRIPTOR_HANDLE{0};
    _incrementSize = _device->GetDescriptorHandleIncrementSize(_desc.Type);
    RADRAY_DEBUG_LOG(
        "create DescriptorHeap. Type={}, IsShaderVisible={}, IncrementSize={}, Length={}, all={}(bytes)",
        _desc.Type,
        IsShaderVisible(),
        _incrementSize,
        _desc.NumDescriptors,
        UINT64(_desc.NumDescriptors) * _incrementSize);
}

ID3D12DescriptorHeap* DescriptorHeap::Get() const noexcept {
    return _heap.Get();
}

D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeap::GetHeapType() const noexcept {
    return _desc.Type;
}

UINT DescriptorHeap::GetLength() const noexcept {
    return _desc.NumDescriptors;
}

bool DescriptorHeap::IsShaderVisible() const noexcept {
    return (_desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) == D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
}

D3D12_GPU_DESCRIPTOR_HANDLE DescriptorHeap::HandleGpu(UINT index) const noexcept {
    return {_gpuStart.ptr + UINT64(index) * UINT64(_incrementSize)};
}

D3D12_CPU_DESCRIPTOR_HANDLE DescriptorHeap::HandleCpu(UINT index) const noexcept {
    return {_cpuStart.ptr + UINT64(index) * UINT64(_incrementSize)};
}

void DescriptorHeap::Create(ID3D12Resource* resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC& desc, UINT index) noexcept {
    _device->CreateUnorderedAccessView(resource, nullptr, &desc, HandleCpu(index));
}

void DescriptorHeap::Create(ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& desc, UINT index) noexcept {
    _device->CreateShaderResourceView(resource, &desc, HandleCpu(index));
}

void DescriptorHeap::Create(const D3D12_CONSTANT_BUFFER_VIEW_DESC& desc, UINT index) noexcept {
    _device->CreateConstantBufferView(&desc, HandleCpu(index));
}

void DescriptorHeap::Create(ID3D12Resource* resource, const D3D12_RENDER_TARGET_VIEW_DESC& desc, UINT index) noexcept {
    _device->CreateRenderTargetView(resource, &desc, HandleCpu(index));
}

void DescriptorHeap::Create(ID3D12Resource* resource, const D3D12_DEPTH_STENCIL_VIEW_DESC& desc, UINT index) noexcept {
    _device->CreateDepthStencilView(resource, &desc, HandleCpu(index));
}

void DescriptorHeap::Create(const D3D12_SAMPLER_DESC& desc, UINT index) noexcept {
    _device->CreateSampler(&desc, HandleCpu(index));
}

void DescriptorHeap::CopyTo(UINT start, UINT count, DescriptorHeap* dst, UINT dstStart) noexcept {
    _device->CopyDescriptorsSimple(
        count,
        dst->HandleCpu(dstStart),
        HandleCpu(start),
        _desc.Type);
}

D3D12_GPU_DESCRIPTOR_HANDLE DescriptorHeapView::HandleGpu() const noexcept {
    return Heap->HandleGpu(Start);
}

D3D12_CPU_DESCRIPTOR_HANDLE DescriptorHeapView::HandleCpu() const noexcept {
    return Heap->HandleCpu(Start);
}

bool DescriptorHeapView::IsValid() const noexcept {
    return Heap != nullptr;
}

CpuDescriptorAllocator::CpuDescriptorAllocator(
    ID3D12Device* device,
    D3D12_DESCRIPTOR_HEAP_TYPE type,
    UINT basicSize,
    UINT keepFreePages) noexcept
    : _device(device),
      _type(type),
      _basicSize(basicSize),
      _freePageKeepCount(keepFreePages) {}

CpuDescriptorAllocator::Page::Page(
    unique_ptr<DescriptorHeap> heap,
    ComPtr<D3D12MA::VirtualBlock> allocator,
    size_t capacity) noexcept
    : Heap(std::move(heap)),
      Allocator(std::move(allocator)),
      Capacity(capacity) {}

std::optional<CpuDescriptorAllocator::Allocation> CpuDescriptorAllocator::Allocate(UINT count) noexcept {
    if (count == 0) {
        return std::nullopt;
    }
    const size_t request = static_cast<size_t>(count);
    D3D12MA::VIRTUAL_ALLOCATION_DESC allocDesc{};
    allocDesc.Size = static_cast<UINT64>(request);
    allocDesc.Alignment = 1;
    if (!_pages.empty()) {
        const size_t n = _pages.size();
        const size_t start = (_hint < n) ? _hint : 0;
        for (size_t step = 0; step < n; step++) {
            const size_t pageIndex = (start + step) % n;
            Page& page = *_pages[pageIndex];
            if (page.Capacity - page.Used < request) {
                continue;
            }
            D3D12MA::VirtualAllocation vAlloc{};
            UINT64 offset = 0;
            if (FAILED(page.Allocator->Allocate(&allocDesc, &vAlloc, &offset))) {
                continue;
            }
            if (offset > static_cast<UINT64>(std::numeric_limits<UINT>::max())) {
                page.Allocator->FreeAllocation(vAlloc);
                continue;
            }
            page.Used += request;
            _hint = pageIndex;
            return std::make_optional(Allocation{
                .Heap = page.Heap.get(),
                .Start = static_cast<UINT>(offset),
                .Length = count,
                .PagePtr = _pages[pageIndex].get(),
                .Offset = static_cast<size_t>(offset),
                .VirtualAlloc = vAlloc,
            });
        }
    }
    const size_t desired = std::max<size_t>(static_cast<size_t>(_basicSize), request);
    const size_t pageCapacity = std::bit_ceil(desired);
    if (pageCapacity > static_cast<size_t>(std::numeric_limits<UINT>::max())) {
        return std::nullopt;
    }
    auto heap = make_unique<DescriptorHeap>(
        _device,
        D3D12_DESCRIPTOR_HEAP_DESC{
            _type,
            static_cast<UINT>(pageCapacity),
            D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
            0});
    D3D12MA::VIRTUAL_BLOCK_DESC blockDesc{};
    blockDesc.Size = static_cast<UINT64>(pageCapacity);
    blockDesc.Flags = D3D12MA::VIRTUAL_BLOCK_FLAG_NONE;
    ComPtr<D3D12MA::VirtualBlock> block;
    if (FAILED(D3D12MA::CreateVirtualBlock(&blockDesc, block.GetAddressOf()))) {
        return std::nullopt;
    }
    auto newPage = make_unique<Page>(std::move(heap), std::move(block), pageCapacity);
    Page* newPagePtr = newPage.get();
    _pages.emplace_back(std::move(newPage));
    const size_t pageIndex = _pages.size() - 1;
    _hint = pageIndex;
    Page& page = *_pages.back();
    D3D12MA::VirtualAllocation vAlloc{};
    UINT64 offset = 0;
    if (FAILED(page.Allocator->Allocate(&allocDesc, &vAlloc, &offset)) ||
        offset > static_cast<UINT64>(std::numeric_limits<UINT>::max())) {
        if (vAlloc.AllocHandle != 0) {
            page.Allocator->FreeAllocation(vAlloc);
        }
        _pages.pop_back();
        return std::nullopt;
    }
    page.Used += request;
    return std::make_optional(Allocation{
        .Heap = page.Heap.get(),
        .Start = static_cast<UINT>(offset),
        .Length = count,
        .PagePtr = newPagePtr,
        .Offset = static_cast<size_t>(offset),
        .VirtualAlloc = vAlloc,
    });
}

void CpuDescriptorAllocator::Destroy(CpuDescriptorAllocator::Allocation view) noexcept {
    if (view.PagePtr == nullptr) {
        return;
    }
    auto* page = view.PagePtr;
    RADRAY_ASSERT(page->Heap.get() == view.Heap);
    page->Allocator->FreeAllocation(view.VirtualAlloc);
    page->Used -= static_cast<size_t>(view.Length);
    if (page->Used == 0) {
        TryReleaseFreePages();
    }
}

void CpuDescriptorAllocator::TryReleaseFreePages() noexcept {
    if (_freePageKeepCount == std::numeric_limits<uint32_t>::max()) {
        return;
    }
    size_t freeCount = 0;
    for (const auto& page : _pages) {
        freeCount += (page->Used == 0) ? 1 : 0;
    }
    while (freeCount > static_cast<size_t>(_freePageKeepCount) && !_pages.empty()) {
        size_t victim = _pages.size();
        for (size_t i = _pages.size(); i-- > 0;) {
            if (_pages[i]->Used == 0) {
                victim = i;
                break;
            }
        }
        if (victim == _pages.size()) {
            return;
        }
        const size_t last = _pages.size() - 1;
        if (victim != last) {
            std::swap(_pages[victim], _pages[last]);
            if (_hint == last) {
                _hint = victim;
            }
        }
        _pages.pop_back();
        if (_hint >= _pages.size()) {
            _hint = 0;
        }
        --freeCount;
    }
}

GpuDescriptorAllocator::GpuDescriptorAllocator(
    ID3D12Device* device,
    D3D12_DESCRIPTOR_HEAP_TYPE type,
    UINT size) noexcept
    : _device(device),
      _allocator(size) {
    _heap = make_unique<DescriptorHeap>(
        _device,
        D3D12_DESCRIPTOR_HEAP_DESC{
            type,
            static_cast<UINT>(size),
            D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
            0});
}

std::optional<GpuDescriptorAllocator::Allocation> GpuDescriptorAllocator::Allocate(UINT count) noexcept {
    const auto allocation = _allocator.Allocate(count);
    if (!allocation.has_value()) {
        return std::nullopt;
    }
    const auto& v = allocation.value();
    return std::make_optional(GpuDescriptorAllocator::Allocation{_heap.get(), static_cast<UINT>(v.Start), count, v});
}

void GpuDescriptorAllocator::Destroy(GpuDescriptorAllocator::Allocation allocation) noexcept {
    _allocator.Destroy(allocation.ParentAllocation);
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
    _cpuResAlloc = make_unique<CpuDescriptorAllocator>(_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 512, 1);
    _cpuRtvAlloc = make_unique<CpuDescriptorAllocator>(_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 128, 1);
    _cpuDsvAlloc = make_unique<CpuDescriptorAllocator>(_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 128, 1);
    _cpuSamplerAlloc = make_unique<CpuDescriptorAllocator>(_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 64, 1);
    _gpuResHeap = make_unique<GpuDescriptorAllocator>(_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1 << 16);
    _gpuSamplerHeap = make_unique<GpuDescriptorAllocator>(_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 1 << 8);
    _features.Init(_device.Get());
}

DeviceD3D12::~DeviceD3D12() noexcept {
    DestroyImpl();
}

bool DeviceD3D12::IsValid() const noexcept {
    return _device != nullptr && _dxgiAdapter != nullptr && _dxgiFactory != nullptr;
}

void DeviceD3D12::Destroy() noexcept {
    DestroyImpl();
}

void DeviceD3D12::DestroyImpl() noexcept {
    TryDrainValidationMessages();
    if (_isDebugLayerEnabled && _infoQueueCallbackCookie != 0) {
        ComPtr<ID3D12InfoQueue1> infoQueue1;
        if (SUCCEEDED(_device.As(&infoQueue1)) && infoQueue1 != nullptr) {
            infoQueue1->UnregisterMessageCallback(_infoQueueCallbackCookie);
        }
    }
    _infoQueueCallbackCookie = 0;
    _isDebugLayerEnabled = false;
    _logCallback = nullptr;
    _logUserData = nullptr;

    _cpuResAlloc = nullptr;
    _cpuRtvAlloc = nullptr;
    _cpuDsvAlloc = nullptr;
    _cpuSamplerAlloc = nullptr;
    _gpuResHeap = nullptr;
    _gpuSamplerHeap = nullptr;
    _mainAlloc = nullptr;
    _device = nullptr;
    _dxgiAdapter = nullptr;
    _dxgiFactory = nullptr;
}

Nullable<shared_ptr<DeviceD3D12>> CreateDevice(const D3D12DeviceDescriptor& desc) {
    uint32_t dxgiFactoryFlags = 0;
    if (desc.IsEnableDebugLayer) {
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(::D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
            debugController->EnableDebugLayer();
            dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
            if (desc.IsEnableGpuBasedValid) {
                ComPtr<ID3D12Debug1> debug1;
                if (SUCCEEDED(debugController.As(&debug1))) {
                    debug1->SetEnableGPUBasedValidation(true);
                } else {
                    RADRAY_WARN_LOG("ID3D12Debug::As<ID3D12Debug1> failed: {}", "cannot enable gpu based validation");
                }
            }
        } else {
            RADRAY_WARN_LOG("D3D12GetDebugInterface failed: {}", "cannot enable gpu based validation");
        }
    }
    ComPtr<IDXGIFactory4> dxgiFactory;
    if (HRESULT hr = ::CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(dxgiFactory.GetAddressOf()));
        FAILED(hr)) {
        RADRAY_ERR_LOG("CreateDXGIFactory2 failed: {} {}", GetErrorName(hr), hr);
        return nullptr;
    }
    ComPtr<IDXGIAdapter1> adapter;
    if (desc.AdapterIndex.has_value()) {
        uint32_t index = desc.AdapterIndex.value();
        if (HRESULT hr = dxgiFactory->EnumAdapters1(index, adapter.GetAddressOf());
            FAILED(hr)) {
            RADRAY_ERR_LOG("IDXGIFactory4::EnumAdapters1 failed: index={} {} {}", index, GetErrorName(hr), hr);
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
                    wstring s{adapDesc.Description};
                    RADRAY_INFO_LOG("d3d12 find adapter: {}", text_encoding::ToMultiByte(s).value_or("???"));
                }
            }
            for (
                auto adapterIndex = 0u;
                factory6->EnumAdapterByGpuPreference(adapterIndex, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(temp.GetAddressOf())) != DXGI_ERROR_NOT_FOUND;
                adapterIndex++) {
                DXGI_ADAPTER_DESC1 adapDesc;
                temp->GetDesc1(&adapDesc);
                if ((adapDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
                    if (SUCCEEDED(::D3D12CreateDevice(temp.Get(), D3D_FEATURE_LEVEL_11_0, _uuidof(ID3D12Device), nullptr))) {
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
                    FAILED(::D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, _uuidof(ID3D12Device), nullptr))) {
                    adapter = nullptr;
                }
            }
        }
    }
    if (adapter == nullptr) {
        RADRAY_ERR_LOG("d3d12 cannot find available adapter");
        return nullptr;
    }
    ComPtr<ID3D12Device> device;
    if (HRESULT hr = ::D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.GetAddressOf()));
        FAILED(hr)) {
        RADRAY_ERR_LOG("D3D12CreateDevice failed: {} {}", GetErrorName(hr), hr);
        return nullptr;
    }
    {
        DXGI_ADAPTER_DESC1 adapDesc{};
        adapter->GetDesc1(&adapDesc);
        wstring s{adapDesc.Description};
        RADRAY_INFO_LOG("d3d12 select adapter: {}", text_encoding::ToMultiByte(s).value_or("???"));
    }
    ComPtr<D3D12MA::Allocator> alloc;
    {
        D3D12MA::ALLOCATOR_DESC allocDesc{};
        allocDesc.Flags = D3D12MA::ALLOCATOR_FLAG_NONE;
        allocDesc.pDevice = device.Get();
        allocDesc.pAdapter = adapter.Get();
        allocDesc.Flags = D3D12MA::ALLOCATOR_FLAG_MSAA_TEXTURES_ALWAYS_COMMITTED;
        if (HRESULT hr = D3D12MA::CreateAllocator(&allocDesc, alloc.GetAddressOf());
            FAILED(hr)) {
            RADRAY_ERR_LOG("D3D12MA::CreateAllocator failed: {} {}", GetErrorName(hr), hr);
            return nullptr;
        }
    }
    auto result = make_shared<DeviceD3D12>(device, dxgiFactory, adapter, alloc);
    result->_isDebugLayerEnabled = desc.IsEnableDebugLayer;
    result->_logCallback = desc.LogCallback;
    result->_logUserData = desc.LogUserData;
    if (result->_isDebugLayerEnabled) {
        ComPtr<ID3D12InfoQueue1> infoQueue1;
        if (HRESULT hr = device.As(&infoQueue1);
            SUCCEEDED(hr) && infoQueue1 != nullptr) {
            DWORD callbackCookie = 0;
            if (HRESULT hr2 = infoQueue1->RegisterMessageCallback(
                    &_D3D12ValidationMessageCallback,
                    D3D12_MESSAGE_CALLBACK_FLAG_NONE,
                    result.get(),
                    &callbackCookie);
                SUCCEEDED(hr2)) {
                result->_infoQueueCallbackCookie = callbackCookie;
            } else {
                RADRAY_WARN_LOG("ID3D12InfoQueue1::RegisterMessageCallback failed: {} {}", GetErrorName(hr2), hr2);
            }
        } else {
            RADRAY_WARN_LOG("ID3D12Device::As<ID3D12InfoQueue1> failed: {} {} {}", GetErrorName(hr), hr, "pump validation messages at fixed points");
        }
    }
    DeviceDetail& detail = result->_detail;
    detail.CBufferAlignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
    detail.TextureDataPitchAlignment = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
    detail.MaxVertexInputBindings = D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT;
    detail.IsBindlessArraySupported = false;
    detail.MaxRayRecursionDepth = 0;
    detail.ShaderTableAlignment = D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT;
    detail.AccelerationStructureAlignment = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT;
    detail.AccelerationStructureScratchAlignment = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT;
    detail.IsRayTracingSupported = false;
    {
        DXGI_ADAPTER_DESC1 adapDesc{};
        if (HRESULT hr = adapter->GetDesc1(&adapDesc); SUCCEEDED(hr)) {
            wstring name{adapDesc.Description};
            detail.GpuName = text_encoding::ToMultiByte(name).value_or("???");
        }
    }
    {
        ComPtr<IDXGIAdapter3> adapter3;
        if (SUCCEEDED(adapter.As(&adapter3))) {
            DXGI_QUERY_VIDEO_MEMORY_INFO memInfo{};
            if (HRESULT hr = adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memInfo);
                SUCCEEDED(hr)) {
                detail.VramBudget = memInfo.Budget;
            }
        }
    }
    RADRAY_INFO_LOG("========== Feature ==========");
    {
        LARGE_INTEGER l;
        HRESULT hr = adapter->CheckInterfaceSupport(IID_IDXGIDevice, &l);
        if (SUCCEEDED(hr)) {
            const int64_t mask = 0xFFFF;
            auto quad = l.QuadPart;
            auto ver = fmt::format(
                "{}.{}.{}.{}",
                quad >> 48,
                (quad >> 32) & mask,
                (quad >> 16) & mask,
                quad & mask);
            RADRAY_INFO_LOG("d3d12 driver Version: {}", ver);
        } else {
            RADRAY_WARN_LOG("IDXGIAdapter::CheckInterfaceSupport failed: {} {}", GetErrorName(hr), hr);
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
                RADRAY_WARN_LOG("IDXGIFactory6::CheckFeatureSupport failed: {} {}", GetErrorName(hr), hr);
            }
        }
        RADRAY_INFO_LOG("Allow Tearing: {}", static_cast<bool>(allowTearing));
        result->_isAllowTearing = allowTearing;
    }
    const CD3DX12FeatureSupport& fs = result->_features;
    if (SUCCEEDED(fs.GetStatus())) {
        RADRAY_INFO_LOG("Feature Level: {}", fs.MaxSupportedFeatureLevel());
        RADRAY_INFO_LOG("Shader Model: {}", fs.HighestShaderModel());
        RADRAY_INFO_LOG("Resource Binding Tier: {}", fs.ResourceBindingTier());
        RADRAY_INFO_LOG("Resource Heap Tier: {}", fs.ResourceHeapTier());
        RADRAY_INFO_LOG("TBR: {}", static_cast<bool>(fs.TileBasedRenderer()));
        detail.IsUMA = static_cast<bool>(fs.UMA());
        RADRAY_INFO_LOG("UMA: {}", detail.IsUMA);
        detail.IsBindlessArraySupported =
            fs.ResourceBindingTier() >= D3D12_RESOURCE_BINDING_TIER_3 &&
            fs.HighestShaderModel() >= D3D_SHADER_MODEL_6_0;
        RADRAY_INFO_LOG("Bindless Array: {}", detail.IsBindlessArraySupported);
    } else {
        RADRAY_WARN_LOG("CD3DX12FeatureSupport::GetStatus failed");
    }
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
        if (HRESULT hr = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5));
            SUCCEEDED(hr)) {
            detail.IsRayTracingSupported = options5.RaytracingTier != D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
            detail.MaxRayRecursionDepth = D3D12_RAYTRACING_MAX_DECLARABLE_TRACE_RECURSION_DEPTH;
            RADRAY_INFO_LOG("Ray Tracing: {} (tier={})", detail.IsRayTracingSupported, options5.RaytracingTier);
        } else {
            RADRAY_WARN_LOG("ID3D12Device::CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5) failed: {} {}", GetErrorName(hr), hr);
        }
    }
    RADRAY_INFO_LOG("=============================");
    return result;
}

DeviceDetail DeviceD3D12::GetDetail() const noexcept {
    return _detail;
}

Nullable<CommandQueue*> DeviceD3D12::GetCommandQueue(QueueType type, uint32_t slot) noexcept {
    uint32_t index = static_cast<size_t>(type);
    RADRAY_ASSERT(index >= 0 && index < 3);
    auto& queues = _queues[index];
    if (queues.size() <= slot) {
        queues.reserve(slot + 1);
        for (size_t i = queues.size(); i <= slot; i++) {
            queues.emplace_back(unique_ptr<CmdQueueD3D12>{nullptr});
        }
    }
    unique_ptr<CmdQueueD3D12>& q = queues[slot];
    if (q == nullptr) {
        auto fenceOpt = CreateFenceD3D12(0);
        if (fenceOpt == nullptr) {
            return nullptr;
        }
        ComPtr<ID3D12CommandQueue> queue;
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type = MapType(type);
        if (HRESULT hr = _device->CreateCommandQueue(&desc, IID_PPV_ARGS(queue.GetAddressOf()));
            SUCCEEDED(hr)) {
            auto f = fenceOpt.Release();
            auto ins = make_unique<CmdQueueD3D12>(this, std::move(queue), desc.Type, std::move(f));
            string debugName = fmt::format("Queue-{}-{}", type, slot);
            SetObjectName(debugName, ins->_queue.Get());
            q = std::move(ins);
        } else {
            RADRAY_ERR_LOG("ID3D12Device::CreateCommandQueue failed: {} {}", GetErrorName(hr), hr);
        }
    }
    return q->IsValid() ? q.get() : nullptr;
}

Nullable<unique_ptr<CommandBuffer>> DeviceD3D12::CreateCommandBuffer(CommandQueue* queue_) noexcept {
    auto queue = CastD3D12Object(queue_);
    ComPtr<ID3D12CommandAllocator> alloc;
    if (HRESULT hr = _device->CreateCommandAllocator(queue->_type, IID_PPV_ARGS(alloc.GetAddressOf()));
        FAILED(hr)) {
        RADRAY_ERR_LOG("ID3D12Device::CreateCommandAllocator failed: {} {}", GetErrorName(hr), hr);
        return nullptr;
    }
    ComPtr<ID3D12GraphicsCommandList> list;
    if (HRESULT hr = _device->CreateCommandList(0, queue->_type, alloc.Get(), nullptr, IID_PPV_ARGS(list.GetAddressOf()));
        SUCCEEDED(hr)) {
        if (FAILED(list->Close())) {
            RADRAY_ERR_LOG("ID3D12GraphicsCommandList::Close failed: {} {}", GetErrorName(hr), hr);
            return nullptr;
        }
        return make_unique<CmdListD3D12>(
            this,
            std::move(alloc),
            std::move(list),
            queue->_type);
    } else {
        RADRAY_ERR_LOG("ID3D12Device::CreateCommandList failed: {} {}", GetErrorName(hr), hr);
        return nullptr;
    }
}

Nullable<unique_ptr<Fence>> DeviceD3D12::CreateFence() noexcept {
    return this->CreateFenceD3D12(0);
}

Nullable<unique_ptr<SwapChain>> DeviceD3D12::CreateSwapChain(const SwapChainDescriptor& desc) noexcept {
    // https://learn.microsoft.com/zh-cn/windows/win32/api/dxgi1_2/ns-dxgi1_2-dxgi_swap_chain_desc1
    DXGI_SWAP_CHAIN_DESC1 scDesc{};
    scDesc.Width = desc.Width;
    scDesc.Height = desc.Height;
    scDesc.Format = MapType(desc.Format);
    if (scDesc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT &&
        scDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
        scDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
        scDesc.Format != DXGI_FORMAT_R10G10B10A2_UNORM) {
        RADRAY_ERR_LOG("IDXGISwapChain format not supported: {}", desc.Format);
        return nullptr;
    }
    scDesc.Stereo = false;
    scDesc.SampleDesc.Count = 1;
    scDesc.SampleDesc.Quality = 0;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.BufferCount = desc.BackBufferCount;
    if (scDesc.BufferCount < 2 || scDesc.BufferCount > 16) {
        RADRAY_ERR_LOG("IDXGISwapChain BufferCount must >= 2 and <= 16: {}", desc.BackBufferCount);
        return nullptr;
    }
    scDesc.Scaling = DXGI_SCALING_STRETCH;
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    scDesc.Flags = 0;
    scDesc.Flags |= _isAllowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    scDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    auto queue = CastD3D12Object(desc.PresentQueue);
    HWND hwnd = std::bit_cast<HWND>(desc.NativeHandler);
    ComPtr<IDXGISwapChain1> temp;
    if (HRESULT hr = _dxgiFactory->CreateSwapChainForHwnd(queue->_queue.Get(), hwnd, &scDesc, nullptr, nullptr, temp.GetAddressOf());
        FAILED(hr)) {
        RADRAY_ERR_LOG("IDXGIFactory::CreateSwapChainForHwnd failed: {} {}", GetErrorName(hr), hr);
        return nullptr;
    }
    if (HRESULT hr = _dxgiFactory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);  // 阻止 Alt + Enter 进全屏
        FAILED(hr)) {
        RADRAY_WARN_LOG("IDXGIFactory::MakeWindowAssociation failed: {} {}", GetErrorName(hr), hr);
    }
    ComPtr<IDXGISwapChain3> swapchain;
    if (HRESULT hr = temp->QueryInterface(IID_PPV_ARGS(swapchain.GetAddressOf()));
        FAILED(hr)) {
        RADRAY_ERR_LOG("IDXGISwapChain1::QueryInterface failed: {} {}", GetErrorName(hr), hr);
        return nullptr;
    }
    if (desc.PresentMode != PresentMode::FIFO) {
        if (HRESULT hr = swapchain->SetMaximumFrameLatency(1); FAILED(hr)) {
            RADRAY_ERR_LOG("IDXGISwapChain3::SetMaximumFrameLatency(1) failed: {} {}", GetErrorName(hr), hr);
            return nullptr;
        }
    }
    auto result = make_unique<SwapChainD3D12>(this, swapchain, desc);
    result->_frames.reserve(scDesc.BufferCount);
    result->_frameLatencyEvent = swapchain->GetFrameLatencyWaitableObject();
    for (size_t i = 0; i < scDesc.BufferCount; i++) {
        auto& frame = result->_frames.emplace_back();
        ComPtr<ID3D12Resource> rt;
        if (HRESULT hr = swapchain->GetBuffer((UINT)i, IID_PPV_ARGS(rt.GetAddressOf()));
            FAILED(hr)) {
            RADRAY_ERR_LOG("IDXGISwapChain1::GetBuffer failed: {} {}", GetErrorName(hr), hr);
            return nullptr;
        }
        auto name = fmt::format("SwapChain_BackBuffer_{}", i);
        frame.image = make_unique<TextureD3D12>(this, std::move(rt), ComPtr<D3D12MA::Allocation>{});
        frame.image->_name = name;
        frame.image->_dimension = TextureDimension::Dim2D;
        frame.image->_format = desc.Format;
        frame.image->_memory = MemoryType::Device;
        frame.image->_usage = TextureUse::UNKNOWN | TextureUse::CopySource;
        if (frame.image->_rawDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) {
            frame.image->_usage |= TextureUse::RenderTarget;
        }
        if (frame.image->_rawDesc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) {
            frame.image->_usage |= TextureUse::Resource;
        }
        frame.image->_hints = ResourceHint::External;
        SetObjectName(name, frame.image->_tex.Get());
    }
    return result;
}

Nullable<unique_ptr<Buffer>> DeviceD3D12::CreateBuffer(const BufferDescriptor& desc_) noexcept {
    BufferDescriptor desc = desc_;
    const uint64_t logicalSize = desc.Size;
    D3D12_RESOURCE_DESC resDesc{};
    resDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    // Alignment must be 64KB (D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT) or 0, which is effectively 64KB.
    // https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_resource_desc
    resDesc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    // D3D12 要求 cbuffer 是 256 字节对齐
    // https://github.com/d3dcoder/d3d12book/blob/master/Common/d3dUtil.h#L99
    uint64_t allocSize = desc.Usage.HasFlag(BufferUse::CBuffer)
                             ? Align(desc.Size, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT)
                             : desc.Size;
    resDesc.Width = allocSize;
    resDesc.Height = 1;
    resDesc.DepthOrArraySize = 1;
    resDesc.MipLevels = 1;
    resDesc.Format = DXGI_FORMAT_UNKNOWN;
    resDesc.SampleDesc.Count = 1;
    resDesc.SampleDesc.Quality = 0;
    resDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    if (desc.Usage.HasFlag(BufferUse::UnorderedAccess) ||
        desc.Usage.HasFlag(BufferUse::AccelerationStructure) ||
        desc.Usage.HasFlag(BufferUse::Scratch)) {
        resDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }
    if (desc.Memory == MemoryType::ReadBack) {
        resDesc.Flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    }
    UINT64 paddedSize;
    _device->GetCopyableFootprints(&resDesc, 0, 1, 0, nullptr, nullptr, nullptr, &paddedSize);
    if (paddedSize != UINT64_MAX) {
        allocSize = paddedSize;
        resDesc.Width = paddedSize;
    }
    D3D12_RESOURCE_STATES rawInitState = MapMemoryTypeToResourceState(desc.Memory);
    D3D12MA::ALLOCATION_DESC allocDesc{};
    allocDesc.HeapType = MapType(desc.Memory);
    allocDesc.Flags = D3D12MA::ALLOCATION_FLAG_NONE;
    if (desc.Hints.HasFlag(ResourceHint::Dedicated)) {
        allocDesc.Flags = static_cast<D3D12MA::ALLOCATION_FLAGS>(allocDesc.Flags | D3D12MA::ALLOCATION_FLAG_COMMITTED);
    }
    ComPtr<ID3D12Resource> buffer;
    ComPtr<D3D12MA::Allocation> allocRes;
    /**
     * https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_heap_type
     * https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_resource_flags
     * upload 和 readback 只能以 GENERIC_READ 和 COPY_DEST 状态创建且不可转换。而 UAV 需要 UNORDERED_ACCESS 状态，需求冲突
     * 利用 Custom 堆开启 L0 系统内存并设置 WRITE_COMBINE 可以绕开 Abstract 堆的限制
     * L0 系统内存既可由 CPU 读写，NUMA 可由设备通过 PCIe 读写，UMA 更是能直接读写，可以同时满足 CPU 想 Mapped 读写，GPU 想 UAV 读写的需求
     * WRITE_COMBINE 设置后 CPU 写入会合并到缓冲区批量提交，NUMA 通过 PCIe 上传到设备。但 CPU 读取会变慢
     */
    if (allocDesc.HeapType != D3D12_HEAP_TYPE_DEFAULT && (resDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) {
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
                &resDesc,
                rawInitState,
                nullptr,
                IID_PPV_ARGS(buffer.GetAddressOf()));
            FAILED(hr)) {
            RADRAY_ERR_LOG("ID3D12Device::CreateCommittedResource failed: {} {}", GetErrorName(hr), hr);
            return nullptr;
        }
    } else {
        if (HRESULT hr = _mainAlloc->CreateResource(
                &allocDesc,
                &resDesc,
                rawInitState,
                nullptr,
                allocRes.GetAddressOf(),
                IID_PPV_ARGS(buffer.GetAddressOf()));
            FAILED(hr)) {
            RADRAY_ERR_LOG("D3D12MA::Allocator::CreateResource failed: {} {}", GetErrorName(hr), hr);
            return nullptr;
        }
    }
    auto result = make_unique<BufferD3D12>(this, std::move(buffer), std::move(allocRes));
    result->_memory = desc.Memory;
    result->_usage = desc.Usage;
    result->_hints = desc.Hints;
    result->_reqSize = logicalSize;
    return result;
}

Nullable<unique_ptr<BufferView>> DeviceD3D12::CreateBufferView(const BufferViewDescriptor& desc) noexcept {
    if (desc.Target == nullptr) {
        RADRAY_ERR_LOG("BufferViewDescriptor.Target is null");
        return nullptr;
    }
    auto buf = CastD3D12Object(desc.Target);
    CpuDescriptorAllocator* heap = _cpuResAlloc.get();
    CpuDescriptorHeapViewRAII heapView{};
    DXGI_FORMAT dxgiFormat = DXGI_FORMAT_UNKNOWN;

    if (desc.Usage == BufferViewUsage::CBuffer) {
        if (desc.Range.Offset % D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT != 0) {
            RADRAY_ERR_LOG("d3d12 constant buffer view offset must be {}-byte aligned", D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
            return nullptr;
        }
        uint64_t alignedSize = Align(desc.Range.Size, uint64_t(D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT));
        auto heapViewOpt = heap->Allocate(1);
        if (!heapViewOpt.has_value()) {
            RADRAY_ERR_LOG("CpuDescriptorAllocator::Allocate failed: {}", "cannot allocate CBV descriptor");
            return nullptr;
        }
        heapView = {heap, heapViewOpt.value()};
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc{};
        cbvDesc.BufferLocation = buf->_gpuAddr + desc.Range.Offset;
        cbvDesc.SizeInBytes = static_cast<UINT>(alignedSize);
        heapView.GetHeap()->Create(cbvDesc, heapView.GetStart());
        dxgiFormat = DXGI_FORMAT_UNKNOWN;
    } else if (desc.Usage == BufferViewUsage::ReadOnlyStorage || desc.Usage == BufferViewUsage::TexelReadOnly) {
        auto heapViewOpt = heap->Allocate(1);
        if (!heapViewOpt.has_value()) {
            RADRAY_ERR_LOG("CpuDescriptorAllocator::Allocate failed: {}", "cannot allocate SRV descriptor");
            return nullptr;
        }
        heapView = {heap, heapViewOpt.value()};
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = MapType(desc.Format);
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (desc.Usage == BufferViewUsage::TexelReadOnly) {
            const auto bpp = GetTextureFormatBytesPerPixel(desc.Format);
            if (desc.Range.Offset % bpp != 0 || desc.Range.Size % bpp != 0) {
                RADRAY_ERR_LOG("d3d12 texel buffer view offset/size must align to format bytes");
                return nullptr;
            }
            srvDesc.Buffer.FirstElement = static_cast<UINT>(desc.Range.Offset / bpp);
            srvDesc.Buffer.NumElements = static_cast<UINT>(desc.Range.Size / bpp);
            srvDesc.Buffer.StructureByteStride = 0;
        } else {
            if (desc.Range.Offset % desc.Stride != 0 || desc.Range.Size % desc.Stride != 0) {
                RADRAY_ERR_LOG("d3d12 structured buffer view offset/size must align to stride");
                return nullptr;
            }
            srvDesc.Buffer.FirstElement = static_cast<UINT>(desc.Range.Offset / desc.Stride);
            srvDesc.Buffer.NumElements = static_cast<UINT>(desc.Range.Size / desc.Stride);
            srvDesc.Buffer.StructureByteStride = desc.Stride;
        }
        srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
        heapView.GetHeap()->Create(buf->_buf.Get(), srvDesc, heapView.GetStart());
        dxgiFormat = srvDesc.Format;
    } else if (desc.Usage == BufferViewUsage::ReadWriteStorage || desc.Usage == BufferViewUsage::TexelReadWrite) {
        auto heapViewOpt = heap->Allocate(1);
        if (!heapViewOpt.has_value()) {
            RADRAY_ERR_LOG("CpuDescriptorAllocator::Allocate failed: {}", "cannot allocate UAV descriptor");
            return nullptr;
        }
        heapView = {heap, heapViewOpt.value()};
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = MapType(desc.Format);
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        if (desc.Usage == BufferViewUsage::TexelReadWrite) {
            const auto bpp = GetTextureFormatBytesPerPixel(desc.Format);
            if (desc.Range.Offset % bpp != 0 || desc.Range.Size % bpp != 0) {
                RADRAY_ERR_LOG("d3d12 texel buffer view offset/size must align to format bytes");
                return nullptr;
            }
            uavDesc.Buffer.FirstElement = static_cast<UINT>(desc.Range.Offset / bpp);
            uavDesc.Buffer.NumElements = static_cast<UINT>(desc.Range.Size / bpp);
            uavDesc.Buffer.StructureByteStride = 0;
        } else {
            if (desc.Range.Offset % desc.Stride != 0 || desc.Range.Size % desc.Stride != 0) {
                RADRAY_ERR_LOG("d3d12 structured buffer view offset/size must align to stride");
                return nullptr;
            }
            uavDesc.Buffer.FirstElement = static_cast<UINT>(desc.Range.Offset / desc.Stride);
            uavDesc.Buffer.NumElements = static_cast<UINT>(desc.Range.Size / desc.Stride);
            uavDesc.Buffer.StructureByteStride = desc.Stride;
        }
        uavDesc.Buffer.CounterOffsetInBytes = 0;
        uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
        heapView.GetHeap()->Create(buf->_buf.Get(), uavDesc, heapView.GetStart());
        dxgiFormat = uavDesc.Format;
    } else {
        RADRAY_ERR_LOG("invalid buffer view type");
        return nullptr;
    }
    auto result = make_unique<BufferViewD3D12>(this, buf, std::move(heapView));
    result->_desc = desc;
    result->_rawFormat = dxgiFormat;
    return result;
}

Nullable<unique_ptr<Texture>> DeviceD3D12::CreateTexture(const TextureDescriptor& desc_) noexcept {
    TextureDescriptor desc = desc_;
    DXGI_FORMAT rawFormat = MapType(desc.Format);
    D3D12_RESOURCE_DESC resDesc{};
    resDesc.Dimension = MapType(desc.Dim);
    resDesc.Alignment = desc.SampleCount > 1 ? D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT : 0;
    resDesc.Width = desc.Width;
    resDesc.Height = desc.Height;
    resDesc.DepthOrArraySize = static_cast<UINT16>(desc.DepthOrArraySize);
    resDesc.MipLevels = static_cast<UINT16>(desc.MipLevels);
    resDesc.Format = FormatToTypeless(rawFormat);
    /**
     * https://learn.microsoft.com/en-us/windows/win32/api/dxgicommon/ns-dxgicommon-dxgi_sample_desc
     * https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_feature_data_multisample_quality_levels
     * https://learn.microsoft.com/en-us/windows/win32/direct3d11/d3d10-graphics-programming-guide-rasterizer-stage-rules#multisample-anti-aliasing-rasterization-rules
     * 无 MSAA 时 quality 必须为 0 或 1
     * DXGI_STANDARD_MULTISAMPLE_QUALITY_PATTERN 使用标准的 MSAA 模式, 避免厂商差异
     * 使用前需要调用 CheckFeatureSupport 确定 Format 支持的 quality，或直接用 DXGI_STANDARD_MULTISAMPLE_QUALITY_PATTERN
     */
    resDesc.SampleDesc.Count = desc.SampleCount ? desc.SampleCount : 1;
    resDesc.SampleDesc.Quality = 0;
    resDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    if (desc.Usage.HasFlag(TextureUse::UnorderedAccess)) {
        resDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }
    if (desc.Usage.HasFlag(TextureUse::RenderTarget)) {
        resDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    }
    if (desc.Usage.HasFlag(TextureUse::DepthStencilRead) || desc.Usage.HasFlag(TextureUse::DepthStencilWrite)) {
        resDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    }
    D3D12_RESOURCE_STATES startState = D3D12_RESOURCE_STATE_COMMON;
    // D3D12_CLEAR_VALUE clear{};
    // clear.Format = rawFormat;
    // if (auto ccv = std::get_if<ColorClearValue>(&clearValue)) {
    //     clear.Color[0] = ccv->Value[0];
    //     clear.Color[1] = ccv->Value[1];
    //     clear.Color[2] = ccv->Value[2];
    //     clear.Color[3] = ccv->Value[3];
    // } else if (auto dcv = std::get_if<DepthStencilClearValue>(&clearValue)) {
    //     clear.DepthStencil.Depth = dcv->Depth;
    //     clear.DepthStencil.Stencil = (UINT8)dcv->Stencil;
    // }
    const D3D12_CLEAR_VALUE* clearPtr = nullptr;
    // if ((resDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) || (resDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) {
    //     clearPtr = &clear;
    // }
    D3D12MA::ALLOCATION_DESC allocDesc{};
    allocDesc.HeapType = MapType(desc.Memory);
    if (desc.Hints.HasFlag(ResourceHint::Dedicated)) {
        allocDesc.Flags = static_cast<D3D12MA::ALLOCATION_FLAGS>(allocDesc.Flags | D3D12MA::ALLOCATION_FLAG_COMMITTED);
    }
    ComPtr<ID3D12Resource> texture;
    ComPtr<D3D12MA::Allocation> allocRes;
    if (HRESULT hr = _mainAlloc->CreateResource(
            &allocDesc,
            &resDesc,
            startState,
            clearPtr,
            allocRes.GetAddressOf(),
            IID_PPV_ARGS(texture.GetAddressOf()));
        FAILED(hr)) {
        RADRAY_ERR_LOG("D3D12MA::Allocator::CreateResource failed: {} {}", GetErrorName(hr), hr);
        return nullptr;
    }
    auto result = make_unique<TextureD3D12>(this, std::move(texture), std::move(allocRes));
    result->_dimension = desc.Dim;
    result->_format = desc.Format;
    result->_memory = desc.Memory;
    result->_usage = desc.Usage;
    result->_hints = desc.Hints;
    return result;
}

Nullable<unique_ptr<TextureView>> DeviceD3D12::CreateTextureView(const TextureViewDescriptor& desc) noexcept {
    // https://learn.microsoft.com/zh-cn/windows/win32/direct3d12/subresources
    // 三种 slice: mip 横向, array 纵向, plane 看起来更像是通道
    auto tex = CastD3D12Object(desc.Target);
    CpuDescriptorHeapViewRAII heapView{};
    DXGI_FORMAT dxgiFormat;
    if (desc.Usage == TextureViewUsage::Resource) {
        {
            auto heap = _cpuResAlloc.get();
            auto heapViewOpt = heap->Allocate(1);
            if (!heapViewOpt.has_value()) {
                RADRAY_ERR_LOG("CpuDescriptorAllocator::Allocate failed: {}", "cannot allocate CBV descriptor");
                return nullptr;
            }
            heapView = {heap, heapViewOpt.value()};
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = MapShaderResourceType(desc.Format);
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        switch (desc.Dim) {
            case TextureDimension::Dim1D:
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
                srvDesc.Texture1D.MostDetailedMip = desc.Range.BaseMipLevel;
                srvDesc.Texture1D.MipLevels = desc.Range.MipLevelCount == SubresourceRange::All ? static_cast<UINT>(-1) : desc.Range.MipLevelCount;
                break;
            case TextureDimension::Dim1DArray:
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
                srvDesc.Texture1DArray.MostDetailedMip = desc.Range.BaseMipLevel;
                srvDesc.Texture1DArray.MipLevels = desc.Range.MipLevelCount == SubresourceRange::All ? static_cast<UINT>(-1) : desc.Range.MipLevelCount;
                srvDesc.Texture1DArray.FirstArraySlice = desc.Range.BaseArrayLayer;
                srvDesc.Texture1DArray.ArraySize = desc.Range.ArrayLayerCount == SubresourceRange::All ? static_cast<UINT>(-1) : desc.Range.ArrayLayerCount;
                break;
            case TextureDimension::Dim2D:
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srvDesc.Texture2D.MostDetailedMip = desc.Range.BaseMipLevel;
                srvDesc.Texture2D.MipLevels = desc.Range.MipLevelCount == SubresourceRange::All ? static_cast<UINT>(-1) : desc.Range.MipLevelCount;
                srvDesc.Texture2D.PlaneSlice = 0;
                break;
            case TextureDimension::Dim2DArray:
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                srvDesc.Texture2DArray.MostDetailedMip = desc.Range.BaseMipLevel;
                srvDesc.Texture2DArray.MipLevels = desc.Range.MipLevelCount == SubresourceRange::All ? static_cast<UINT>(-1) : desc.Range.MipLevelCount;
                srvDesc.Texture2DArray.FirstArraySlice = desc.Range.BaseArrayLayer;
                srvDesc.Texture2DArray.ArraySize = desc.Range.ArrayLayerCount == SubresourceRange::All ? static_cast<UINT>(-1) : desc.Range.ArrayLayerCount;
                srvDesc.Texture2DArray.PlaneSlice = 0;
                break;
            case TextureDimension::Dim3D:
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
                srvDesc.Texture3D.MostDetailedMip = desc.Range.BaseMipLevel;
                srvDesc.Texture3D.MipLevels = desc.Range.MipLevelCount == SubresourceRange::All ? static_cast<UINT>(-1) : desc.Range.MipLevelCount;
                break;
            case TextureDimension::Cube:
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                srvDesc.TextureCube.MostDetailedMip = desc.Range.BaseMipLevel;
                srvDesc.TextureCube.MipLevels = desc.Range.MipLevelCount == SubresourceRange::All ? static_cast<UINT>(-1) : desc.Range.MipLevelCount;
                break;
            case TextureDimension::CubeArray:
                // https://learn.microsoft.com/zh-cn/windows/win32/api/d3d12/ns-d3d12-d3d12_texcube_array_srv
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
                srvDesc.TextureCubeArray.MostDetailedMip = desc.Range.BaseMipLevel;
                srvDesc.TextureCubeArray.MipLevels = desc.Range.MipLevelCount == SubresourceRange::All ? static_cast<UINT>(-1) : desc.Range.MipLevelCount;
                srvDesc.TextureCubeArray.First2DArrayFace = desc.Range.BaseArrayLayer;
                {
                    const uint32_t layers = desc.Range.ArrayLayerCount == SubresourceRange::All ? tex->_rawDesc.DepthOrArraySize - desc.Range.BaseArrayLayer : desc.Range.ArrayLayerCount;
                    if ((layers % 6) != 0) {
                        RADRAY_ERR_LOG("d3d12 cube array texture view layer count must be a multiple of 6");
                        return nullptr;
                    }
                    srvDesc.TextureCubeArray.NumCubes = layers / 6;
                }
                break;
            default:
                RADRAY_ERR_LOG("d3d12 invalid texture view dimension: {}", desc.Dim);
                return nullptr;
        }
        heapView.GetHeap()->Create(tex->_tex.Get(), srvDesc, heapView.GetStart());
        dxgiFormat = srvDesc.Format;
    } else if (desc.Usage == TextureViewUsage::RenderTarget) {
        {
            auto heap = _cpuRtvAlloc.get();
            auto heapViewOpt = heap->Allocate(1);
            if (!heapViewOpt.has_value()) {
                RADRAY_ERR_LOG("CpuDescriptorAllocator::Allocate failed: {}", "cannot allocate RTV descriptor");
                return nullptr;
            }
            heapView = {heap, heapViewOpt.value()};
        }
        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
        rtvDesc.Format = MapType(desc.Format);
        switch (desc.Dim) {
            case TextureDimension::Dim1D:
                rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1D;
                rtvDesc.Texture1D.MipSlice = desc.Range.BaseMipLevel;
                break;
            case TextureDimension::Dim1DArray:
                rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1DARRAY;
                rtvDesc.Texture1DArray.MipSlice = desc.Range.BaseMipLevel;
                rtvDesc.Texture1DArray.FirstArraySlice = desc.Range.BaseArrayLayer;
                rtvDesc.Texture1DArray.ArraySize = desc.Range.ArrayLayerCount == SubresourceRange::All ? static_cast<UINT>(-1) : desc.Range.ArrayLayerCount;
                break;
            case TextureDimension::Dim2D:
                rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
                rtvDesc.Texture2D.MipSlice = desc.Range.BaseMipLevel;
                rtvDesc.Texture2D.PlaneSlice = 0;
                break;
            case TextureDimension::Dim2DArray:
                rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                rtvDesc.Texture2DArray.MipSlice = desc.Range.BaseMipLevel;
                rtvDesc.Texture2DArray.FirstArraySlice = desc.Range.BaseArrayLayer;
                rtvDesc.Texture2DArray.ArraySize = desc.Range.ArrayLayerCount == SubresourceRange::All ? static_cast<UINT>(-1) : desc.Range.ArrayLayerCount;
                rtvDesc.Texture2DArray.PlaneSlice = 0;
                break;
            case TextureDimension::Dim3D:
                rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
                rtvDesc.Texture3D.MipSlice = desc.Range.BaseMipLevel;
                rtvDesc.Texture3D.FirstWSlice = desc.Range.BaseArrayLayer;
                rtvDesc.Texture3D.WSize = desc.Range.ArrayLayerCount == SubresourceRange::All ? static_cast<UINT>(-1) : desc.Range.ArrayLayerCount;
                break;
            default:
                RADRAY_ERR_LOG("d3d12 invalid texture view dimension: {}", desc.Dim);
                return nullptr;
        }
        heapView.GetHeap()->Create(tex->_tex.Get(), rtvDesc, heapView.GetStart());
        dxgiFormat = rtvDesc.Format;
    } else if (desc.Usage.HasFlag(TextureViewUsage::DepthRead) || desc.Usage.HasFlag(TextureViewUsage::DepthWrite)) {
        {
            auto heap = _cpuDsvAlloc.get();
            auto heapViewOpt = heap->Allocate(1);
            if (!heapViewOpt.has_value()) {
                RADRAY_ERR_LOG("CpuDescriptorAllocator::Allocate failed: {}", "cannot allocate DSV descriptor");
                return nullptr;
            }
            heapView = {heap, heapViewOpt.value()};
        }
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format = MapType(desc.Format);
        dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
        const bool readOnlyDepth = desc.Usage == TextureViewUsage::DepthRead;
        if (readOnlyDepth) {
            dsvDesc.Flags = static_cast<D3D12_DSV_FLAGS>(dsvDesc.Flags | D3D12_DSV_FLAG_READ_ONLY_DEPTH);
            if (IsStencilFormatDXGI(dsvDesc.Format)) {
                dsvDesc.Flags = static_cast<D3D12_DSV_FLAGS>(dsvDesc.Flags | D3D12_DSV_FLAG_READ_ONLY_STENCIL);
            }
        }
        switch (desc.Dim) {
            case TextureDimension::Dim1D:
                dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1D;
                dsvDesc.Texture1D.MipSlice = desc.Range.BaseMipLevel;
                break;
            case TextureDimension::Dim1DArray:
                dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1DARRAY;
                dsvDesc.Texture1DArray.MipSlice = desc.Range.BaseMipLevel;
                dsvDesc.Texture1DArray.FirstArraySlice = desc.Range.BaseArrayLayer;
                dsvDesc.Texture1DArray.ArraySize = desc.Range.ArrayLayerCount == SubresourceRange::All ? static_cast<UINT>(-1) : desc.Range.ArrayLayerCount;
                break;
            case TextureDimension::Dim2D:
                dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
                dsvDesc.Texture2D.MipSlice = desc.Range.BaseMipLevel;
                break;
            case TextureDimension::Dim2DArray:
                dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
                dsvDesc.Texture2DArray.MipSlice = desc.Range.BaseMipLevel;
                dsvDesc.Texture2DArray.FirstArraySlice = desc.Range.BaseArrayLayer;
                dsvDesc.Texture2DArray.ArraySize = desc.Range.ArrayLayerCount == SubresourceRange::All ? static_cast<UINT>(-1) : desc.Range.ArrayLayerCount;
                break;
            default:
                RADRAY_ERR_LOG("d3d12 invalid texture view dimension: {}", desc.Dim);
                return nullptr;
        }
        heapView.GetHeap()->Create(tex->_tex.Get(), dsvDesc, heapView.GetStart());
        dxgiFormat = dsvDesc.Format;
    } else if (desc.Usage == TextureViewUsage::UnorderedAccess) {
        {
            auto heap = _cpuResAlloc.get();
            auto heapViewOpt = heap->Allocate(1);
            if (!heapViewOpt.has_value()) {
                RADRAY_ERR_LOG("CpuDescriptorAllocator::Allocate failed: {}", "cannot allocate UAV descriptor");
                return nullptr;
            }
            heapView = {heap, heapViewOpt.value()};
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = MapShaderResourceType(desc.Format);
        switch (desc.Dim) {
            case TextureDimension::Dim1D:
                uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D;
                uavDesc.Texture1D.MipSlice = desc.Range.BaseMipLevel;
                break;
            case TextureDimension::Dim1DArray:
                uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1DARRAY;
                uavDesc.Texture1DArray.MipSlice = desc.Range.BaseMipLevel;
                uavDesc.Texture1DArray.FirstArraySlice = desc.Range.BaseArrayLayer;
                uavDesc.Texture1DArray.ArraySize = desc.Range.ArrayLayerCount == SubresourceRange::All ? static_cast<UINT>(-1) : desc.Range.ArrayLayerCount;
                break;
            case TextureDimension::Dim2D:
                uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                uavDesc.Texture2D.MipSlice = desc.Range.BaseMipLevel;
                uavDesc.Texture2D.PlaneSlice = 0;
                break;
            case TextureDimension::Dim2DArray:
                uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
                uavDesc.Texture2DArray.MipSlice = desc.Range.BaseMipLevel;
                uavDesc.Texture2DArray.FirstArraySlice = desc.Range.BaseArrayLayer;
                uavDesc.Texture2DArray.ArraySize = desc.Range.ArrayLayerCount == SubresourceRange::All ? static_cast<UINT>(-1) : desc.Range.ArrayLayerCount;
                uavDesc.Texture2DArray.PlaneSlice = 0;
                break;
            case TextureDimension::Dim3D:
                uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
                uavDesc.Texture3D.MipSlice = desc.Range.BaseMipLevel;
                uavDesc.Texture3D.FirstWSlice = desc.Range.BaseArrayLayer;
                uavDesc.Texture3D.WSize = desc.Range.ArrayLayerCount == SubresourceRange::All ? static_cast<UINT>(-1) : desc.Range.ArrayLayerCount;
                break;
            default:
                RADRAY_ERR_LOG("d3d12 invalid texture view dimension: {}", desc.Dim);
                return nullptr;
        }
        heapView.GetHeap()->Create(tex->_tex.Get(), uavDesc, heapView.GetStart());
        dxgiFormat = uavDesc.Format;
    } else {
        RADRAY_ERR_LOG("d3d12 invalid texture view usage: {}", desc.Usage);
        return nullptr;
    }
    auto result = make_unique<TextureViewD3D12>(this, tex, std::move(heapView));
    result->_desc = desc;
    result->_rawFormat = dxgiFormat;
    return result;
}

Nullable<unique_ptr<AccelerationStructureView>> DeviceD3D12::CreateAccelerationStructureView(const AccelerationStructureViewDescriptor& desc) noexcept {
    auto target = CastD3D12Object(desc.Target);
    auto heap = _cpuResAlloc.get();
    auto heapViewOpt = heap->Allocate(1);
    if (!heapViewOpt.has_value()) {
        RADRAY_ERR_LOG("CpuDescriptorAllocator::Allocate failed: {}", "cannot allocate RTAS SRV descriptor");
        return nullptr;
    }
    CpuDescriptorHeapViewRAII heapView{heap, heapViewOpt.value()};

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.RaytracingAccelerationStructure.Location = target->_gpuAddr;
    heapView.GetHeap()->Create(nullptr, srvDesc, heapView.GetStart());

    auto result = make_unique<AccelerationStructureViewD3D12>(this, target, std::move(heapView));
    result->_desc = desc;
    return result;
}

Nullable<unique_ptr<Shader>> DeviceD3D12::CreateShader(const ShaderDescriptor& desc) noexcept {
    if (desc.Category != ShaderBlobCategory::DXIL) {
        RADRAY_ERR_LOG("d3d12 only support DXIL shader blobs");
        return nullptr;
    }
    if (desc.Reflection.has_value() && !std::holds_alternative<HlslShaderDesc>(desc.Reflection.value())) {
        RADRAY_ERR_LOG("d3d12 shader only accepts hlsl reflection metadata");
        return nullptr;
    }
    return make_unique<Dxil>(desc.Source.begin(), desc.Source.end(), desc.Stages, desc.Reflection);
}

Nullable<unique_ptr<RootSignature>> DeviceD3D12::CreateRootSignature(const RootSignatureDescriptor& desc) noexcept {
    ShaderStages allStages = ShaderStage::UNKNOWN;
    for (Shader* shader : desc.Shaders) {
        if (shader == nullptr) {
            RADRAY_ERR_LOG("root signature shader is null");
            return nullptr;
        }
        auto reflectionOpt = shader->GetReflection();
        if (!reflectionOpt.HasValue() || reflectionOpt.Get() == nullptr) {
            RADRAY_ERR_LOG("d3d12 root signature requires reflection metadata on every shader");
            return nullptr;
        }
        if (!std::holds_alternative<HlslShaderDesc>(*reflectionOpt.Get())) {
            RADRAY_ERR_LOG("d3d12 root signature requires hlsl reflection metadata");
            return nullptr;
        }
        allStages |= shader->GetStages();
    }

    auto mergedOpt = BuildMergedBindingLayoutD3D12(desc.Shaders);
    if (!mergedOpt.has_value()) {
        return nullptr;
    }
    auto merged = std::move(mergedOpt.value());
    const auto allParameters = merged.Layout.GetParameters();
    if (merged.D3D12Parameters.size() != allParameters.size()) {
        RADRAY_ERR_LOG("internal error: merged parameter metadata size mismatch");
        return nullptr;
    }
    auto staticSamplerSelectionOpt = _SelectStaticSamplersD3D12(
        merged.Layout,
        merged.D3D12Parameters,
        desc.StaticSamplers);
    if (!staticSamplerSelectionOpt.has_value()) {
        return nullptr;
    }
    auto staticSamplerSelection = std::move(staticSamplerSelectionOpt.value());
    BindingLayout publicLayout{std::move(staticSamplerSelection.PublicParameters)};
    const auto parameters = allParameters;

    vector<RootSigD3D12::ParameterBindingInfo> parameterInfos(parameters.size());
    vector<vector<BindingParameterLayout>> descriptorSetLayouts(merged.SetCount);
    vector<BindlessSetLayout> bindlessSetLayouts{};
    vector<StaticSamplerLayout> staticSamplerLayouts = std::move(staticSamplerSelection.StaticSamplers);
    vector<PushConstantRange> pushConstantRanges{};
    vector<RootSigD3D12::DescriptorSetInfo> descriptorSets(merged.SetCount);
    for (uint32_t setIndex = 0; setIndex < merged.SetCount; ++setIndex) {
        descriptorSets[setIndex].SetIndex = DescriptorSetIndex{setIndex};
        descriptorSets[setIndex].RegisterSpace = setIndex;
    }
    vector<RootSigD3D12::BindlessSetInfo> bindlessSets{};
    vector<RootSigD3D12::DescriptorTableInfo> resourceTables{};
    vector<RootSigD3D12::DescriptorTableInfo> samplerTables{};
    vector<vector<D3D12_DESCRIPTOR_RANGE1>> resourceRanges{};
    vector<vector<D3D12_DESCRIPTOR_RANGE1>> samplerRanges{};
    vector<vector<D3D12_DESCRIPTOR_RANGE1>> bindlessRanges{};
    vector<D3D12_ROOT_PARAMETER1> rootParams{};
    rootParams.reserve(parameters.size());

    auto appendTables = [&](BindingParameterKind kind) noexcept -> bool {
        auto& tables = kind == BindingParameterKind::Sampler ? samplerTables : resourceTables;
        auto& ranges = kind == BindingParameterKind::Sampler ? samplerRanges : resourceRanges;
        uint32_t currentSet = std::numeric_limits<uint32_t>::max();
        size_t currentTableIndex = std::numeric_limits<size_t>::max();
        for (size_t i = 0; i < parameters.size(); ++i) {
            const auto& param = parameters[i];
            if (param.Kind != kind) {
                continue;
            }
            if (staticSamplerSelection.IsStaticParameter[i]) {
                const auto& abi = std::get<ResourceBindingAbi>(param.Abi);
                const auto& d3d12 = merged.D3D12Parameters[i];
                auto& info = parameterInfos[i];
                info.Kind = BindingParameterKind::Sampler;
                info.Type = abi.Type;
                info.SetIndex = abi.Set;
                info.BindingIndex = abi.Binding;
                info.ShaderRegister = d3d12.ShaderRegister;
                info.RegisterSpace = d3d12.RegisterSpace;
                info.DescriptorCount = abi.Count;
                info.IsReadOnly = abi.IsReadOnly;
                info.IsStaticSampler = true;
                info.Stages = param.Stages;
                continue;
            }
            const auto& abi = std::get<ResourceBindingAbi>(param.Abi);
            if (abi.IsBindless) {
                continue;
            }
            const auto& d3d12 = merged.D3D12Parameters[i];
            if (!d3d12.IsAvailable) {
                RADRAY_ERR_LOG("d3d12 lowering metadata is unavailable for '{}'", param.Name);
                return false;
            }
            const uint32_t setIndex = abi.Set.Value;
            if (setIndex >= descriptorSets.size()) {
                RADRAY_ERR_LOG("internal error: descriptor set {} is out of range", setIndex);
                return false;
            }
            if (currentTableIndex == std::numeric_limits<size_t>::max() || currentSet != setIndex) {
                currentSet = setIndex;
                currentTableIndex = tables.size();
                RootSigD3D12::DescriptorTableInfo table{};
                table.SetIndex = abi.Set;
                table.Kind = kind;
                table.RegisterSpace = d3d12.RegisterSpace;
                table.RootParameterIndex = static_cast<uint32_t>(rootParams.size());
                table.HeapOffset = 0;
                table.DescriptorCount = 0;
                table.Stages = ShaderStage::UNKNOWN;
                auto& setInfo = descriptorSets[setIndex];
                if ((setInfo.ResourceDescriptorCount > 0 || setInfo.SamplerDescriptorCount > 0) &&
                    setInfo.RegisterSpace != d3d12.RegisterSpace) {
                    RADRAY_ERR_LOG("d3d12 register space conflict for descriptor set {}", setIndex);
                    return false;
                }
                setInfo.RegisterSpace = d3d12.RegisterSpace;
                if (kind == BindingParameterKind::Sampler) {
                    setInfo.SamplerRootParameterIndex = table.RootParameterIndex;
                } else {
                    setInfo.ResourceRootParameterIndex = table.RootParameterIndex;
                }
                tables.push_back(table);
                ranges.emplace_back();
                rootParams.emplace_back();
            }

            auto rangeType = _MapDescriptorRangeType(abi.Type);
            if (!rangeType.has_value()) {
                RADRAY_ERR_LOG("unsupported descriptor range type for '{}'", param.Name);
                return false;
            }

            auto& table = tables[currentTableIndex];
            auto& rangeList = ranges[currentTableIndex];
            const uint32_t localOffset = table.DescriptorCount;
            rangeList.push_back(CD3DX12_DESCRIPTOR_RANGE1{
                rangeType.value(),
                abi.Count,
                d3d12.ShaderRegister,
                d3d12.RegisterSpace,
                D3D12_DESCRIPTOR_RANGE_FLAG_NONE,
                localOffset});
            table.DescriptorCount += abi.Count;
            table.Stages |= param.Stages;

            auto& info = parameterInfos[i];
            info.Kind = kind;
            info.Type = abi.Type;
            info.SetIndex = abi.Set;
            info.BindingIndex = abi.Binding;
            info.ShaderRegister = d3d12.ShaderRegister;
            info.RegisterSpace = d3d12.RegisterSpace;
            info.DescriptorCount = abi.Count;
            info.IsReadOnly = abi.IsReadOnly;
            info.RootParameterIndex = table.RootParameterIndex;
            info.DescriptorHeapOffset = localOffset;
            info.Stages = param.Stages;
            descriptorSetLayouts[setIndex].push_back(param);
            auto& setInfo = descriptorSets[setIndex];
            if (kind == BindingParameterKind::Sampler) {
                setInfo.SamplerDescriptorCount = table.DescriptorCount;
            } else {
                setInfo.ResourceDescriptorCount = table.DescriptorCount;
            }
        }

        for (size_t i = 0; i < tables.size(); ++i) {
            auto& rp = rootParams[tables[i].RootParameterIndex];
            CD3DX12_ROOT_PARAMETER1::InitAsDescriptorTable(
                rp,
                static_cast<UINT>(ranges[i].size()),
                ranges[i].empty() ? nullptr : ranges[i].data(),
                MapShaderStages(tables[i].Stages));
        }
        return true;
    };

    auto appendBindlessTables = [&]() noexcept -> bool {
        for (size_t i = 0; i < parameters.size(); ++i) {
            const auto& param = parameters[i];
            if (param.Kind != BindingParameterKind::Resource) {
                continue;
            }
            const auto& abi = std::get<ResourceBindingAbi>(param.Abi);
            if (!abi.IsBindless) {
                continue;
            }
            const auto& d3d12 = merged.D3D12Parameters[i];
            if (!d3d12.IsAvailable || !d3d12.IsBindless) {
                RADRAY_ERR_LOG("d3d12 bindless lowering metadata is unavailable for '{}'", param.Name);
                return false;
            }
            auto rangeType = _MapDescriptorRangeType(abi.Type);
            if (!rangeType.has_value() || rangeType.value() == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER) {
                RADRAY_ERR_LOG("unsupported d3d12 bindless descriptor type for '{}'", param.Name);
                return false;
            }
            bindlessRanges.push_back(vector<D3D12_DESCRIPTOR_RANGE1>{});
            auto& rangeList = bindlessRanges.back();
            rangeList.push_back(CD3DX12_DESCRIPTOR_RANGE1{
                rangeType.value(),
                UINT_MAX,
                d3d12.ShaderRegister,
                d3d12.RegisterSpace,
                D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE,
                0});

            auto& rp = rootParams.emplace_back();
            const uint32_t rootParameterIndex = static_cast<uint32_t>(rootParams.size() - 1);
            CD3DX12_ROOT_PARAMETER1::InitAsDescriptorTable(
                rp,
                static_cast<UINT>(rangeList.size()),
                rangeList.data(),
                MapShaderStages(param.Stages));

            auto& info = parameterInfos[i];
            info.Kind = BindingParameterKind::Resource;
            info.Type = abi.Type;
            info.SetIndex = abi.Set;
            info.BindingIndex = abi.Binding;
            info.ShaderRegister = d3d12.ShaderRegister;
            info.RegisterSpace = d3d12.RegisterSpace;
            info.DescriptorCount = 0;
            info.IsReadOnly = abi.IsReadOnly;
            info.IsBindless = true;
            info.BindlessSlotType = d3d12.BindlessSlotType;
            info.RootParameterIndex = rootParameterIndex;
            info.Stages = param.Stages;

            bindlessSetLayouts.push_back(BindlessSetLayout{
                .Name = param.Name,
                .Id = param.Id,
                .Set = abi.Set,
                .Binding = abi.Binding,
                .Type = abi.Type,
                .SlotType = d3d12.BindlessSlotType,
                .Stages = param.Stages,
            });
            bindlessSets.push_back(RootSigD3D12::BindlessSetInfo{
                .SetIndex = abi.Set,
                .Id = param.Id,
                .BindingIndex = abi.Binding,
                .ShaderRegister = d3d12.ShaderRegister,
                .RegisterSpace = d3d12.RegisterSpace,
                .Type = abi.Type,
                .SlotType = d3d12.BindlessSlotType,
                .RootParameterIndex = rootParameterIndex,
                .Stages = param.Stages,
            });
        }
        return true;
    };

    if (!appendTables(BindingParameterKind::Resource) ||
        !appendTables(BindingParameterKind::Sampler) ||
        !appendBindlessTables()) {
        return nullptr;
    }

    for (size_t i = 0; i < parameters.size(); ++i) {
        const auto& param = parameters[i];
        if (param.Kind != BindingParameterKind::PushConstant) {
            continue;
        }
        const auto& abi = std::get<PushConstantBindingAbi>(param.Abi);
        const auto& d3d12 = merged.D3D12Parameters[i];
        if (!d3d12.IsAvailable) {
            RADRAY_ERR_LOG("d3d12 push constant metadata is unavailable for '{}'", param.Name);
            return nullptr;
        }
        if (abi.Size == 0 || (abi.Offset % 4) != 0 || (abi.Size % 4) != 0) {
            RADRAY_ERR_LOG("d3d12 push constant '{}' must be 4-byte aligned and non-empty", param.Name);
            return nullptr;
        }
        auto& rp = rootParams.emplace_back();
        const uint32_t rootParameterIndex = static_cast<uint32_t>(rootParams.size() - 1);
        CD3DX12_ROOT_PARAMETER1::InitAsConstants(
            rp,
            abi.Size / 4,
            d3d12.ShaderRegister,
            d3d12.RegisterSpace,
            MapShaderStages(param.Stages));

        auto& info = parameterInfos[i];
        info.Kind = BindingParameterKind::PushConstant;
        info.RootParameterIndex = rootParameterIndex;
        info.ShaderRegister = d3d12.ShaderRegister;
        info.RegisterSpace = d3d12.RegisterSpace;
        info.PushConstantOffset = abi.Offset;
        info.PushConstantSize = abi.Size;
        info.Stages = param.Stages;
        pushConstantRanges.push_back(PushConstantRange{
            .Name = param.Name,
            .Id = param.Id,
            .Stages = param.Stages,
            .Offset = abi.Offset,
            .Size = abi.Size,
        });
    }
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC versionDesc{};
    versionDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    versionDesc.Desc_1_1 = {};
    versionDesc.Desc_1_1.NumParameters = static_cast<UINT>(rootParams.size());
    versionDesc.Desc_1_1.pParameters = rootParams.empty() ? nullptr : rootParams.data();
    versionDesc.Desc_1_1.NumStaticSamplers = static_cast<UINT>(staticSamplerSelection.RawStaticSamplers.size());
    versionDesc.Desc_1_1.pStaticSamplers = staticSamplerSelection.RawStaticSamplers.empty()
                                               ? nullptr
                                               : staticSamplerSelection.RawStaticSamplers.data();
    versionDesc.Desc_1_1.Flags =
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_AMPLIFICATION_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_MESH_SHADER_ROOT_ACCESS;
    if (!allStages.HasFlag(ShaderStage::Vertex)) {
        versionDesc.Desc_1_1.Flags = static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(
            versionDesc.Desc_1_1.Flags | D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS);
    }
    if (!allStages.HasFlag(ShaderStage::Pixel)) {
        versionDesc.Desc_1_1.Flags = static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(
            versionDesc.Desc_1_1.Flags | D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS);
    }

    ComPtr<ID3DBlob> rootSigBlob{};
    ComPtr<ID3DBlob> errBlob{};
    if (HRESULT hr = ::D3D12SerializeVersionedRootSignature(&versionDesc, rootSigBlob.GetAddressOf(), errBlob.GetAddressOf());
        FAILED(hr)) {
        string err{};
        if (errBlob != nullptr && errBlob->GetBufferPointer() != nullptr) {
            err.assign(
                static_cast<const char*>(errBlob->GetBufferPointer()),
                static_cast<size_t>(errBlob->GetBufferSize()));
        }
        RADRAY_ERR_LOG("D3D12SerializeVersionedRootSignature failed: {} {}\n{}", GetErrorName(hr), hr, err);
        return nullptr;
    }

    ComPtr<ID3D12RootSignature> rootSig{};
    if (HRESULT hr = _device->CreateRootSignature(
            0,
            rootSigBlob->GetBufferPointer(),
            rootSigBlob->GetBufferSize(),
            IID_PPV_ARGS(rootSig.GetAddressOf()));
        FAILED(hr)) {
        RADRAY_ERR_LOG("ID3D12Device::CreateRootSignature failed: {} {}", GetErrorName(hr), hr);
        return nullptr;
    }

    auto result = make_unique<RootSigD3D12>(this, std::move(rootSig));
    result->_desc = VersionedRootSignatureDescContainer{versionDesc};
    result->_bindingLayout = std::move(publicLayout);
    result->_descriptorSetLayouts = std::move(descriptorSetLayouts);
    result->_bindlessSetLayouts = std::move(bindlessSetLayouts);
    result->_staticSamplerLayouts = std::move(staticSamplerLayouts);
    result->_pushConstantRanges = std::move(pushConstantRanges);
    result->_parameters = std::move(parameterInfos);
    result->_descriptorSets = std::move(descriptorSets);
    result->_bindlessSets = std::move(bindlessSets);
    result->_resourceTables = std::move(resourceTables);
    result->_samplerTables = std::move(samplerTables);
    return result;
}

Nullable<unique_ptr<DescriptorSet>> DeviceD3D12::CreateDescriptorSet(RootSignature* rootSig_, DescriptorSetIndex setIndex) noexcept {
    if (rootSig_ == nullptr) {
        RADRAY_ERR_LOG("root signature is null");
        return nullptr;
    }
    auto* rootSig = CastD3D12Object(rootSig_);
    if (rootSig == nullptr || !rootSig->IsValid()) {
        RADRAY_ERR_LOG("root signature is invalid");
        return nullptr;
    }
    if (rootSig->HasBindlessSet(setIndex)) {
        RADRAY_ERR_LOG("descriptor set {} is declared as a bindless set", setIndex.Value);
        return nullptr;
    }
    auto setInfoOpt = rootSig->FindDescriptorSetInfo(setIndex);
    if (!setInfoOpt.HasValue() || setInfoOpt.Get() == nullptr) {
        RADRAY_ERR_LOG("descriptor set {} is unavailable", setIndex.Value);
        return nullptr;
    }
    const auto* setInfo = setInfoOpt.Get();

    GpuDescriptorHeapViewRAII resHeapView{};
    if (setInfo->ResourceDescriptorCount > 0) {
        auto alloc = _gpuResHeap->Allocate(setInfo->ResourceDescriptorCount);
        if (!alloc.has_value()) {
            RADRAY_ERR_LOG("failed to allocate d3d12 resource descriptor heap slice");
            return nullptr;
        }
        resHeapView = GpuDescriptorHeapViewRAII{_gpuResHeap.get(), alloc.value()};
    }

    GpuDescriptorHeapViewRAII samplerHeapView{};
    if (setInfo->SamplerDescriptorCount > 0) {
        auto alloc = _gpuSamplerHeap->Allocate(setInfo->SamplerDescriptorCount);
        if (!alloc.has_value()) {
            RADRAY_ERR_LOG("failed to allocate d3d12 sampler descriptor heap slice");
            return nullptr;
        }
        samplerHeapView = GpuDescriptorHeapViewRAII{_gpuSamplerHeap.get(), alloc.value()};
    }

    auto result = make_unique<DescriptorSetD3D12>(this, rootSig, setIndex, std::move(resHeapView), std::move(samplerHeapView));
    result->_resourceWritten.resize(setInfo->ResourceDescriptorCount, 0);
    result->_samplerWritten.resize(setInfo->SamplerDescriptorCount, 0);
    return result;
}

Nullable<unique_ptr<GraphicsPipelineState>> DeviceD3D12::CreateGraphicsPipelineState(const GraphicsPipelineStateDescriptor& desc) noexcept {
    if (desc.Primitive.StripIndexFormat.has_value() &&
        desc.Primitive.Topology != PrimitiveTopology::LineStrip &&
        desc.Primitive.Topology != PrimitiveTopology::TriangleStrip) {
        RADRAY_ERR_LOG("StripIndexFormat is only valid for LineStrip or TriangleStrip topology");
        return nullptr;
    }
    auto [topoClass, topo] = MapType(desc.Primitive.Topology);
    vector<D3D12_INPUT_ELEMENT_DESC> inputElements;
    vector<uint64_t> arrayStrides(desc.VertexLayouts.size(), 0);
    for (size_t index = 0; index < desc.VertexLayouts.size(); index++) {
        const VertexBufferLayout& i = desc.VertexLayouts[index];
        arrayStrides[index] = i.ArrayStride;
        D3D12_INPUT_CLASSIFICATION inputClass = MapType(i.StepMode);
        for (const VertexElement& j : i.Elements) {
            auto& ied = inputElements.emplace_back(D3D12_INPUT_ELEMENT_DESC{});
            ied.SemanticName = j.Semantic.data();
            ied.SemanticIndex = j.SemanticIndex;
            ied.Format = MapType(j.Format);
            ied.InputSlot = (UINT)index;
            ied.AlignedByteOffset = (UINT)j.Offset;
            ied.InputSlotClass = inputClass;
            ied.InstanceDataStepRate = inputClass == D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA ? 1 : 0;
        }
    }
    DepthBiasState depBias;
    if (desc.DepthStencil.has_value()) {
        depBias = desc.DepthStencil.value().DepthBias;
    } else {
        depBias = DepthBiasState{0, 0, 0};
    }
    D3D12_RASTERIZER_DESC rawRaster{};
    if (auto fillMode = MapType(desc.Primitive.Poly);
        fillMode.has_value()) {
        rawRaster.FillMode = fillMode.value();
    } else {
        RADRAY_ERR_LOG("invalid primitive polygon mode: {}", desc.Primitive.Poly);
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
    rawBlend.IndependentBlendEnable = false;
    for (size_t i = 0; i < ArrayLength(rawBlend.RenderTarget); i++) {
        D3D12_RENDER_TARGET_BLEND_DESC& rtb = rawBlend.RenderTarget[i];
        if (i < desc.ColorTargets.size()) {
            const ColorTargetState& ct = desc.ColorTargets[i];
            rtb.BlendEnable = ct.Blend.has_value();
            if (ct.Blend.has_value()) {
                const auto& ctBlend = ct.Blend.value();
                rtb.SrcBlend = MapBlendColor(ctBlend.Color.Src);
                rtb.DestBlend = MapBlendColor(ctBlend.Color.Dst);
                rtb.BlendOp = MapType(ctBlend.Color.Op);
                rtb.SrcBlendAlpha = MapBlendAlpha(ctBlend.Alpha.Src);
                rtb.DestBlendAlpha = MapBlendAlpha(ctBlend.Alpha.Dst);
                rtb.BlendOpAlpha = MapType(ctBlend.Alpha.Op);
            }
            if (auto writeMask = MapColorWrites(ct.WriteMask);
                writeMask.has_value()) {
                rtb.RenderTargetWriteMask = (UINT8)writeMask.value();
            } else {
                RADRAY_ERR_LOG("invalid color target write mask: {}", ct.WriteMask);
                return nullptr;
            }
        } else {
            rtb.BlendEnable = false;
            rtb.LogicOpEnable = false;
            rtb.LogicOp = D3D12_LOGIC_OP_CLEAR;
            rtb.RenderTargetWriteMask = 0;
        }
    }
    D3D12_DEPTH_STENCIL_DESC dsDesc{};
    if (desc.DepthStencil.has_value()) {
        const auto& ds = desc.DepthStencil.value();
        dsDesc.DepthEnable = true;
        dsDesc.DepthWriteMask = ds.DepthWriteEnable ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        dsDesc.DepthFunc = MapType(ds.DepthCompare);
        dsDesc.StencilEnable = ds.Stencil.has_value();
        if (ds.Stencil.has_value()) {
            const auto& s = ds.Stencil.value();
            auto ToDsd = [](StencilFaceState v) noexcept {
                D3D12_DEPTH_STENCILOP_DESC result{};
                result.StencilFailOp = MapType(v.FailOp);
                result.StencilDepthFailOp = MapType(v.DepthFailOp);
                result.StencilPassOp = MapType(v.PassOp);
                result.StencilFunc = MapType(v.Compare);
                return result;
            };
            dsDesc.StencilReadMask = (UINT8)s.ReadMask;
            dsDesc.StencilWriteMask = (UINT8)s.WriteMask;
            dsDesc.FrontFace = ToDsd(s.Front);
            dsDesc.BackFace = ToDsd(s.Back);
        }
    } else {
        dsDesc.DepthEnable = false;
        dsDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        dsDesc.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        dsDesc.StencilEnable = false;
        dsDesc.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
        dsDesc.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
        dsDesc.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
        dsDesc.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
        dsDesc.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
        dsDesc.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        dsDesc.BackFace = dsDesc.FrontFace;
    }
    DXGI_SAMPLE_DESC sampleDesc{desc.MultiSample.Count, 0};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC rawPsoDesc{};
    rawPsoDesc.pRootSignature = CastD3D12Object(desc.RootSig)->_rootSig.Get();
    rawPsoDesc.VS = desc.VS ? CastD3D12Object(desc.VS->Target)->ToByteCode() : D3D12_SHADER_BYTECODE{};
    rawPsoDesc.PS = desc.PS ? CastD3D12Object(desc.PS->Target)->ToByteCode() : D3D12_SHADER_BYTECODE{};
    rawPsoDesc.DS = D3D12_SHADER_BYTECODE{};
    rawPsoDesc.HS = D3D12_SHADER_BYTECODE{};
    rawPsoDesc.GS = D3D12_SHADER_BYTECODE{};
    rawPsoDesc.StreamOutput = D3D12_STREAM_OUTPUT_DESC{};
    rawPsoDesc.BlendState = rawBlend;
    rawPsoDesc.SampleMask = (UINT)desc.MultiSample.Mask;
    rawPsoDesc.RasterizerState = rawRaster;
    rawPsoDesc.DepthStencilState = dsDesc;
    rawPsoDesc.InputLayout = {inputElements.data(), static_cast<uint32_t>(inputElements.size())};
    rawPsoDesc.IBStripCutValue = desc.Primitive.StripIndexFormat.has_value() ? MapType(desc.Primitive.StripIndexFormat.value()) : D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    rawPsoDesc.PrimitiveTopologyType = topoClass;
    rawPsoDesc.NumRenderTargets = std::min(static_cast<uint32_t>(desc.ColorTargets.size()), (uint32_t)D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT);
    for (size_t i = 0; i < rawPsoDesc.NumRenderTargets; i++) {
        rawPsoDesc.RTVFormats[i] = i < desc.ColorTargets.size() ? MapType(desc.ColorTargets[i].Format) : DXGI_FORMAT_UNKNOWN;
    }
    rawPsoDesc.DSVFormat = desc.DepthStencil.has_value() ? MapType(desc.DepthStencil.value().Format) : DXGI_FORMAT_UNKNOWN;
    rawPsoDesc.SampleDesc = sampleDesc;
    rawPsoDesc.NodeMask = 0;
    rawPsoDesc.CachedPSO = D3D12_CACHED_PIPELINE_STATE{};
    rawPsoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    ComPtr<ID3D12PipelineState> pso;
    if (HRESULT hr = _device->CreateGraphicsPipelineState(&rawPsoDesc, IID_PPV_ARGS(pso.GetAddressOf()));
        FAILED(hr)) {
        RADRAY_ERR_LOG("ID3D12Device::CreateGraphicsPipelineState failed: {} {}", GetErrorName(hr), hr);
        return nullptr;
    }
    return make_unique<GraphicsPsoD3D12>(this, std::move(pso), std::move(arrayStrides), topo);
}

Nullable<unique_ptr<ComputePipelineState>> DeviceD3D12::CreateComputePipelineState(const ComputePipelineStateDescriptor& desc) noexcept {
    D3D12_COMPUTE_PIPELINE_STATE_DESC rawPsoDesc{};
    rawPsoDesc.pRootSignature = CastD3D12Object(desc.RootSig)->_rootSig.Get();
    rawPsoDesc.CS = CastD3D12Object(desc.CS.Target)->ToByteCode();
    rawPsoDesc.NodeMask = 0;
    rawPsoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    ComPtr<ID3D12PipelineState> pso;
    if (HRESULT hr = _device->CreateComputePipelineState(&rawPsoDesc, IID_PPV_ARGS(pso.GetAddressOf()));
        FAILED(hr)) {
        RADRAY_ERR_LOG("ID3D12Device::CreateComputePipelineState failed: {} {}", GetErrorName(hr), hr);
        return nullptr;
    }
    return make_unique<ComputePsoD3D12>(this, std::move(pso));
}

Nullable<unique_ptr<AccelerationStructure>> DeviceD3D12::CreateAccelerationStructure(const AccelerationStructureDescriptor& desc) noexcept {
    if (!this->GetDetail().IsRayTracingSupported) {
        RADRAY_ERR_LOG("d3d12 ray tracing acceleration structure is not supported by this device");
        return nullptr;
    }
    uint64_t estimatedSize = 0;
    if (desc.Type == AccelerationStructureType::BottomLevel) {
        estimatedSize = std::max<uint64_t>(1ull << 20, uint64_t(std::max(1u, desc.MaxGeometryCount)) * (2ull << 20));
    } else {
        estimatedSize = std::max<uint64_t>(1ull << 20, uint64_t(std::max(1u, desc.MaxInstanceCount)) * 4096ull);
    }
    estimatedSize = Align(estimatedSize, uint64_t(this->GetDetail().AccelerationStructureAlignment));

    D3D12_RESOURCE_DESC rawDesc{};
    rawDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rawDesc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    rawDesc.Width = estimatedSize;
    rawDesc.Height = 1;
    rawDesc.DepthOrArraySize = 1;
    rawDesc.MipLevels = 1;
    rawDesc.Format = DXGI_FORMAT_UNKNOWN;
    rawDesc.SampleDesc.Count = 1;
    rawDesc.SampleDesc.Quality = 0;
    rawDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rawDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12MA::ALLOCATION_DESC allocDesc{};
    allocDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    allocDesc.Flags = D3D12MA::ALLOCATION_FLAG_NONE;
    if (desc.Flags.HasFlag(AccelerationStructureBuildFlag::AllowCompaction)) {
        allocDesc.Flags = static_cast<D3D12MA::ALLOCATION_FLAGS>(allocDesc.Flags | D3D12MA::ALLOCATION_FLAG_COMMITTED);
    }

    ComPtr<ID3D12Resource> buffer;
    ComPtr<D3D12MA::Allocation> allocRes;
    if (HRESULT hr = _mainAlloc->CreateResource(
            &allocDesc,
            &rawDesc,
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
            nullptr,
            allocRes.GetAddressOf(),
            IID_PPV_ARGS(buffer.GetAddressOf()));
        FAILED(hr)) {
        RADRAY_ERR_LOG("D3D12MA::Allocator::CreateResource (AS) failed: {} {}", GetErrorName(hr), hr);
        return nullptr;
    }
    return make_unique<AccelerationStructureD3D12>(this, std::move(buffer), std::move(allocRes), desc, estimatedSize);
}

Nullable<unique_ptr<RayTracingPipelineState>> DeviceD3D12::CreateRayTracingPipelineState(const RayTracingPipelineStateDescriptor& desc) noexcept {
    if (!this->GetDetail().IsRayTracingSupported) {
        RADRAY_ERR_LOG("d3d12 ray tracing pipeline state is not supported by this device");
        return nullptr;
    }
    if (desc.RootSig == nullptr) {
        RADRAY_ERR_LOG("RayTracingPipelineStateDescriptor.RootSig is null");
        return nullptr;
    }
    if (desc.ShaderEntries.empty()) {
        RADRAY_ERR_LOG("RayTracingPipelineStateDescriptor.ShaderEntries is empty");
        return nullptr;
    }
    if (desc.MaxRecursionDepth == 0 || desc.MaxRecursionDepth > this->GetDetail().MaxRayRecursionDepth) {
        RADRAY_ERR_LOG("invalid MaxRecursionDepth {} (device max={})", desc.MaxRecursionDepth, this->GetDetail().MaxRayRecursionDepth);
        return nullptr;
    }
    auto isRtStage = [](ShaderStage s) {
        return s == ShaderStage::RayGen ||
               s == ShaderStage::Miss ||
               s == ShaderStage::ClosestHit ||
               s == ShaderStage::AnyHit ||
               s == ShaderStage::Intersection ||
               s == ShaderStage::Callable;
    };
    auto supportsShaderIdentifier = [](ShaderStage s) {
        return s == ShaderStage::RayGen ||
               s == ShaderStage::Miss ||
               s == ShaderStage::Callable;
    };

    vector<wstring> exportNamesW;
    vector<D3D12_EXPORT_DESC> exportDescs;
    vector<D3D12_DXIL_LIBRARY_DESC> libDescs;
    vector<D3D12_STATE_SUBOBJECT> subobjects;
    exportNamesW.reserve(desc.ShaderEntries.size() + desc.HitGroups.size());
    exportDescs.reserve(desc.ShaderEntries.size());
    libDescs.reserve(desc.ShaderEntries.size());
    subobjects.reserve(desc.ShaderEntries.size() + desc.HitGroups.size() + 4);

    unordered_set<string> exportNamesUtf8;
    unordered_set<string> shaderIdNamesUtf8;
    for (const auto& entry : desc.ShaderEntries) {
        if (entry.Target == nullptr) {
            RADRAY_ERR_LOG("RayTracingPipelineStateDescriptor contains null shader entry target");
            return nullptr;
        }
        if (!isRtStage(entry.Stage)) {
            RADRAY_ERR_LOG("RayTracingPipelineStateDescriptor shader entry has non-RT stage {}", entry.Stage);
            return nullptr;
        }
        string exportNameUtf8{entry.EntryPoint};
        if (exportNameUtf8.empty()) {
            RADRAY_ERR_LOG("RayTracingPipelineStateDescriptor shader entry has empty export name");
            return nullptr;
        }
        if (!exportNamesUtf8.insert(exportNameUtf8).second) {
            RADRAY_ERR_LOG("duplicated RT export name '{}'", exportNameUtf8);
            return nullptr;
        }
        if (supportsShaderIdentifier(entry.Stage)) {
            shaderIdNamesUtf8.insert(exportNameUtf8);
        }
        auto exportNameWOpt = text_encoding::ToWideChar(exportNameUtf8);
        if (!exportNameWOpt.has_value()) {
            RADRAY_ERR_LOG("cannot convert RT export name to wide char '{}'", exportNameUtf8);
            return nullptr;
        }
        exportNamesW.emplace_back(std::move(exportNameWOpt.value()));
        auto* dxil = CastD3D12Object(entry.Target);
        D3D12_EXPORT_DESC exportDesc{};
        exportDesc.Name = exportNamesW.back().c_str();
        exportDesc.ExportToRename = nullptr;
        exportDesc.Flags = D3D12_EXPORT_FLAG_NONE;
        exportDescs.push_back(exportDesc);

        D3D12_DXIL_LIBRARY_DESC libDesc{};
        libDesc.DXILLibrary = dxil->ToByteCode();
        libDesc.NumExports = 1;
        libDesc.pExports = &exportDescs.back();
        libDescs.push_back(libDesc);
    }

    for (auto& i : libDescs) {
        D3D12_STATE_SUBOBJECT subObj{};
        subObj.Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
        subObj.pDesc = &i;
        subobjects.push_back(subObj);
    }

    vector<D3D12_HIT_GROUP_DESC> hitGroups;
    hitGroups.reserve(desc.HitGroups.size());
    vector<wstring> hitGroupClosestW;
    vector<wstring> hitGroupAnyW;
    vector<wstring> hitGroupIntersectW;
    hitGroupClosestW.reserve(desc.HitGroups.size());
    hitGroupAnyW.reserve(desc.HitGroups.size());
    hitGroupIntersectW.reserve(desc.HitGroups.size());
    for (const auto& hit : desc.HitGroups) {
        auto hitGroupType = GetHitGroupType(hit);
        if (!hitGroupType.has_value()) {
            RADRAY_ERR_LOG("invalid hit group '{}' without shader references", hit.Name);
            return nullptr;
        }
        string groupNameUtf8{hit.Name};
        if (groupNameUtf8.empty()) {
            RADRAY_ERR_LOG("RayTracingHitGroupDescriptor.Name is empty");
            return nullptr;
        }
        if (!exportNamesUtf8.insert(groupNameUtf8).second) {
            RADRAY_ERR_LOG("duplicated RT hit group name '{}'", groupNameUtf8);
            return nullptr;
        }
        shaderIdNamesUtf8.insert(groupNameUtf8);
        auto groupNameWOpt = text_encoding::ToWideChar(groupNameUtf8);
        if (!groupNameWOpt.has_value()) {
            RADRAY_ERR_LOG("cannot convert hit group name '{}'", groupNameUtf8);
            return nullptr;
        }
        exportNamesW.emplace_back(std::move(groupNameWOpt.value()));

        D3D12_HIT_GROUP_DESC hg{};
        hg.Type = hitGroupType.value();
        hg.HitGroupExport = exportNamesW.back().c_str();
        if (hit.ClosestHit.has_value()) {
            string s{hit.ClosestHit->EntryPoint};
            if (s.empty()) {
                RADRAY_ERR_LOG("hit group '{}' has empty ClosestHit entry", groupNameUtf8);
                return nullptr;
            }
            auto w = text_encoding::ToWideChar(s);
            if (!w.has_value()) {
                RADRAY_ERR_LOG("cannot convert ClosestHit entry '{}' to wide char", s);
                return nullptr;
            }
            hitGroupClosestW.emplace_back(std::move(w.value()));
            hg.ClosestHitShaderImport = hitGroupClosestW.back().c_str();
        }
        if (hit.AnyHit.has_value()) {
            string s{hit.AnyHit->EntryPoint};
            if (s.empty()) {
                RADRAY_ERR_LOG("hit group '{}' has empty AnyHit entry", groupNameUtf8);
                return nullptr;
            }
            auto w = text_encoding::ToWideChar(s);
            if (!w.has_value()) {
                RADRAY_ERR_LOG("cannot convert AnyHit entry '{}' to wide char", s);
                return nullptr;
            }
            hitGroupAnyW.emplace_back(std::move(w.value()));
            hg.AnyHitShaderImport = hitGroupAnyW.back().c_str();
        }
        if (hit.Intersection.has_value()) {
            string s{hit.Intersection->EntryPoint};
            if (s.empty()) {
                RADRAY_ERR_LOG("hit group '{}' has empty Intersection entry", groupNameUtf8);
                return nullptr;
            }
            auto w = text_encoding::ToWideChar(s);
            if (!w.has_value()) {
                RADRAY_ERR_LOG("cannot convert Intersection entry '{}' to wide char", s);
                return nullptr;
            }
            hitGroupIntersectW.emplace_back(std::move(w.value()));
            hg.IntersectionShaderImport = hitGroupIntersectW.back().c_str();
        }
        hitGroups.push_back(hg);
    }
    for (auto& i : hitGroups) {
        D3D12_STATE_SUBOBJECT subObj{};
        subObj.Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;
        subObj.pDesc = &i;
        subobjects.push_back(subObj);
    }

    D3D12_RAYTRACING_SHADER_CONFIG shaderCfg{};
    shaderCfg.MaxPayloadSizeInBytes = desc.MaxPayloadSize;
    shaderCfg.MaxAttributeSizeInBytes = desc.MaxAttributeSize;
    D3D12_STATE_SUBOBJECT shaderCfgSubObj{};
    shaderCfgSubObj.Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG;
    shaderCfgSubObj.pDesc = &shaderCfg;
    subobjects.push_back(shaderCfgSubObj);

    auto rootSig = CastD3D12Object(desc.RootSig);
    ID3D12RootSignature* rawRootSig = rootSig->_rootSig.Get();
    D3D12_STATE_SUBOBJECT globalRsSubObj{};
    globalRsSubObj.Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;
    globalRsSubObj.pDesc = &rawRootSig;
    subobjects.push_back(globalRsSubObj);

    D3D12_RAYTRACING_PIPELINE_CONFIG pipelineCfg{};
    pipelineCfg.MaxTraceRecursionDepth = desc.MaxRecursionDepth;
    D3D12_STATE_SUBOBJECT pipelineCfgSubObj{};
    pipelineCfgSubObj.Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG;
    pipelineCfgSubObj.pDesc = &pipelineCfg;
    subobjects.push_back(pipelineCfgSubObj);

    D3D12_STATE_OBJECT_DESC soDesc{};
    soDesc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    soDesc.NumSubobjects = static_cast<UINT>(subobjects.size());
    soDesc.pSubobjects = subobjects.data();

    ComPtr<ID3D12Device5> device5;
    if (HRESULT hr = _device->QueryInterface(IID_PPV_ARGS(device5.GetAddressOf()));
        FAILED(hr)) {
        RADRAY_ERR_LOG("ID3D12Device::QueryInterface(ID3D12Device5) failed: {} {}", GetErrorName(hr), hr);
        return nullptr;
    }
    ComPtr<ID3D12StateObject> stateObject;
    if (HRESULT hr = device5->CreateStateObject(&soDesc, IID_PPV_ARGS(stateObject.GetAddressOf()));
        FAILED(hr)) {
        RADRAY_ERR_LOG("ID3D12Device5::CreateStateObject failed: {} {}", GetErrorName(hr), hr);
        return nullptr;
    }
    ComPtr<ID3D12StateObjectProperties> stateProps;
    if (HRESULT hr = stateObject->QueryInterface(IID_PPV_ARGS(stateProps.GetAddressOf()));
        FAILED(hr)) {
        RADRAY_ERR_LOG("ID3D12StateObject::QueryInterface(ID3D12StateObjectProperties) failed: {} {}", GetErrorName(hr), hr);
        return nullptr;
    }

    auto result = make_unique<RayTracingPsoD3D12>(this, std::move(stateObject), std::move(stateProps), rootSig);
    for (const auto& name : shaderIdNamesUtf8) {
        auto nameWOpt = text_encoding::ToWideChar(name);
        if (!nameWOpt.has_value()) {
            RADRAY_ERR_LOG("cannot convert export name '{}' to wide char", name);
            return nullptr;
        }
        void* shaderId = result->_stateProps->GetShaderIdentifier(nameWOpt->c_str());
        if (shaderId == nullptr) {
            RADRAY_ERR_LOG("cannot get shader identifier for export '{}'", name);
            return nullptr;
        }
        auto& id = result->_shaderIdentifiers[name];
        std::memcpy(id.data(), shaderId, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
    }
    return result;
}

Nullable<unique_ptr<ShaderBindingTable>> DeviceD3D12::CreateShaderBindingTable(const ShaderBindingTableDescriptor& desc) noexcept {
    if (!this->GetDetail().IsRayTracingSupported) {
        RADRAY_ERR_LOG("d3d12 shader binding table is not supported by this device");
        return nullptr;
    }
    if (desc.Pipeline == nullptr) {
        RADRAY_ERR_LOG("ShaderBindingTableDescriptor.Pipeline is null");
        return nullptr;
    }
    if (desc.RayGenCount != 1) {
        RADRAY_ERR_LOG("ShaderBindingTableDescriptor.RayGenCount must be exactly 1");
        return nullptr;
    }
    auto* pipeline = CastD3D12Object(desc.Pipeline);
    auto req = pipeline->GetShaderBindingTableRequirements();
    if (req.HandleSize == 0) {
        RADRAY_ERR_LOG("invalid SBT handle size");
        return nullptr;
    }
    uint64_t recordSize = static_cast<uint64_t>(req.HandleSize) + static_cast<uint64_t>(desc.MaxLocalDataSize);
    uint64_t recordStride = Align(recordSize, static_cast<uint64_t>(this->GetDetail().ShaderTableAlignment));
    uint64_t totalRecords = static_cast<uint64_t>(desc.RayGenCount) +
                            static_cast<uint64_t>(desc.MissCount) +
                            static_cast<uint64_t>(desc.HitGroupCount) +
                            static_cast<uint64_t>(desc.CallableCount);
    if (totalRecords == 0) {
        RADRAY_ERR_LOG("ShaderBindingTableDescriptor has no records");
        return nullptr;
    }
    uint64_t totalSize = recordStride * totalRecords;
    auto buffer = this->CreateBuffer(
        {totalSize, MemoryType::Upload, BufferUse::ShaderTable | BufferUse::MapWrite, ResourceHint::None});
    if (!buffer.HasValue()) {
        return nullptr;
    }
    return make_unique<ShaderBindingTableD3D12>(this, pipeline, buffer.Release(), desc, recordStride);
}

Nullable<unique_ptr<Sampler>> DeviceD3D12::CreateSampler(const SamplerDescriptor& desc) noexcept {
    D3D12_SAMPLER_DESC rawDesc{};
    rawDesc.Filter = MapType(desc.MinFilter, desc.MagFilter, desc.MipmapFilter, desc.Compare.has_value(), desc.AnisotropyClamp);
    rawDesc.AddressU = MapType(desc.AddressS);
    rawDesc.AddressV = MapType(desc.AddressT);
    rawDesc.AddressW = MapType(desc.AddressR);
    rawDesc.MipLODBias = 0;
    rawDesc.MaxAnisotropy = desc.AnisotropyClamp;
    rawDesc.ComparisonFunc = desc.Compare.has_value() ? MapType(desc.Compare.value()) : D3D12_COMPARISON_FUNC_NEVER;
    rawDesc.BorderColor[0] = 0;
    rawDesc.BorderColor[1] = 0;
    rawDesc.BorderColor[2] = 0;
    rawDesc.BorderColor[3] = 0;
    rawDesc.MinLOD = desc.LodMin;
    rawDesc.MaxLOD = desc.LodMax;
    auto alloc = this->_cpuSamplerAlloc.get();
    CpuDescriptorHeapViewRAII heapView{};
    {
        auto opt = alloc->Allocate(1);
        if (!opt.has_value()) {
            RADRAY_ERR_LOG("CpuDescriptorAllocator::Allocate failed: {}", "cannot allocate Sampler descriptors");
            return nullptr;
        }
        heapView = {alloc, opt.value()};
    }
    heapView.GetHeap()->Create(rawDesc, heapView.GetStart());
    return make_unique<SamplerD3D12>(this, std::move(heapView));
}

Nullable<unique_ptr<BindlessArray>> DeviceD3D12::CreateBindlessArray(const BindlessArrayDescriptor& desc) noexcept {
    if (!this->GetDetail().IsBindlessArraySupported) {
        RADRAY_ERR_LOG("d3d12 bindless array is not supported by this device");
        return nullptr;
    }
    if (desc.Size == 0) {
        RADRAY_ERR_LOG("d3d12 bindless array size must be greater than 0");
        return nullptr;
    }
    if (desc.SlotType != BindlessSlotType::BufferOnly &&
        desc.SlotType != BindlessSlotType::Texture2DOnly) {
        RADRAY_ERR_LOG(
            "d3d12 bindless array slot type {} is not supported by the shader-derived path",
            desc.SlotType);
        return nullptr;
    }
    GpuDescriptorHeapViewRAII resHeapView{};
    {
        auto gpuResHeapAllocationOpt = _gpuResHeap->Allocate(desc.Size);
        if (!gpuResHeapAllocationOpt.has_value()) {
            RADRAY_ERR_LOG("GpuDescriptorAllocator::Allocate failed: {}", "cannot allocate bindless resource descriptors");
            return nullptr;
        }
        resHeapView = {_gpuResHeap.get(), gpuResHeapAllocationOpt.value()};
    }
    return make_unique<BindlessArrayD3D12>(
        this,
        desc,
        std::move(resHeapView),
        GpuDescriptorHeapViewRAII{});
}

Nullable<unique_ptr<FenceD3D12>> DeviceD3D12::CreateFenceD3D12(uint64_t initValue) noexcept {
    ComPtr<ID3D12Fence> fence;
    if (HRESULT hr = _device->CreateFence(initValue, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf()));
        FAILED(hr)) {
        RADRAY_ERR_LOG("ID3D12Device::CreateFence failed: {} {}", GetErrorName(hr), hr);
        return nullptr;
    }
    std::optional<Win32Event> e = Win32Event::Create();
    if (!e.has_value()) {
        return nullptr;
    }
    auto result = make_unique<FenceD3D12>(std::move(fence), std::move(e.value()));
    result->_fenceValue = initValue + 1;
    return result;
}

void DeviceD3D12::TryDrainValidationMessages() {
    if (!_isDebugLayerEnabled || _infoQueueCallbackCookie != 0) {
        return;
    }
    ComPtr<ID3D12InfoQueue> infoQueue;
    if (HRESULT hr = _device.As(&infoQueue);
        FAILED(hr) || infoQueue == nullptr) {
        return;
    }

    const UINT64 messageCount = infoQueue->GetNumStoredMessages();
    for (UINT64 i = 0; i < messageCount; i++) {
        SIZE_T messageByteLength = 0;
        if (FAILED(infoQueue->GetMessage(i, nullptr, &messageByteLength)) || messageByteLength == 0) {
            continue;
        }
        vector<byte> data(static_cast<size_t>(messageByteLength));
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(data.data());
        if (FAILED(infoQueue->GetMessage(i, message, &messageByteLength))) {
            continue;
        }
        _LogD3D12ValidationMessage(this, message->Category, message->Severity, message->ID, message->pDescription);
    }
    if (messageCount != 0) {
        infoQueue->ClearStoredMessages();
    }
}

CmdQueueD3D12::CmdQueueD3D12(
    DeviceD3D12* device,
    ComPtr<ID3D12CommandQueue> queue,
    D3D12_COMMAND_LIST_TYPE type,
    unique_ptr<FenceD3D12> fence) noexcept
    : _device(device),
      _queue(std::move(queue)),
      _fence(std::move(fence)),
      _type(type) {}

bool CmdQueueD3D12::IsValid() const noexcept {
    return _queue != nullptr;
}

void CmdQueueD3D12::Destroy() noexcept {
    _fence = nullptr;
    _queue = nullptr;
}

void CmdQueueD3D12::Submit(const CommandQueueSubmitDescriptor& desc) noexcept {
    for (size_t i = 0; i < desc.WaitFences.size(); ++i) {
        auto* f = CastD3D12Object(desc.WaitFences[i]);
        uint64_t waitValue = desc.WaitValues[i];
        _queue->Wait(f->_fence.Get(), waitValue);
    }

    vector<ID3D12CommandList*> submits;
    submits.reserve(desc.CmdBuffers.size());
    for (auto& i : desc.CmdBuffers) {
        auto cmdList = CastD3D12Object(i);
        submits.emplace_back(cmdList->_cmdList.Get());
    }
    if (!submits.empty()) {
        _queue->ExecuteCommandLists(static_cast<UINT>(submits.size()), submits.data());
    }

    for (size_t i = 0; i < desc.SignalFences.size(); ++i) {
        auto* f = CastD3D12Object(desc.SignalFences[i]);
        uint64_t signalValue = desc.SignalValues[i];
        _queue->Signal(f->_fence.Get(), signalValue);
        if (signalValue + 1 > f->_fenceValue) {
            f->_fenceValue = signalValue + 1;
        }
    }

    _device->TryDrainValidationMessages();
}

void CmdQueueD3D12::Wait() noexcept {
    _queue->Signal(_fence->_fence.Get(), _fence->_fenceValue++);
    _fence->Wait();
    _device->TryDrainValidationMessages();
}

QueueType CmdQueueD3D12::GetQueueType() const noexcept {
    switch (_type) {
        case D3D12_COMMAND_LIST_TYPE_DIRECT:
            return QueueType::Direct;
        case D3D12_COMMAND_LIST_TYPE_COMPUTE:
            return QueueType::Compute;
        case D3D12_COMMAND_LIST_TYPE_COPY:
            return QueueType::Copy;
        default:
            RADRAY_ABORT("invalid D3D12 command list type: {}", _type);
            return QueueType::Direct;
    }
}

FenceD3D12::FenceD3D12(
    ComPtr<ID3D12Fence> fence,
    Win32Event event) noexcept
    : _fence(std::move(fence)),
      _event(std::move(event)) {}

bool FenceD3D12::IsValid() const noexcept {
    return _fence != nullptr;
}

void FenceD3D12::Destroy() noexcept {
    _fence = nullptr;
    _event.Destroy();
}

void FenceD3D12::SetDebugName(std::string_view name) noexcept {
    SetObjectName(name, _fence.Get());
}

void FenceD3D12::Wait() noexcept {
    UINT64 completedValue = _fence->GetCompletedValue();
    uint64_t signaledValue = _fenceValue - 1;
    if (completedValue < signaledValue) {
        _fence->SetEventOnCompletion(signaledValue, _event.Get());
        ::WaitForSingleObject(_event.Get(), INFINITE);
    }
}

void FenceD3D12::Wait(uint64_t value) noexcept {
    UINT64 completedValue = _fence->GetCompletedValue();
    if (completedValue < value) {
        _fence->SetEventOnCompletion(value, _event.Get());
        ::WaitForSingleObject(_event.Get(), INFINITE);
    }
}

uint64_t FenceD3D12::GetCompletedValue() const noexcept {
    return _fence->GetCompletedValue();
}

uint64_t FenceD3D12::GetLastSignaledValue() const noexcept {
    return _fenceValue - 1;
}

CmdListD3D12::CmdListD3D12(
    DeviceD3D12* device,
    ComPtr<ID3D12CommandAllocator> cmdAlloc,
    ComPtr<ID3D12GraphicsCommandList> cmdList,
    D3D12_COMMAND_LIST_TYPE type) noexcept
    : _device(device),
      _cmdAlloc(std::move(cmdAlloc)),
      _cmdList(std::move(cmdList)),
      _type(type) {}

bool CmdListD3D12::IsValid() const noexcept {
    return _cmdAlloc != nullptr && _cmdList != nullptr;
}

void CmdListD3D12::Destroy() noexcept {
    _keepAliveResources.clear();
    _cmdAlloc = nullptr;
    _cmdList = nullptr;
}

void CmdListD3D12::SetDebugName(std::string_view name) noexcept {
    auto listName = fmt::format("CmdList_{}", name);
    auto allocName = fmt::format("CmdAlloc_{}", name);
    SetObjectName(name, _cmdList.Get());
    SetObjectName(name, _cmdAlloc.Get());
}

void CmdListD3D12::Begin() noexcept {
    _keepAliveResources.clear();
    if (HRESULT hr = _cmdAlloc->Reset();
        FAILED(hr)) {
        RADRAY_ABORT("ID3D12CommandAllocator::Reset failed: {} {}", GetErrorName(hr), hr);
    }
    if (HRESULT hr = _cmdList->Reset(_cmdAlloc.Get(), nullptr);
        FAILED(hr)) {
        RADRAY_ABORT("ID3D12GraphicsCommandList::Reset failed: {} {}", GetErrorName(hr), hr);
    }
    ID3D12DescriptorHeap* heaps[] = {_device->_gpuResHeap->GetNative(), _device->_gpuSamplerHeap->GetNative()};
    if (_type != D3D12_COMMAND_LIST_TYPE_COPY) {
        _cmdList->SetDescriptorHeaps((UINT)radray::ArrayLength(heaps), heaps);
    }
}

void CmdListD3D12::End() noexcept {
    _cmdList->Close();
}

void CmdListD3D12::ResourceBarrier(std::span<const ResourceBarrierDescriptor> barriers) noexcept {
    vector<D3D12_RESOURCE_BARRIER> rawBarriers;
    rawBarriers.reserve(barriers.size());
    for (const auto& v : barriers) {
        if (const auto* bb = std::get_if<BarrierBufferDescriptor>(&v)) {
            auto buf = CastD3D12Object(bb->Target);
            D3D12_RESOURCE_BARRIER raw{};
            if (bb->Before == BufferState::UnorderedAccess && bb->After == BufferState::UnorderedAccess) {
                raw.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                raw.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                raw.UAV.pResource = buf->_buf.Get();
            } else {
                raw.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                raw.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                raw.Transition.pResource = buf->_buf.Get();
                raw.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                raw.Transition.StateBefore = MapType(bb->Before);
                raw.Transition.StateAfter = MapType(bb->After);
                if (raw.Transition.StateBefore == raw.Transition.StateAfter) {
                    continue;
                }
            }
            rawBarriers.push_back(raw);
        } else if (const auto* tb = std::get_if<BarrierTextureDescriptor>(&v)) {
            auto tex = CastD3D12Object(tb->Target);
            D3D12_RESOURCE_BARRIER raw{};
            if (tb->Before == TextureState::UnorderedAccess && tb->After == TextureState::UnorderedAccess) {
                raw.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                raw.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                raw.UAV.pResource = tex->_tex.Get();
            } else {
                if (tb->Before == tb->After) {
                    continue;
                }
                raw.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                raw.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                raw.Transition.pResource = tex->_tex.Get();
                if (tb->IsSubresourceBarrier) {
                    raw.Transition.Subresource = D3D12CalcSubresource(
                        tb->Range.BaseMipLevel,
                        tb->Range.BaseArrayLayer,
                        0,
                        tex->_rawDesc.MipLevels,
                        tex->_rawDesc.DepthOrArraySize);
                } else {
                    raw.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                }
                raw.Transition.StateBefore = MapType(tb->Before);
                raw.Transition.StateAfter = MapType(tb->After);
                // D3D12 COMMON 和 PRESENT flag 完全一致
                if (raw.Transition.StateBefore == D3D12_RESOURCE_STATE_COMMON && raw.Transition.StateAfter == D3D12_RESOURCE_STATE_COMMON) {
                    if (tb->Before == TextureState::Present || tb->After == TextureState::Present) {
                        continue;
                    }
                }
                if (raw.Transition.StateBefore == raw.Transition.StateAfter) {
                    continue;
                }
            }
            rawBarriers.push_back(raw);
        } else {
            const auto* ab = std::get_if<BarrierAccelerationStructureDescriptor>(&v);
            RADRAY_ASSERT(ab != nullptr);
            auto as = CastD3D12Object(ab->Target);
            if (ab->Before == BufferState::AccelerationStructureRead && ab->After == BufferState::AccelerationStructureRead) {
                D3D12_RESOURCE_BARRIER raw{};
                raw.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                raw.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                raw.UAV.pResource = as->_buffer.Get();
                rawBarriers.push_back(raw);
            } else {
                RADRAY_ERR_LOG("d3d12 acceleration structure state transition is not supported; use AccelerationStructureRead->AccelerationStructureRead for sync");
            }
        }
    }
    if (!rawBarriers.empty()) {
        _cmdList->ResourceBarrier(static_cast<UINT>(rawBarriers.size()), rawBarriers.data());
    }
}

Nullable<unique_ptr<GraphicsCommandEncoder>> CmdListD3D12::BeginRenderPass(const RenderPassDescriptor& desc) noexcept {
    ComPtr<ID3D12GraphicsCommandList4> cmdList4;
    if (HRESULT hr = _cmdList->QueryInterface(IID_PPV_ARGS(cmdList4.GetAddressOf()));
        FAILED(hr)) {
        RADRAY_ERR_LOG("ID3D12GraphicsCommandList::QueryInterface failed: {} {}", GetErrorName(hr), hr);
        return nullptr;
    }
    vector<D3D12_RENDER_PASS_RENDER_TARGET_DESC> rtDescs;
    rtDescs.reserve(desc.ColorAttachments.size());
    for (const ColorAttachment& color : desc.ColorAttachments) {
        auto v = CastD3D12Object(color.Target);
        D3D12_CLEAR_VALUE clearColor{};
        clearColor.Format = v->_rawFormat;
        clearColor.Color[0] = color.ClearValue.Value[0];
        clearColor.Color[1] = color.ClearValue.Value[1];
        clearColor.Color[2] = color.ClearValue.Value[2];
        clearColor.Color[3] = color.ClearValue.Value[3];
        D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE beginningAccess = MapType(color.Load);
        D3D12_RENDER_PASS_ENDING_ACCESS_TYPE endingAccess = MapType(color.Store);
        auto& rtDesc = rtDescs.emplace_back(D3D12_RENDER_PASS_RENDER_TARGET_DESC{});
        rtDesc.cpuDescriptor = v->_heapView.HandleCpu();
        rtDesc.BeginningAccess.Type = beginningAccess;
        rtDesc.BeginningAccess.Clear.ClearValue = clearColor;
        rtDesc.EndingAccess.Type = endingAccess;
    }
    D3D12_RENDER_PASS_DEPTH_STENCIL_DESC dsDesc{};
    D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* pDsDesc = nullptr;
    if (desc.DepthStencilAttachment.has_value()) {
        const DepthStencilAttachment& depthStencil = desc.DepthStencilAttachment.value();
        auto v = CastD3D12Object(depthStencil.Target);
        D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE depthBeginningAccess = MapType(depthStencil.DepthLoad);
        D3D12_RENDER_PASS_ENDING_ACCESS_TYPE depthEndingAccess = MapType(depthStencil.DepthStore);
        D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE stencilBeginningAccess = MapType(depthStencil.StencilLoad);
        D3D12_RENDER_PASS_ENDING_ACCESS_TYPE stencilEndingAccess = MapType(depthStencil.StencilStore);
        if (!IsStencilFormatDXGI(v->_rawFormat)) {
            stencilBeginningAccess = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS;
            stencilEndingAccess = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS;
        }
        D3D12_CLEAR_VALUE clear{};
        clear.Format = v->_rawFormat;
        clear.DepthStencil.Depth = depthStencil.ClearValue.Depth;
        clear.DepthStencil.Stencil = depthStencil.ClearValue.Stencil;
        dsDesc.cpuDescriptor = v->_heapView.HandleCpu();
        dsDesc.DepthBeginningAccess.Type = depthBeginningAccess;
        dsDesc.DepthBeginningAccess.Clear.ClearValue = clear;
        dsDesc.DepthEndingAccess.Type = depthEndingAccess;
        dsDesc.StencilBeginningAccess.Type = stencilBeginningAccess;
        dsDesc.StencilBeginningAccess.Clear.ClearValue = clear;
        dsDesc.StencilEndingAccess.Type = stencilEndingAccess;
        pDsDesc = &dsDesc;
    }
    cmdList4->BeginRenderPass((UINT32)rtDescs.size(), rtDescs.data(), pDsDesc, D3D12_RENDER_PASS_FLAG_NONE);
    return make_unique<CmdRenderPassD3D12>(this);
}

void CmdListD3D12::EndRenderPass(unique_ptr<GraphicsCommandEncoder> encoder) noexcept {
    ComPtr<ID3D12GraphicsCommandList4> cmdList4;
    if (HRESULT hr = _cmdList->QueryInterface(IID_PPV_ARGS(cmdList4.GetAddressOf()));
        FAILED(hr)) {
        RADRAY_ABORT("ID3D12GraphicsCommandList::QueryInterface failed: {} {}", GetErrorName(hr), hr);
        return;
    }
    cmdList4->EndRenderPass();
    encoder->Destroy();
}

Nullable<unique_ptr<ComputeCommandEncoder>> CmdListD3D12::BeginComputePass() noexcept {
    return make_unique<CmdComputePassD3D12>(this);
}

void CmdListD3D12::EndComputePass(unique_ptr<ComputeCommandEncoder> encoder) noexcept {
    encoder->Destroy();
}

Nullable<unique_ptr<RayTracingCommandEncoder>> CmdListD3D12::BeginRayTracingPass() noexcept {
    if (!_device->GetDetail().IsRayTracingSupported) {
        RADRAY_ERR_LOG("d3d12 ray tracing command encoder is not supported by this device");
        return nullptr;
    }
    if (_type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        RADRAY_ERR_LOG("d3d12 ray tracing requires direct command list, current type={}", _type);
        return nullptr;
    }
    if (QueryCommandList4() == nullptr) {
        return nullptr;
    }
    return make_unique<CmdRayTracingPassD3D12>(this);
}

void CmdListD3D12::EndRayTracingPass(unique_ptr<RayTracingCommandEncoder> encoder) noexcept {
    if (encoder != nullptr) {
        encoder->Destroy();
    }
}

void CmdListD3D12::CopyBufferToBuffer(Buffer* dst_, uint64_t dstOffset, Buffer* src_, uint64_t srcOffset, uint64_t size) noexcept {
    auto src = CastD3D12Object(src_);
    auto dst = CastD3D12Object(dst_);
    _cmdList->CopyBufferRegion(dst->_buf.Get(), dstOffset, src->_buf.Get(), srcOffset, size);
}

void CmdListD3D12::CopyBufferToTexture(Texture* dst_, SubresourceRange dstRange, Buffer* src_, uint64_t srcOffset) noexcept {
    auto src = CastD3D12Object(src_);
    auto dst = CastD3D12Object(dst_);
    const D3D12_RESOURCE_DESC& dstDesc = dst->_rawDesc;

    if (dstRange.MipLevelCount == SubresourceRange::All || dstRange.ArrayLayerCount == SubresourceRange::All) {
        RADRAY_ERR_LOG("d3d12 CopyBufferToTexture requires explicit SubresourceRange count");
        return;
    }

    uint32_t mipLevels = dstRange.MipLevelCount;
    uint32_t layerCount = dstRange.ArrayLayerCount;
    if (mipLevels == 0 || layerCount == 0) {
        RADRAY_ERR_LOG("d3d12 CopyBufferToTexture invalid SubresourceRange count (mipLevels={}, layerCount={})", mipLevels, layerCount);
        return;
    }
    if (dstRange.BaseMipLevel >= dst->_rawDesc.MipLevels ||
        dstRange.BaseMipLevel + mipLevels > dst->_rawDesc.MipLevels) {
        RADRAY_ERR_LOG("d3d12 CopyBufferToTexture mip range out of bounds (base={}, count={}, total={})",
                       dstRange.BaseMipLevel, mipLevels, dst->_rawDesc.MipLevels);
        return;
    }

    uint32_t arraySize = dst->_dimension == TextureDimension::Dim3D ? 1u : dst->_rawDesc.DepthOrArraySize;
    if (dstRange.BaseArrayLayer >= arraySize ||
        dstRange.BaseArrayLayer + layerCount > arraySize) {
        RADRAY_ERR_LOG("d3d12 CopyBufferToTexture array range out of bounds (base={}, count={}, total={})",
                       dstRange.BaseArrayLayer, layerCount, arraySize);
        return;
    }

    uint64_t bufferOffset = srcOffset;
    for (uint32_t mip = 0; mip < mipLevels; ++mip) {
        uint32_t mipLevel = dstRange.BaseMipLevel + mip;
        for (uint32_t layer = 0; layer < layerCount; ++layer) {
            uint32_t arrayLayer = dstRange.BaseArrayLayer + layer;
            UINT subres = D3D12CalcSubresource(mipLevel, arrayLayer, 0, dst->_rawDesc.MipLevels, arraySize);

            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
            UINT64 totalBytes = 0;
            _device->_device->GetCopyableFootprints(&dstDesc, subres, 1, bufferOffset, &footprint, nullptr, nullptr, &totalBytes);

            D3D12_TEXTURE_COPY_LOCATION srcLoc{};
            srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            srcLoc.pResource = src->_buf.Get();
            srcLoc.PlacedFootprint = footprint;

            D3D12_TEXTURE_COPY_LOCATION dstLoc{};
            dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dstLoc.pResource = dst->_tex.Get();
            dstLoc.SubresourceIndex = subres;

            _cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
            bufferOffset = footprint.Offset + totalBytes;
        }
    }
}

void CmdListD3D12::CopyTextureToBuffer(Buffer* dst_, uint64_t dstOffset, Texture* src_, SubresourceRange srcRange) noexcept {
    auto dst = CastD3D12Object(dst_);
    auto src = CastD3D12Object(src_);
    const D3D12_RESOURCE_DESC& srcDesc = src->_rawDesc;

    if (srcRange.MipLevelCount == SubresourceRange::All || srcRange.ArrayLayerCount == SubresourceRange::All) {
        RADRAY_ERR_LOG("d3d12 CopyTextureToBuffer requires explicit SubresourceRange count");
        return;
    }

    uint32_t mipLevels = srcRange.MipLevelCount;
    uint32_t layerCount = srcRange.ArrayLayerCount;
    if (mipLevels == 0 || layerCount == 0) {
        RADRAY_ERR_LOG("d3d12 CopyTextureToBuffer invalid SubresourceRange count (mipLevels={}, layerCount={})", mipLevels, layerCount);
        return;
    }
    if (srcRange.BaseMipLevel >= src->_rawDesc.MipLevels ||
        srcRange.BaseMipLevel + mipLevels > src->_rawDesc.MipLevels) {
        RADRAY_ERR_LOG("d3d12 CopyTextureToBuffer mip range out of bounds (base={}, count={}, total={})",
                       srcRange.BaseMipLevel, mipLevels, src->_rawDesc.MipLevels);
        return;
    }

    uint32_t arraySize = src->_dimension == TextureDimension::Dim3D ? 1u : src->_rawDesc.DepthOrArraySize;
    if (srcRange.BaseArrayLayer >= arraySize ||
        srcRange.BaseArrayLayer + layerCount > arraySize) {
        RADRAY_ERR_LOG("d3d12 CopyTextureToBuffer array range out of bounds (base={}, count={}, total={})",
                       srcRange.BaseArrayLayer, layerCount, arraySize);
        return;
    }

    uint64_t bufferOffset = dstOffset;
    for (uint32_t mip = 0; mip < mipLevels; ++mip) {
        uint32_t mipLevel = srcRange.BaseMipLevel + mip;
        for (uint32_t layer = 0; layer < layerCount; ++layer) {
            uint32_t arrayLayer = srcRange.BaseArrayLayer + layer;
            UINT subres = D3D12CalcSubresource(mipLevel, arrayLayer, 0, src->_rawDesc.MipLevels, arraySize);

            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
            UINT64 totalBytes = 0;
            _device->_device->GetCopyableFootprints(&srcDesc, subres, 1, bufferOffset, &footprint, nullptr, nullptr, &totalBytes);

            D3D12_TEXTURE_COPY_LOCATION srcLoc{};
            srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            srcLoc.pResource = src->_tex.Get();
            srcLoc.SubresourceIndex = subres;

            D3D12_TEXTURE_COPY_LOCATION dstLoc{};
            dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dstLoc.pResource = dst->_buf.Get();
            dstLoc.PlacedFootprint = footprint;

            _cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
            bufferOffset = footprint.Offset + totalBytes;
        }
    }
}

ComPtr<ID3D12GraphicsCommandList4> CmdListD3D12::QueryCommandList4() noexcept {
    ComPtr<ID3D12GraphicsCommandList4> cmdList4;
    if (HRESULT hr = _cmdList->QueryInterface(IID_PPV_ARGS(cmdList4.GetAddressOf()));
        FAILED(hr)) {
        RADRAY_ERR_LOG("ID3D12GraphicsCommandList::QueryInterface(ID3D12GraphicsCommandList4) failed: {} {}", GetErrorName(hr), hr);
        return nullptr;
    }
    return cmdList4;
}

CmdRenderPassD3D12::CmdRenderPassD3D12(CmdListD3D12* cmdList) noexcept
    : _cmdList(cmdList) {}

bool CmdRenderPassD3D12::IsValid() const noexcept {
    return _cmdList != nullptr;
}

void CmdRenderPassD3D12::Destroy() noexcept {
    _cmdList = nullptr;
}

CommandBuffer* CmdRenderPassD3D12::GetCommandBuffer() const noexcept {
    return _cmdList;
}

void CmdRenderPassD3D12::SetViewport(Viewport viewport) noexcept {
    D3D12_VIEWPORT vp{};
    vp.TopLeftX = viewport.X;
    vp.TopLeftY = viewport.Y;
    vp.Width = viewport.Width;
    vp.Height = viewport.Height;
    vp.MinDepth = viewport.MinDepth;
    vp.MaxDepth = viewport.MaxDepth;
    _cmdList->_cmdList->RSSetViewports(1, &vp);
}

void CmdRenderPassD3D12::SetScissor(Rect scissor) noexcept {
    D3D12_RECT rect{};
    rect.left = scissor.X;
    rect.top = scissor.Y;
    rect.right = scissor.X + scissor.Width;
    rect.bottom = scissor.Y + scissor.Height;
    _cmdList->_cmdList->RSSetScissorRects(1, &rect);
}

void CmdRenderPassD3D12::BindVertexBuffer(std::span<const VertexBufferView> vbv) noexcept {
    if (_boundPso == nullptr) {
        _boundVbvs.clear();
        _boundVbvs.insert(_boundVbvs.end(), vbv.begin(), vbv.end());
    } else {
        const auto& strides = _boundPso->_arrayStrides;
        vector<D3D12_VERTEX_BUFFER_VIEW> rawVbvs;
        rawVbvs.reserve(vbv.size());
        for (size_t index = 0; index < std::min(vbv.size(), strides.size()); index++) {
            const VertexBufferView& i = vbv[index];
            D3D12_VERTEX_BUFFER_VIEW& raw = rawVbvs.emplace_back();
            auto buf = CastD3D12Object(i.Target);
            raw.BufferLocation = buf->_gpuAddr + i.Offset;
            raw.SizeInBytes = (UINT)(buf->_rawDesc.Width - i.Offset);
            raw.StrideInBytes = (UINT)strides[index];
        }
        _cmdList->_cmdList->IASetVertexBuffers(0, (UINT)rawVbvs.size(), rawVbvs.data());
    }
}

void CmdRenderPassD3D12::BindIndexBuffer(IndexBufferView ibv) noexcept {
    if (ibv.Stride != 2 && ibv.Stride != 4) {
        RADRAY_ERR_LOG("d3d12 index buffer stride must be 2 or 4 bytes, got {}", ibv.Stride);
        return;
    }
    auto buf = CastD3D12Object(ibv.Target);
    D3D12_INDEX_BUFFER_VIEW view{};
    view.BufferLocation = buf->_gpuAddr + ibv.Offset;
    view.SizeInBytes = (UINT)buf->_rawDesc.Width - ibv.Offset;
    view.Format = ibv.Stride == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
    _cmdList->_cmdList->IASetIndexBuffer(&view);
}

void CmdRenderPassD3D12::BindRootSignature(RootSignature* rootSig) noexcept {
    auto rs = CastD3D12Object(rootSig);
    _cmdList->_cmdList->SetGraphicsRootSignature(rs->_rootSig.Get());
    _boundRootSig = rs;
}

void CmdRenderPassD3D12::BindGraphicsPipelineState(GraphicsPipelineState* pso) noexcept {
    auto ps = CastD3D12Object(pso);
    _cmdList->_cmdList->SetPipelineState(ps->_pso.Get());
    _cmdList->_cmdList->IASetPrimitiveTopology(ps->_topo);
    _boundPso = ps;
    if (!_boundVbvs.empty()) {
        this->BindVertexBuffer(_boundVbvs);
        _boundVbvs.clear();
    }
}

void CmdRenderPassD3D12::BindDescriptorSet(DescriptorSetIndex setIndex, DescriptorSet* set) noexcept {
    _BindDescriptorSetD3D12(_cmdList, _boundRootSig, setIndex, set, true);
}

void CmdRenderPassD3D12::PushConstants(BindingParameterId id, const void* data, uint32_t size) noexcept {
    _PushConstantsD3D12(_cmdList, _boundRootSig, id, data, size, true);
}

void CmdRenderPassD3D12::BindBindlessArray(DescriptorSetIndex set, BindlessArray* array) noexcept {
    _BindBindlessArrayD3D12(_cmdList, _boundRootSig, set, array, true);
}

void CmdRenderPassD3D12::Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) noexcept {
    _cmdList->_cmdList->DrawInstanced(vertexCount, instanceCount, firstVertex, firstInstance);
}

void CmdRenderPassD3D12::DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) noexcept {
    _cmdList->_cmdList->DrawIndexedInstanced(indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

CmdComputePassD3D12::CmdComputePassD3D12(CmdListD3D12* cmdList) noexcept
    : _cmdList(cmdList) {}

bool CmdComputePassD3D12::IsValid() const noexcept {
    return _cmdList != nullptr;
}

void CmdComputePassD3D12::Destroy() noexcept {
    _cmdList = nullptr;
}

CommandBuffer* CmdComputePassD3D12::GetCommandBuffer() const noexcept {
    return _cmdList;
}

void CmdComputePassD3D12::BindRootSignature(RootSignature* rootSig) noexcept {
    auto rs = CastD3D12Object(rootSig);
    _cmdList->_cmdList->SetComputeRootSignature(rs->_rootSig.Get());
    _boundRootSig = rs;
}

void CmdComputePassD3D12::BindDescriptorSet(DescriptorSetIndex setIndex, DescriptorSet* set) noexcept {
    _BindDescriptorSetD3D12(_cmdList, _boundRootSig, setIndex, set, false);
}

void CmdComputePassD3D12::PushConstants(BindingParameterId id, const void* data, uint32_t size) noexcept {
    _PushConstantsD3D12(_cmdList, _boundRootSig, id, data, size, false);
}

void CmdComputePassD3D12::BindBindlessArray(DescriptorSetIndex set, BindlessArray* array) noexcept {
    _BindBindlessArrayD3D12(_cmdList, _boundRootSig, set, array, false);
}

void CmdComputePassD3D12::BindComputePipelineState(ComputePipelineState* pso) noexcept {
    auto ps = CastD3D12Object(pso);
    _cmdList->_cmdList->SetPipelineState(ps->_pso.Get());
}

void CmdComputePassD3D12::Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) noexcept {
    _cmdList->_cmdList->Dispatch(groupCountX, groupCountY, groupCountZ);
}

CmdRayTracingPassD3D12::CmdRayTracingPassD3D12(CmdListD3D12* cmdList) noexcept
    : _cmdList(cmdList) {}

bool CmdRayTracingPassD3D12::IsValid() const noexcept {
    return _cmdList != nullptr;
}

void CmdRayTracingPassD3D12::Destroy() noexcept {
    _cmdList = nullptr;
    _boundRootSig = nullptr;
    _boundRtPso = nullptr;
}

CommandBuffer* CmdRayTracingPassD3D12::GetCommandBuffer() const noexcept {
    return _cmdList;
}

void CmdRayTracingPassD3D12::BindRootSignature(RootSignature* rootSig) noexcept {
    auto rs = CastD3D12Object(rootSig);
    _cmdList->_cmdList->SetComputeRootSignature(rs->_rootSig.Get());
    _boundRootSig = rs;
}

void CmdRayTracingPassD3D12::BindDescriptorSet(DescriptorSetIndex setIndex, DescriptorSet* set) noexcept {
    _BindDescriptorSetD3D12(_cmdList, _boundRootSig, setIndex, set, false);
}

void CmdRayTracingPassD3D12::PushConstants(BindingParameterId id, const void* data, uint32_t size) noexcept {
    _PushConstantsD3D12(_cmdList, _boundRootSig, id, data, size, false);
}

void CmdRayTracingPassD3D12::BindBindlessArray(DescriptorSetIndex set, BindlessArray* array) noexcept {
    _BindBindlessArrayD3D12(_cmdList, _boundRootSig, set, array, false);
}

void CmdRayTracingPassD3D12::BuildBottomLevelAS(const BuildBottomLevelASDescriptor& desc) noexcept {
    auto cmdList4 = _cmdList->QueryCommandList4();
    if (cmdList4 == nullptr) {
        return;
    }
    auto target = CastD3D12Object(desc.Target);
    auto scratch = CastD3D12Object(desc.ScratchBuffer);
    if (target->_desc.Type != AccelerationStructureType::BottomLevel) {
        RADRAY_ERR_LOG("BuildBottomLevelAS target type mismatch");
        return;
    }
    if (desc.Mode == AccelerationStructureBuildMode::Update &&
        !target->_desc.Flags.HasFlag(AccelerationStructureBuildFlag::AllowUpdate)) {
        RADRAY_ERR_LOG("BuildBottomLevelAS update requested without AllowUpdate flag");
        return;
    }

    vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometries;
    geometries.reserve(desc.Geometries.size());
    for (const auto& geom : desc.Geometries) {
        D3D12_RAYTRACING_GEOMETRY_DESC g{};
        g.Flags = geom.Opaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
        if (const auto* tri = std::get_if<RayTracingTrianglesDescriptor>(&geom.Geometry)) {
            g.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            g.Triangles.VertexFormat = MapType(tri->VertexFmt);
            g.Triangles.VertexCount = tri->VertexCount;
            g.Triangles.VertexBuffer.StartAddress = CastD3D12Object(tri->VertexBuffer)->_gpuAddr + tri->VertexOffset;
            g.Triangles.VertexBuffer.StrideInBytes = tri->VertexStride;
            g.Triangles.Transform3x4 = tri->TransformBuffer ? (CastD3D12Object(tri->TransformBuffer)->_gpuAddr + tri->TransformOffset) : 0;
            if (tri->IndexBuffer != nullptr) {
                g.Triangles.IndexFormat = tri->IndexFmt == IndexFormat::UINT16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
                g.Triangles.IndexCount = tri->IndexCount;
                g.Triangles.IndexBuffer = CastD3D12Object(tri->IndexBuffer)->_gpuAddr + tri->IndexOffset;
            } else {
                g.Triangles.IndexFormat = DXGI_FORMAT_UNKNOWN;
                g.Triangles.IndexCount = 0;
                g.Triangles.IndexBuffer = 0;
            }
        } else {
            const auto* aabb = std::get_if<RayTracingAABBsDescriptor>(&geom.Geometry);
            RADRAY_ASSERT(aabb != nullptr);
            g.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
            g.AABBs.AABBCount = aabb->Count;
            g.AABBs.AABBs.StrideInBytes = aabb->Stride;
            g.AABBs.AABBs.StartAddress = CastD3D12Object(aabb->Target)->_gpuAddr + aabb->Offset;
        }
        geometries.push_back(g);
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.NumDescs = static_cast<UINT>(geometries.size());
    inputs.pGeometryDescs = geometries.data();
    inputs.Flags = MapBuildFlags(target->_desc.Flags);
    if (desc.Mode == AccelerationStructureBuildMode::Update) {
        inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
    }

    ComPtr<ID3D12Device5> device5;
    if (HRESULT hr = _cmdList->_device->_device->QueryInterface(IID_PPV_ARGS(device5.GetAddressOf()));
        FAILED(hr)) {
        RADRAY_ERR_LOG("ID3D12Device::QueryInterface(ID3D12Device5) failed: {} {}", GetErrorName(hr), hr);
        return;
    }
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild{};
    device5->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);
    if (prebuild.ResultDataMaxSizeInBytes == 0) {
        RADRAY_ERR_LOG("GetRaytracingAccelerationStructurePrebuildInfo returned invalid result size for BLAS");
        return;
    }
    if (target->_asSize < prebuild.ResultDataMaxSizeInBytes) {
        RADRAY_ERR_LOG("BLAS target AS buffer too small: need={}, actual={}", prebuild.ResultDataMaxSizeInBytes, target->_asSize);
        return;
    }
    if (desc.ScratchSize < prebuild.ScratchDataSizeInBytes) {
        RADRAY_ERR_LOG("BLAS scratch buffer too small: need={}, actual={}", prebuild.ScratchDataSizeInBytes, desc.ScratchSize);
        return;
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
    build.Inputs = inputs;
    build.DestAccelerationStructureData = target->_gpuAddr;
    build.ScratchAccelerationStructureData = scratch->_gpuAddr + desc.ScratchOffset;
    if (desc.Mode == AccelerationStructureBuildMode::Update) {
        build.SourceAccelerationStructureData = target->_gpuAddr;
    }
    cmdList4->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
}

void CmdRayTracingPassD3D12::BuildTopLevelAS(const BuildTopLevelASDescriptor& desc) noexcept {
    auto cmdList4 = _cmdList->QueryCommandList4();
    if (cmdList4 == nullptr) {
        return;
    }
    auto target = CastD3D12Object(desc.Target);
    auto scratch = CastD3D12Object(desc.ScratchBuffer);
    if (target->_desc.Type != AccelerationStructureType::TopLevel) {
        RADRAY_ERR_LOG("BuildTopLevelAS target type mismatch");
        return;
    }
    if (desc.Mode == AccelerationStructureBuildMode::Update &&
        !target->_desc.Flags.HasFlag(AccelerationStructureBuildFlag::AllowUpdate)) {
        RADRAY_ERR_LOG("BuildTopLevelAS update requested without AllowUpdate flag");
        return;
    }

    const uint64_t instanceBufferSize = uint64_t(desc.Instances.size()) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
    ComPtr<ID3D12Resource> instanceBuffer;
    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC resDesc{};
    resDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resDesc.Alignment = 0;
    resDesc.Width = Align(instanceBufferSize, 256ull);
    resDesc.Height = 1;
    resDesc.DepthOrArraySize = 1;
    resDesc.MipLevels = 1;
    resDesc.Format = DXGI_FORMAT_UNKNOWN;
    resDesc.SampleDesc.Count = 1;
    resDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (HRESULT hr = _cmdList->_device->_device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &resDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(instanceBuffer.GetAddressOf()));
        FAILED(hr)) {
        RADRAY_ERR_LOG("CreateCommittedResource for TLAS instances failed: {} {}", GetErrorName(hr), hr);
        return;
    }
    _cmdList->_keepAliveResources.push_back(instanceBuffer);

    auto* mapped = static_cast<D3D12_RAYTRACING_INSTANCE_DESC*>(nullptr);
    D3D12_RANGE mapRange{0, static_cast<SIZE_T>(instanceBufferSize)};
    if (HRESULT hr = instanceBuffer->Map(0, &mapRange, reinterpret_cast<void**>(&mapped));
        FAILED(hr)) {
        RADRAY_ERR_LOG("Map TLAS instance buffer failed: {} {}", GetErrorName(hr), hr);
        return;
    }
    for (size_t i = 0; i < desc.Instances.size(); i++) {
        const auto& src = desc.Instances[i];
        auto& dst = mapped[i];
        dst.Transform[0][0] = src.Transform(0, 0);
        dst.Transform[0][1] = src.Transform(0, 1);
        dst.Transform[0][2] = src.Transform(0, 2);
        dst.Transform[0][3] = src.Transform(0, 3);
        dst.Transform[1][0] = src.Transform(1, 0);
        dst.Transform[1][1] = src.Transform(1, 1);
        dst.Transform[1][2] = src.Transform(1, 2);
        dst.Transform[1][3] = src.Transform(1, 3);
        dst.Transform[2][0] = src.Transform(2, 0);
        dst.Transform[2][1] = src.Transform(2, 1);
        dst.Transform[2][2] = src.Transform(2, 2);
        dst.Transform[2][3] = src.Transform(2, 3);
        dst.InstanceID = src.InstanceID;
        dst.InstanceMask = src.InstanceMask;
        dst.InstanceContributionToHitGroupIndex = src.InstanceContributionToHitGroupIndex;
        dst.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
        if (src.ForceOpaque) dst.Flags |= D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE;
        if (src.ForceNoOpaque) dst.Flags |= D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_NON_OPAQUE;
        dst.AccelerationStructure = CastD3D12Object(src.Blas)->_gpuAddr;
    }
    D3D12_RANGE unmapRange{0, static_cast<SIZE_T>(instanceBufferSize)};
    instanceBuffer->Unmap(0, &unmapRange);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.NumDescs = static_cast<UINT>(desc.Instances.size());
    inputs.InstanceDescs = instanceBuffer->GetGPUVirtualAddress();
    inputs.Flags = MapBuildFlags(target->_desc.Flags);
    if (desc.Mode == AccelerationStructureBuildMode::Update) {
        inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
    }

    ComPtr<ID3D12Device5> device5;
    if (HRESULT hr = _cmdList->_device->_device->QueryInterface(IID_PPV_ARGS(device5.GetAddressOf()));
        FAILED(hr)) {
        RADRAY_ERR_LOG("ID3D12Device::QueryInterface(ID3D12Device5) failed: {} {}", GetErrorName(hr), hr);
        return;
    }
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild{};
    device5->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);
    if (prebuild.ResultDataMaxSizeInBytes == 0) {
        RADRAY_ERR_LOG("GetRaytracingAccelerationStructurePrebuildInfo returned invalid result size for TLAS");
        return;
    }
    if (target->_asSize < prebuild.ResultDataMaxSizeInBytes) {
        RADRAY_ERR_LOG("TLAS target AS buffer too small: need={}, actual={}", prebuild.ResultDataMaxSizeInBytes, target->_asSize);
        return;
    }
    if (desc.ScratchSize < prebuild.ScratchDataSizeInBytes) {
        RADRAY_ERR_LOG("TLAS scratch buffer too small: need={}, actual={}", prebuild.ScratchDataSizeInBytes, desc.ScratchSize);
        return;
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
    build.Inputs = inputs;
    build.DestAccelerationStructureData = target->_gpuAddr;
    build.ScratchAccelerationStructureData = scratch->_gpuAddr + desc.ScratchOffset;
    if (desc.Mode == AccelerationStructureBuildMode::Update) {
        build.SourceAccelerationStructureData = target->_gpuAddr;
    }
    cmdList4->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
}

void CmdRayTracingPassD3D12::BindRayTracingPipelineState(RayTracingPipelineState* pso) noexcept {
    _boundRtPso = CastD3D12Object(pso);
}

void CmdRayTracingPassD3D12::TraceRays(const TraceRaysDescriptor& desc) noexcept {
    if (_boundRtPso == nullptr) {
        RADRAY_ERR_LOG("bind ray tracing pipeline state before TraceRays");
        return;
    }
    TraceRaysDescriptor resolved = desc;
    if (desc.Sbt != nullptr) {
        auto regions = desc.Sbt->GetRegions();
        resolved.RayGen = regions.RayGen;
        resolved.Miss = regions.Miss;
        resolved.HitGroup = regions.HitGroup;
        resolved.Callable = regions.Callable;
    }
    auto cmdList4 = _cmdList->QueryCommandList4();
    if (cmdList4 == nullptr) {
        return;
    }

    auto toRegion = [](const ShaderBindingTableRegion& v) {
        auto* buf = CastD3D12Object(v.Target);
        D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE r{};
        r.StartAddress = buf->_gpuAddr + v.Offset;
        r.SizeInBytes = v.Size;
        r.StrideInBytes = v.Stride;
        return r;
    };

    D3D12_DISPATCH_RAYS_DESC dispatch{};
    dispatch.RayGenerationShaderRecord.StartAddress = CastD3D12Object(resolved.RayGen.Target)->_gpuAddr + resolved.RayGen.Offset;
    dispatch.RayGenerationShaderRecord.SizeInBytes = resolved.RayGen.Size;
    dispatch.MissShaderTable = toRegion(resolved.Miss);
    dispatch.HitGroupTable = toRegion(resolved.HitGroup);
    if (resolved.Callable.has_value()) {
        dispatch.CallableShaderTable = toRegion(resolved.Callable.value());
    }
    dispatch.Width = resolved.Width;
    dispatch.Height = resolved.Height;
    dispatch.Depth = resolved.Depth;

    cmdList4->SetPipelineState1(_boundRtPso->_stateObject.Get());
    cmdList4->DispatchRays(&dispatch);
}

SwapChainD3D12::SwapChainD3D12(
    DeviceD3D12* device,
    ComPtr<IDXGISwapChain3> swapchain,
    const SwapChainDescriptor& desc) noexcept
    : _device(device),
      _presentQueue(CastD3D12Object(desc.PresentQueue)),
      _swapchain(std::move(swapchain)),
      _nativeHandler(desc.NativeHandler),
      _mode(desc.PresentMode),
      _reqFormat(desc.Format) {}

SwapChainD3D12::~SwapChainD3D12() noexcept {
    _frames.clear();
    _currentBackBufferIndex = std::numeric_limits<uint32_t>::max();
    _swapchain = nullptr;
    if (_frameLatencyEvent) {
        CloseHandle(_frameLatencyEvent);
        _frameLatencyEvent = nullptr;
    }
}

bool SwapChainD3D12::IsValid() const noexcept {
    return _swapchain != nullptr;
}

void SwapChainD3D12::Destroy() noexcept {
    _frames.clear();
    _currentBackBufferIndex = std::numeric_limits<uint32_t>::max();
    _swapchain = nullptr;
    if (_frameLatencyEvent) {
        CloseHandle(_frameLatencyEvent);
        _frameLatencyEvent = nullptr;
    }
}

SwapChainAcquireResult SwapChainD3D12::AcquireNext(uint64_t timeoutMs) noexcept {
    SwapChainAcquireResult result{};
    if (_swapchain == nullptr || _frameLatencyEvent == nullptr) {
        _currentBackBufferIndex = std::numeric_limits<uint32_t>::max();
        return result;
    }
    DWORD milliseconds;
    if (timeoutMs == std::numeric_limits<uint64_t>::max()) {
        milliseconds = INFINITE;
    } else if (timeoutMs > static_cast<uint64_t>(INFINITE) - 1) {
        milliseconds = INFINITE - 1;
    } else {
        milliseconds = static_cast<DWORD>(timeoutMs);
    }
    const DWORD waitResult = ::WaitForSingleObjectEx(_frameLatencyEvent, milliseconds, false);
    if (waitResult == WAIT_OBJECT_0) {
        const auto curr = static_cast<uint32_t>(_swapchain->GetCurrentBackBufferIndex());
        _currentBackBufferIndex = curr;
        result.Status = SwapChainStatus::Success;
        result.NativeStatusCode = 0;
        result.BackBuffer = _frames[curr].image.get();
        result.BackBufferIndex = curr;
        return result;
    } else if (waitResult == WAIT_TIMEOUT) {
        _currentBackBufferIndex = std::numeric_limits<uint32_t>::max();
        result.Status = SwapChainStatus::RetryLater;
        result.NativeStatusCode = static_cast<int64_t>(waitResult);
        return result;
    } else {
        _currentBackBufferIndex = std::numeric_limits<uint32_t>::max();
        result.Status = SwapChainStatus::Error;
        result.NativeStatusCode = static_cast<int64_t>(waitResult);
        return result;
    }
}

SwapChainPresentResult SwapChainD3D12::Present(SwapChainSyncObject* waitToPresent) noexcept {
    (void)waitToPresent;
    SwapChainPresentResult result{};
    if (_swapchain == nullptr) {
        result.NativeStatusCode = static_cast<int64_t>(E_POINTER);
        result.Status = SwapChainStatus::Error;
        return result;
    }
    UINT syncInterval = 0;
    UINT presentFlags = 0;
    switch (_mode) {
        case PresentMode::FIFO: {
            syncInterval = 1;
            presentFlags = 0;  // FIFO 使用阻塞 present，保证 VSync 语义
            break;
        }
        case PresentMode::Mailbox: {
            syncInterval = 0;
            presentFlags = 0;
            break;
        }
        case PresentMode::Immediate: {
            syncInterval = 0;
            presentFlags = _device->_isAllowTearing ? DXGI_PRESENT_ALLOW_TEARING : 0;
            break;
        }
    }
    const HRESULT hr = _swapchain->Present(syncInterval, presentFlags);
    result.NativeStatusCode = static_cast<int64_t>(hr);
    if (SUCCEEDED(hr)) {
        result.Status = SwapChainStatus::Success;
        return result;
    }
    if (hr == DXGI_ERROR_DEVICE_REMOVED ||
        hr == DXGI_ERROR_DEVICE_RESET ||
        hr == DXGI_ERROR_ACCESS_LOST ||
        hr == DXGI_ERROR_ACCESS_DENIED ||
        hr == DXGI_ERROR_INVALID_CALL) {
        RADRAY_WARN_LOG("IDXGISwapChain::Present requires recreate: {} {}", GetErrorName(hr), hr);
        result.Status = SwapChainStatus::RequireRecreate;
        return result;
    }
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
        RADRAY_ERR_LOG("IDXGISwapChain::Present returned unexpected DXGI_ERROR_WAS_STILL_DRAWING");
        result.Status = SwapChainStatus::Error;
        return result;
    }
    RADRAY_ERR_LOG("IDXGISwapChain::Present failed: {} {}", GetErrorName(hr), hr);
    result.Status = SwapChainStatus::Error;
    return result;
}

Nullable<Texture*> SwapChainD3D12::GetCurrentBackBuffer() const noexcept {
    if (_currentBackBufferIndex >= _frames.size()) {
        return nullptr;
    }
    return _frames[_currentBackBufferIndex].image.get();
}

uint32_t SwapChainD3D12::GetCurrentBackBufferIndex() const noexcept {
    return _currentBackBufferIndex;
}

uint32_t SwapChainD3D12::GetBackBufferCount() const noexcept {
    return static_cast<uint32_t>(_frames.size());
}

SwapChainDescriptor SwapChainD3D12::GetDesc() const noexcept {
    DXGI_SWAP_CHAIN_DESC1 desc;
    _swapchain->GetDesc1(&desc);
    return SwapChainDescriptor{
        _presentQueue,
        _nativeHandler,
        desc.Width,
        desc.Height,
        desc.BufferCount,
        _reqFormat,
        _mode};
}

BufferD3D12::BufferD3D12(
    DeviceD3D12* device,
    ComPtr<ID3D12Resource> buf,
    ComPtr<D3D12MA::Allocation> alloc) noexcept
    : _device(device),
      _buf(std::move(buf)),
      _alloc(std::move(alloc)) {
    _rawDesc = _buf->GetDesc();
    _gpuAddr = _buf->GetGPUVirtualAddress();
}

bool BufferD3D12::IsValid() const noexcept {
    return _buf != nullptr;
}

void BufferD3D12::Destroy() noexcept {
    _buf = nullptr;
    _alloc = nullptr;
}

void* BufferD3D12::Map(uint64_t offset, uint64_t size) noexcept {
    D3D12_RANGE range{offset, offset + size};
    void* ptr = nullptr;
    if (HRESULT hr = _buf->Map(0, &range, &ptr);
        FAILED(hr)) {
        RADRAY_ABORT("ID3D12Resource::Map failed: {} {}", GetErrorName(hr), hr);
    }
    return ptr;
}

void BufferD3D12::Unmap(uint64_t offset, uint64_t size) noexcept {
    D3D12_RANGE range{offset, offset + size};
    _buf->Unmap(0, &range);
}

void BufferD3D12::SetDebugName(std::string_view name) noexcept {
    _name = string(name);
    SetObjectName(name, _buf.Get(), _alloc.Get());
}

BufferDescriptor BufferD3D12::GetDesc() const noexcept {
    return BufferDescriptor{
        _reqSize,
        _memory,
        _usage,
        _hints};
}

BufferViewD3D12::BufferViewD3D12(
    DeviceD3D12* device,
    BufferD3D12* buffer,
    CpuDescriptorHeapViewRAII heapView) noexcept
    : _device(device),
      _buffer(buffer),
      _heapView(std::move(heapView)) {}

bool BufferViewD3D12::IsValid() const noexcept {
    return _heapView.IsValid();
}

void BufferViewD3D12::Destroy() noexcept {
    _heapView.Destroy();
}

void BufferViewD3D12::SetDebugName(std::string_view name) noexcept {
    RADRAY_UNUSED(name);
}

TextureD3D12::TextureD3D12(
    DeviceD3D12* device,
    ComPtr<ID3D12Resource> tex,
    ComPtr<D3D12MA::Allocation> alloc) noexcept
    : _device(device),
      _tex(std::move(tex)),
      _alloc(std::move(alloc)) {
    _rawDesc = _tex->GetDesc();
}

bool TextureD3D12::IsValid() const noexcept {
    return _tex != nullptr;
}

void TextureD3D12::Destroy() noexcept {
    _tex = nullptr;
    _alloc = nullptr;
}

void TextureD3D12::SetDebugName(std::string_view name) noexcept {
    _name = string(name);
    SetObjectName(name, _tex.Get(), _alloc.Get());
}

TextureDescriptor TextureD3D12::GetDesc() const noexcept {
    return TextureDescriptor{
        _dimension,
        static_cast<uint32_t>(_rawDesc.Width),
        static_cast<uint32_t>(_rawDesc.Height),
        static_cast<uint32_t>(_rawDesc.DepthOrArraySize),
        _rawDesc.MipLevels,
        _rawDesc.SampleDesc.Count,
        _format,
        _memory,
        _usage,
        _hints};
}

TextureViewD3D12::TextureViewD3D12(
    DeviceD3D12* device,
    TextureD3D12* texture,
    CpuDescriptorHeapViewRAII heapView) noexcept
    : _device(device),
      _texture(texture),
      _heapView(std::move(heapView)) {}

bool TextureViewD3D12::IsValid() const noexcept {
    return _heapView.IsValid();
}

void TextureViewD3D12::Destroy() noexcept {
    _heapView.Destroy();
}

void TextureViewD3D12::SetDebugName(std::string_view name) noexcept {
    RADRAY_UNUSED(name);
}

bool Dxil::IsValid() const noexcept {
    return !_dxil.empty();
}

void Dxil::Destroy() noexcept {
    _dxil.clear();
    _dxil.shrink_to_fit();
    _reflection.reset();
    _stages = ShaderStage::UNKNOWN;
}

D3D12_SHADER_BYTECODE Dxil::ToByteCode() const noexcept {
    return {_dxil.data(), _dxil.size()};
}

RootSigD3D12::RootSigD3D12(
    DeviceD3D12* device,
    ComPtr<ID3D12RootSignature> rootSig) noexcept
    : _device(device),
      _rootSig(std::move(rootSig)) {}

bool RootSigD3D12::IsValid() const noexcept {
    return _rootSig != nullptr;
}

void RootSigD3D12::Destroy() noexcept {
    _rootSig = nullptr;
    _bindingLayout = {};
    _descriptorSetLayouts.clear();
    _bindlessSetLayouts.clear();
    _staticSamplerLayouts.clear();
    _pushConstantRanges.clear();
    _parameters.clear();
    _descriptorSets.clear();
    _bindlessSets.clear();
    _resourceTables.clear();
    _samplerTables.clear();
}

void RootSigD3D12::SetDebugName(std::string_view name) noexcept {
    SetObjectName(name, _rootSig.Get());
}

Nullable<const RootSigD3D12::ParameterBindingInfo*> RootSigD3D12::FindParameterInfo(BindingParameterId id) const noexcept {
    if (id.Value >= _parameters.size()) {
        return nullptr;
    }
    return &_parameters[id.Value];
}

std::span<const BindingParameterLayout> RootSigD3D12::GetDescriptorSetLayout(DescriptorSetIndex set) const noexcept {
    if (set.Value >= _descriptorSetLayouts.size()) {
        return {};
    }
    return _descriptorSetLayouts[set.Value];
}

Nullable<const RootSigD3D12::DescriptorSetInfo*> RootSigD3D12::FindDescriptorSetInfo(DescriptorSetIndex set) const noexcept {
    if (set.Value >= _descriptorSets.size()) {
        return nullptr;
    }
    return &_descriptorSets[set.Value];
}

Nullable<const RootSigD3D12::BindlessSetInfo*> RootSigD3D12::FindBindlessSetInfo(DescriptorSetIndex set) const noexcept {
    for (const auto& bindlessSet : _bindlessSets) {
        if (bindlessSet.SetIndex == set) {
            return &bindlessSet;
        }
    }
    return nullptr;
}

GraphicsPsoD3D12::GraphicsPsoD3D12(
    DeviceD3D12* device,
    ComPtr<ID3D12PipelineState> pso,
    vector<uint64_t> arrayStrides,
    D3D12_PRIMITIVE_TOPOLOGY topo) noexcept
    : _device(device),
      _pso(std::move(pso)),
      _arrayStrides(std::move(arrayStrides)),
      _topo(topo) {}

bool GraphicsPsoD3D12::IsValid() const noexcept {
    return _pso != nullptr;
}

void GraphicsPsoD3D12::Destroy() noexcept {
    _pso = nullptr;
}

void GraphicsPsoD3D12::SetDebugName(std::string_view name) noexcept {
    SetObjectName(name, _pso.Get());
}

ComputePsoD3D12::ComputePsoD3D12(
    DeviceD3D12* device,
    ComPtr<ID3D12PipelineState> pso) noexcept
    : _device(device),
      _pso(std::move(pso)) {}

bool ComputePsoD3D12::IsValid() const noexcept {
    return _pso != nullptr;
}

void ComputePsoD3D12::Destroy() noexcept {
    _pso = nullptr;
}

void ComputePsoD3D12::SetDebugName(std::string_view name) noexcept {
    SetObjectName(name, _pso.Get());
}

AccelerationStructureD3D12::AccelerationStructureD3D12(
    DeviceD3D12* device,
    ComPtr<ID3D12Resource> buffer,
    ComPtr<D3D12MA::Allocation> alloc,
    const AccelerationStructureDescriptor& desc,
    uint64_t asSize) noexcept
    : _device(device),
      _buffer(std::move(buffer)),
      _alloc(std::move(alloc)),
      _desc(desc),
      _asSize(asSize) {
    if (_buffer != nullptr) {
        _gpuAddr = _buffer->GetGPUVirtualAddress();
    }
}

bool AccelerationStructureD3D12::IsValid() const noexcept {
    return _buffer != nullptr;
}

void AccelerationStructureD3D12::Destroy() noexcept {
    _buffer = nullptr;
    _alloc = nullptr;
    _gpuAddr = 0;
}

void AccelerationStructureD3D12::SetDebugName(std::string_view name) noexcept {
    _name = string(name);
    SetObjectName(name, _buffer.Get(), _alloc.Get());
}

AccelerationStructureViewD3D12::AccelerationStructureViewD3D12(
    DeviceD3D12* device,
    AccelerationStructureD3D12* target,
    CpuDescriptorHeapViewRAII heapView) noexcept
    : _device(device),
      _target(target),
      _heapView(std::move(heapView)) {}

bool AccelerationStructureViewD3D12::IsValid() const noexcept {
    return _heapView.IsValid() && _target != nullptr;
}

void AccelerationStructureViewD3D12::Destroy() noexcept {
    _heapView.Destroy();
    _target = nullptr;
    _device = nullptr;
}

void AccelerationStructureViewD3D12::SetDebugName(std::string_view name) noexcept {
    RADRAY_UNUSED(name);
}

RayTracingPsoD3D12::RayTracingPsoD3D12(
    DeviceD3D12* device,
    ComPtr<ID3D12StateObject> stateObject,
    ComPtr<ID3D12StateObjectProperties> stateProps,
    RootSigD3D12* rootSig) noexcept
    : _device(device),
      _stateObject(std::move(stateObject)),
      _stateProps(std::move(stateProps)),
      _rootSig(rootSig) {}

bool RayTracingPsoD3D12::IsValid() const noexcept {
    return _stateObject != nullptr && _stateProps != nullptr;
}

void RayTracingPsoD3D12::Destroy() noexcept {
    _stateObject = nullptr;
    _stateProps = nullptr;
    _shaderIdentifiers.clear();
}

void RayTracingPsoD3D12::SetDebugName(std::string_view name) noexcept {
    SetObjectName(name, _stateObject.Get());
}

ShaderBindingTableRequirements RayTracingPsoD3D12::GetShaderBindingTableRequirements() const noexcept {
    return {
        D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES,
        D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES,
        _device == nullptr ? 0u : _device->GetDetail().ShaderTableAlignment};
}

std::optional<vector<byte>> RayTracingPsoD3D12::GetShaderBindingTableHandle(std::string_view shaderName) const noexcept {
    auto it = _shaderIdentifiers.find(string(shaderName));
    if (it == _shaderIdentifiers.end()) {
        return std::nullopt;
    }
    vector<byte> out(D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
    std::memcpy(out.data(), it->second.data(), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
    return out;
}

ShaderBindingTableD3D12::ShaderBindingTableD3D12(
    DeviceD3D12* device,
    RayTracingPsoD3D12* pipeline,
    unique_ptr<Buffer> buffer,
    const ShaderBindingTableDescriptor& desc,
    uint64_t recordStride) noexcept
    : _device(device),
      _pipeline(pipeline),
      _buffer(std::move(buffer)),
      _desc(desc),
      _recordStride(recordStride) {
    _rayGenOffset = 0;
    _missOffset = _recordStride * _desc.RayGenCount;
    _hitGroupOffset = _missOffset + _recordStride * _desc.MissCount;
    _callableOffset = _hitGroupOffset + _recordStride * _desc.HitGroupCount;
}

bool ShaderBindingTableD3D12::IsValid() const noexcept {
    return _device != nullptr && _pipeline != nullptr && _buffer != nullptr;
}

void ShaderBindingTableD3D12::Destroy() noexcept {
    _buffer.reset();
    _pipeline = nullptr;
    _device = nullptr;
    _isBuilt = false;
}

void ShaderBindingTableD3D12::SetDebugName(std::string_view name) noexcept {
    _name = string(name);
    if (_buffer) {
        _buffer->SetDebugName(_name);
    }
}

bool ShaderBindingTableD3D12::Build(std::span<const ShaderBindingTableBuildEntry> entries) noexcept {
    if (!this->IsValid()) {
        return false;
    }
    auto req = _pipeline->GetShaderBindingTableRequirements();
    if (req.HandleSize == 0 || _recordStride < req.HandleSize) {
        RADRAY_ERR_LOG("invalid SBT record stride/handle size");
        return false;
    }
    uint64_t totalSize = _buffer->GetDesc().Size;
    auto* mapped = static_cast<byte*>(_buffer->Map(0, totalSize));
    if (mapped == nullptr) {
        RADRAY_ERR_LOG("failed to map SBT buffer");
        return false;
    }
    std::memset(mapped, 0, static_cast<size_t>(totalSize));
    for (const auto& entry : entries) {
        uint32_t count = 0;
        uint64_t baseOffset = 0;
        switch (entry.Type) {
            case ShaderBindingTableEntryType::RayGen:
                count = _desc.RayGenCount;
                baseOffset = _rayGenOffset;
                break;
            case ShaderBindingTableEntryType::Miss:
                count = _desc.MissCount;
                baseOffset = _missOffset;
                break;
            case ShaderBindingTableEntryType::HitGroup:
                count = _desc.HitGroupCount;
                baseOffset = _hitGroupOffset;
                break;
            case ShaderBindingTableEntryType::Callable:
                count = _desc.CallableCount;
                baseOffset = _callableOffset;
                break;
        }
        if (entry.RecordIndex >= count) {
            RADRAY_ERR_LOG("SBT record index out of range: type={}, index={}, count={}", static_cast<uint32_t>(entry.Type), entry.RecordIndex, count);
            _buffer->Unmap(0, totalSize);
            return false;
        }
        auto handle = _pipeline->GetShaderBindingTableHandle(entry.ShaderName);
        if (!handle.has_value()) {
            RADRAY_ERR_LOG("cannot find shader handle '{}'", entry.ShaderName);
            _buffer->Unmap(0, totalSize);
            return false;
        }
        if (entry.LocalData.size() > _recordStride - req.HandleSize) {
            RADRAY_ERR_LOG("local data too large for SBT record '{}'", entry.ShaderName);
            _buffer->Unmap(0, totalSize);
            return false;
        }
        uint64_t dstOffset = baseOffset + _recordStride * entry.RecordIndex;
        auto* dst = mapped + dstOffset;
        std::memcpy(dst, handle->data(), req.HandleSize);
        if (!entry.LocalData.empty()) {
            std::memcpy(dst + req.HandleSize, entry.LocalData.data(), entry.LocalData.size());
        }
    }
    _buffer->Unmap(0, totalSize);
    _isBuilt = true;
    return true;
}

bool ShaderBindingTableD3D12::IsBuilt() const noexcept {
    return _isBuilt;
}

ShaderBindingTableRegions ShaderBindingTableD3D12::GetRegions() const noexcept {
    ShaderBindingTableRegions regions{};
    regions.RayGen = {_buffer.get(), _rayGenOffset, _recordStride * _desc.RayGenCount, _recordStride};
    regions.Miss = {_buffer.get(), _missOffset, _recordStride * _desc.MissCount, _recordStride};
    regions.HitGroup = {_buffer.get(), _hitGroupOffset, _recordStride * _desc.HitGroupCount, _recordStride};
    if (_desc.CallableCount > 0) {
        regions.Callable = ShaderBindingTableRegion{_buffer.get(), _callableOffset, _recordStride * _desc.CallableCount, _recordStride};
    }
    return regions;
}

DescriptorSetD3D12::DescriptorSetD3D12(
    DeviceD3D12* device,
    RootSigD3D12* rootSig,
    DescriptorSetIndex setIndex,
    GpuDescriptorHeapViewRAII resHeapView,
    GpuDescriptorHeapViewRAII samplerHeapView) noexcept
    : _device(device),
      _rootSig(rootSig),
      _setIndex(setIndex),
      _resHeapView(std::move(resHeapView)),
      _samplerHeapView(std::move(samplerHeapView)) {}

bool DescriptorSetD3D12::IsValid() const noexcept {
    if (_device == nullptr || _rootSig == nullptr || !_rootSig->IsValid()) {
        return false;
    }
    auto setInfoOpt = _rootSig->FindDescriptorSetInfo(_setIndex);
    if (!setInfoOpt.HasValue() || setInfoOpt.Get() == nullptr) {
        return false;
    }
    const auto* setInfo = setInfoOpt.Get();
    if (setInfo->ResourceDescriptorCount > 0 && !_resHeapView.IsValid()) {
        return false;
    }
    if (setInfo->SamplerDescriptorCount > 0 && !_samplerHeapView.IsValid()) {
        return false;
    }
    return true;
}

void DescriptorSetD3D12::Destroy() noexcept {
    _resHeapView.Destroy();
    _samplerHeapView.Destroy();
    _resourceWritten.clear();
    _samplerWritten.clear();
    _name.clear();
    _setIndex = DescriptorSetIndex{};
    _rootSig = nullptr;
    _device = nullptr;
}

void DescriptorSetD3D12::SetDebugName(std::string_view name) noexcept {
    _name = string(name);
}

bool DescriptorSetD3D12::WriteResource(BindingParameterId id, ResourceView* view, uint32_t arrayIndex) noexcept {
    if (!this->IsValid()) {
        RADRAY_ERR_LOG("descriptor set is invalid");
        return false;
    }
    if (view == nullptr) {
        RADRAY_ERR_LOG("resource view is null");
        return false;
    }
    auto infoOpt = _rootSig->FindParameterInfo(id);
    if (!infoOpt.HasValue() || infoOpt.Get() == nullptr) {
        RADRAY_ERR_LOG("binding parameter id {} is out of range", id.Value);
        return false;
    }
    const auto* info = infoOpt.Get();
    if (info->Kind != BindingParameterKind::Resource) {
        RADRAY_ERR_LOG("binding parameter id {} is not a resource parameter", id.Value);
        return false;
    }
    if (info->SetIndex != _setIndex) {
        RADRAY_ERR_LOG(
            "binding parameter id {} belongs to descriptor set {}, not {}",
            id.Value,
            info->SetIndex.Value,
            _setIndex.Value);
        return false;
    }
    if (arrayIndex >= info->DescriptorCount) {
        RADRAY_ERR_LOG("argument out of range '{}' expected: {}, actual: {}", "arrayIndex", info->DescriptorCount, arrayIndex);
        return false;
    }
    auto bindType = _GetResourceViewBindType(view);
    if (!bindType.has_value() || bindType.value() != info->Type) {
        RADRAY_ERR_LOG(
            "resource type mismatch for binding parameter id {} expected: {}, actual: {}",
            id.Value,
            info->Type,
            bindType.has_value() ? bindType.value() : ResourceBindType::UNKNOWN);
        return false;
    }
    if (!_resHeapView.IsValid()) {
        RADRAY_ERR_LOG("descriptor set has no resource descriptor heap slice");
        return false;
    }
    const uint32_t heapIndex = info->DescriptorHeapOffset + arrayIndex;
    if (!_CopyResourceViewToGpuHeap(view, _resHeapView, heapIndex)) {
        RADRAY_ERR_LOG("failed to copy resource descriptor for binding parameter id {}", id.Value);
        return false;
    }
    if (heapIndex >= _resourceWritten.size()) {
        RADRAY_ERR_LOG("internal error: resource heap index {} is out of range", heapIndex);
        return false;
    }
    _resourceWritten[heapIndex] = 1;
    return true;
}

bool DescriptorSetD3D12::WriteSampler(BindingParameterId id, Sampler* sampler, uint32_t arrayIndex) noexcept {
    if (!this->IsValid()) {
        RADRAY_ERR_LOG("descriptor set is invalid");
        return false;
    }
    if (sampler == nullptr) {
        RADRAY_ERR_LOG("sampler is null");
        return false;
    }
    auto infoOpt = _rootSig->FindParameterInfo(id);
    if (!infoOpt.HasValue() || infoOpt.Get() == nullptr) {
        RADRAY_ERR_LOG("binding parameter id {} is out of range", id.Value);
        return false;
    }
    const auto* info = infoOpt.Get();
    if (info->IsStaticSampler) {
        RADRAY_ERR_LOG("binding parameter id {} is a static sampler and cannot be written through DescriptorSet", id.Value);
        return false;
    }
    if (info->Kind != BindingParameterKind::Sampler) {
        RADRAY_ERR_LOG("binding parameter id {} is not a sampler parameter", id.Value);
        return false;
    }
    if (info->SetIndex != _setIndex) {
        RADRAY_ERR_LOG(
            "binding parameter id {} belongs to descriptor set {}, not {}",
            id.Value,
            info->SetIndex.Value,
            _setIndex.Value);
        return false;
    }
    if (arrayIndex >= info->DescriptorCount) {
        RADRAY_ERR_LOG("argument out of range '{}' expected: {}, actual: {}", "arrayIndex", info->DescriptorCount, arrayIndex);
        return false;
    }
    if (!_samplerHeapView.IsValid()) {
        RADRAY_ERR_LOG("descriptor set has no sampler descriptor heap slice");
        return false;
    }
    const uint32_t heapIndex = info->DescriptorHeapOffset + arrayIndex;
    if (heapIndex >= _samplerWritten.size()) {
        RADRAY_ERR_LOG("internal error: sampler heap index {} is out of range", heapIndex);
        return false;
    }
    auto* sam = CastD3D12Object(sampler);
    sam->_samplerView.CopyTo(0, 1, _samplerHeapView, heapIndex);
    _samplerWritten[heapIndex] = 1;
    return true;
}

bool DescriptorSetD3D12::IsFullyWritten() const noexcept {
    if (!this->IsValid()) {
        return false;
    }
    for (const auto& param : _rootSig->GetDescriptorSetLayout(_setIndex)) {
        auto infoOpt = _rootSig->FindParameterInfo(param.Id);
        if (!infoOpt.HasValue() || infoOpt.Get() == nullptr) {
            return false;
        }
        const auto& info = *infoOpt.Get();
        const auto& written = info.Kind == BindingParameterKind::Sampler ? _samplerWritten : _resourceWritten;
        for (uint32_t j = 0; j < info.DescriptorCount; ++j) {
            const uint32_t index = info.DescriptorHeapOffset + j;
            if (index >= written.size() || !written[index]) {
                return false;
            }
        }
    }
    return true;
}

SamplerD3D12::SamplerD3D12(
    DeviceD3D12* device,
    CpuDescriptorHeapViewRAII heapView) noexcept
    : _device(device),
      _samplerView(std::move(heapView)) {}

bool SamplerD3D12::IsValid() const noexcept {
    return _samplerView.IsValid();
}

void SamplerD3D12::Destroy() noexcept {
    _samplerView.Destroy();
}

void SamplerD3D12::SetDebugName(std::string_view name) noexcept {
    _name = string(name);
}

BindlessArrayD3D12::BindlessArrayD3D12(
    DeviceD3D12* device,
    const BindlessArrayDescriptor& desc,
    GpuDescriptorHeapViewRAII resHeap,
    GpuDescriptorHeapViewRAII samplerHeap) noexcept
    : _device(device),
      _desc(desc),
      _resHeap(std::move(resHeap)),
      _samplerHeap(std::move(samplerHeap)),
      _slotKinds(desc.Size, SlotKind::None),
      _slotResourceTypes(desc.Size, ResourceBindType::UNKNOWN),
      _slotViews(desc.Size, nullptr),
      _size(desc.Size),
      _slotType(desc.SlotType) {}

bool BindlessArrayD3D12::IsValid() const noexcept {
    return _device != nullptr && _resHeap.IsValid();
}

void BindlessArrayD3D12::Destroy() noexcept {
    _resHeap.Destroy();
    _samplerHeap.Destroy();
    _slotKinds.clear();
    _slotResourceTypes.clear();
    _slotViews.clear();
    _desc = {};
    _size = 0;
    _slotType = BindlessSlotType::Multiple;
    _device = nullptr;
}

void BindlessArrayD3D12::SetDebugName(std::string_view name) noexcept {
    _name = string(name);
}

void BindlessArrayD3D12::SetBuffer(uint32_t slot, BufferView* bufView) noexcept {
    if (_slotType != BindlessSlotType::BufferOnly) {
        RADRAY_ERR_LOG("d3d12 bindless array does not support buffer slots");
        return;
    }
    if (slot >= _size) {
        RADRAY_ERR_LOG("argument out of range '{}' expected: {}, actual: {}", "slot", _size, slot);
        return;
    }
    if (bufView == nullptr) {
        RADRAY_ERR_LOG("d3d12 bindless array buffer view is null");
        return;
    }
    auto bindType = _GetResourceViewBindType(bufView);
    if (!bindType.has_value() ||
        (bindType.value() != ResourceBindType::Buffer &&
         bindType.value() != ResourceBindType::RWBuffer)) {
        RADRAY_ERR_LOG(
            "d3d12 bindless array does not support buffer view type {}",
            bindType.has_value() ? bindType.value() : ResourceBindType::UNKNOWN);
        return;
    }
    auto bufferView = CastD3D12Object(bufView);
    bufferView->_heapView.CopyTo(0, 1, _resHeap, slot);
    _slotKinds[slot] = SlotKind::Buffer;
    _slotResourceTypes[slot] = bindType.value();
    _slotViews[slot] = bufView;
}

void BindlessArrayD3D12::SetTexture(uint32_t slot, TextureView* texView, Sampler* sampler) noexcept {
    RADRAY_UNUSED(sampler);
    if (texView == nullptr) {
        RADRAY_ERR_LOG("d3d12 bindless array texture view is null");
        return;
    }
    auto textureView = CastD3D12Object(texView);
    auto dim = textureView->_desc.Dim;
    if (dim != TextureDimension::Dim2D) {
        RADRAY_ERR_LOG("d3d12 bindless array only supports texture 2D");
        return;
    }
    if (_slotType != BindlessSlotType::Texture2DOnly) {
        RADRAY_ERR_LOG("d3d12 bindless array does not support texture slots");
        return;
    }
    if (slot >= _size) {
        RADRAY_ERR_LOG("argument out of range '{}' expected: {}, actual: {}", "slot", _size, slot);
        return;
    }
    auto bindType = _GetResourceViewBindType(texView);
    if (!bindType.has_value() || bindType.value() != ResourceBindType::Texture) {
        RADRAY_ERR_LOG(
            "d3d12 bindless array does not support texture view type {}",
            bindType.has_value() ? bindType.value() : ResourceBindType::UNKNOWN);
        return;
    }
    textureView->_heapView.CopyTo(0, 1, _resHeap, slot);
    _slotKinds[slot] = SlotKind::Texture2D;
    _slotResourceTypes[slot] = bindType.value();
    _slotViews[slot] = texView;
}

}  // namespace radray::render::d3d12
