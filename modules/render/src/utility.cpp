#include <radray/render/utility.h>

#include <algorithm>

#include <radray/errors.h>

#ifdef RADRAY_ENABLE_D3D12
#include <radray/render/backend/d3d12_impl.h>
#endif

#ifdef RADRAY_ENABLE_VULKAN
#include <radray/render/backend/vulkan_impl.h>
#endif

namespace radray::render {

RootSignatureSetElementContainer::RootSignatureSetElementContainer(const RootSignatureSetElement& elem) noexcept
    : _elem(elem),
      _staticSamplers(elem.StaticSamplers.begin(), elem.StaticSamplers.end()) {
    Refresh();
}

RootSignatureSetElementContainer::RootSignatureSetElementContainer(const RootSignatureSetElementContainer& other) noexcept
    : _elem(other._elem),
      _staticSamplers(other._staticSamplers) {
    Refresh();
}

RootSignatureSetElementContainer::RootSignatureSetElementContainer(RootSignatureSetElementContainer&& other) noexcept
    : _elem(other._elem),
      _staticSamplers(std::move(other._staticSamplers)) {
    Refresh();
}

RootSignatureSetElementContainer& RootSignatureSetElementContainer::operator=(const RootSignatureSetElementContainer& other) noexcept {
    RootSignatureSetElementContainer temp{other};
    swap(*this, temp);
    return *this;
}

RootSignatureSetElementContainer& RootSignatureSetElementContainer::operator=(RootSignatureSetElementContainer&& other) noexcept {
    RootSignatureSetElementContainer temp{std::move(other)};
    swap(*this, temp);
    return *this;
}

void swap(RootSignatureSetElementContainer& lhs, RootSignatureSetElementContainer& rhs) noexcept {
    using std::swap;
    swap(lhs._elem, rhs._elem);
    swap(lhs._staticSamplers, rhs._staticSamplers);
    lhs.Refresh();
    rhs.Refresh();
}

void RootSignatureSetElementContainer::Refresh() noexcept {
    _elem.StaticSamplers = std::span{_staticSamplers};
}

vector<RootSignatureSetElementContainer> RootSignatureSetElementContainer::FromView(std::span<const RootSignatureSetElement> elems) noexcept {
    vector<RootSignatureSetElementContainer> result;
    result.reserve(elems.size());
    for (const auto& i : elems) {
        result.emplace_back(i);
    }
    return result;
}

RootSignatureDescriptorContainer::RootSignatureDescriptorContainer(const RootSignatureDescriptor& desc) noexcept {
    _rootDescriptors.assign(desc.RootDescriptors.begin(), desc.RootDescriptors.end());
    _constant = desc.Constant;

    const size_t setCount = desc.DescriptorSets.size();
    size_t totalElements = 0;
    size_t totalSamplers = 0;
    for (const auto& set : desc.DescriptorSets) {
        totalElements += set.Elements.size();
        for (const auto& elem : set.Elements) {
            totalSamplers += elem.StaticSamplers.size();
        }
    }

    _descriptorSetElements.clear();
    _descriptorSetElements.reserve(totalElements);
    _descriptorSetRanges.clear();
    _descriptorSetRanges.reserve(setCount);
    _descriptorSets.clear();
    _descriptorSets.resize(setCount);

    _staticSamplers.clear();
    _staticSamplers.reserve(totalSamplers);
    _staticSamplerRanges.clear();
    _staticSamplerRanges.reserve(totalElements);

    for (const auto& set : desc.DescriptorSets) {
        DescriptorSetRange range{};
        range.Offset = _descriptorSetElements.size();
        for (const auto& elem : set.Elements) {
            SamplerRange samplerRange{};
            if (!elem.StaticSamplers.empty()) {
                samplerRange.Offset = _staticSamplers.size();
                samplerRange.Count = elem.StaticSamplers.size();
                _staticSamplers.insert(_staticSamplers.end(), elem.StaticSamplers.begin(), elem.StaticSamplers.end());
            }
            _staticSamplerRanges.push_back(samplerRange);
            RootSignatureSetElement stored = elem;
            stored.StaticSamplers = {};
            _descriptorSetElements.push_back(stored);
        }
        range.Count = _descriptorSetElements.size() - range.Offset;
        _descriptorSetRanges.push_back(range);
    }

    Refresh();
}

RootSignatureDescriptorContainer::RootSignatureDescriptorContainer(const RootSignatureDescriptorContainer& other) noexcept
    : _rootDescriptors(other._rootDescriptors),
      _descriptorSetElements(other._descriptorSetElements),
      _descriptorSetRanges(other._descriptorSetRanges),
      _descriptorSets(other._descriptorSets),
      _staticSamplers(other._staticSamplers),
      _staticSamplerRanges(other._staticSamplerRanges),
      _constant(other._constant) {
    Refresh();
}

RootSignatureDescriptorContainer::RootSignatureDescriptorContainer(RootSignatureDescriptorContainer&& other) noexcept
    : _rootDescriptors(std::move(other._rootDescriptors)),
      _descriptorSetElements(std::move(other._descriptorSetElements)),
      _descriptorSetRanges(std::move(other._descriptorSetRanges)),
      _descriptorSets(std::move(other._descriptorSets)),
      _staticSamplers(std::move(other._staticSamplers)),
      _staticSamplerRanges(std::move(other._staticSamplerRanges)),
      _constant(std::move(other._constant)) {
    Refresh();
}

RootSignatureDescriptor RootSignatureDescriptorContainer::Get() const noexcept {
    RootSignatureDescriptor desc{};
    desc.RootDescriptors = std::span<const RootSignatureRootDescriptor>{_rootDescriptors};
    desc.DescriptorSets = std::span<const RootSignatureDescriptorSet>{_descriptorSets};
    desc.Constant = _constant;
    return desc;
}

RootSignatureDescriptorContainer& RootSignatureDescriptorContainer::operator=(const RootSignatureDescriptorContainer& other) noexcept {
    RootSignatureDescriptorContainer tmp{other};
    swap(*this, tmp);
    return *this;
}

RootSignatureDescriptorContainer& RootSignatureDescriptorContainer::operator=(RootSignatureDescriptorContainer&& other) noexcept {
    RootSignatureDescriptorContainer tmp{std::move(other)};
    swap(*this, tmp);
    return *this;
}

void RootSignatureDescriptorContainer::Refresh() noexcept {
    for (size_t i = 0; i < _descriptorSetElements.size(); i++) {
        const SamplerRange range = i < _staticSamplerRanges.size() ? _staticSamplerRanges[i] : SamplerRange{};
        if (range.Count == 0) {
            _descriptorSetElements[i].StaticSamplers = std::span<SamplerDescriptor>{};
        } else {
            _descriptorSetElements[i].StaticSamplers = std::span<SamplerDescriptor>{_staticSamplers.data() + range.Offset, range.Count};
        }
    }

    if (_descriptorSets.size() != _descriptorSetRanges.size()) {
        _descriptorSets.resize(_descriptorSetRanges.size());
    }
    for (size_t i = 0; i < _descriptorSetRanges.size(); i++) {
        const DescriptorSetRange range = _descriptorSetRanges[i];
        if (range.Count == 0) {
            _descriptorSets[i].Elements = std::span<RootSignatureSetElement>{};
        } else {
            _descriptorSets[i].Elements = std::span<RootSignatureSetElement>{_descriptorSetElements.data() + range.Offset, range.Count};
        }
    }
}

void swap(RootSignatureDescriptorContainer& lhs, RootSignatureDescriptorContainer& rhs) noexcept {
    using std::swap;
    swap(lhs._rootDescriptors, rhs._rootDescriptors);
    swap(lhs._descriptorSetElements, rhs._descriptorSetElements);
    swap(lhs._descriptorSetRanges, rhs._descriptorSetRanges);
    swap(lhs._descriptorSets, rhs._descriptorSets);
    swap(lhs._staticSamplers, rhs._staticSamplers);
    swap(lhs._staticSamplerRanges, rhs._staticSamplerRanges);
    swap(lhs._constant, rhs._constant);
    lhs.Refresh();
    rhs.Refresh();
}

bool IsDepthStencilFormat(TextureFormat format) noexcept {
    switch (format) {
        case TextureFormat::S8:
        case TextureFormat::D16_UNORM:
        case TextureFormat::D32_FLOAT:
        case TextureFormat::D24_UNORM_S8_UINT:
        case TextureFormat::D32_FLOAT_S8_UINT: return true;
        default: return false;
    }
}

uint32_t GetVertexFormatSize(VertexFormat format) noexcept {
    switch (format) {
        case VertexFormat::UINT8X2:
        case VertexFormat::SINT8X2:
        case VertexFormat::UNORM8X2:
        case VertexFormat::SNORM8X2: return 2;
        case VertexFormat::UINT8X4:
        case VertexFormat::SINT8X4:
        case VertexFormat::UNORM8X4:
        case VertexFormat::SNORM8X4:
        case VertexFormat::UINT16X2:
        case VertexFormat::SINT16X2:
        case VertexFormat::UNORM16X2:
        case VertexFormat::SNORM16X2:
        case VertexFormat::FLOAT16X2:
        case VertexFormat::UINT32:
        case VertexFormat::SINT32:
        case VertexFormat::FLOAT32: return 4;
        case VertexFormat::UINT16X4:
        case VertexFormat::SINT16X4:
        case VertexFormat::UNORM16X4:
        case VertexFormat::SNORM16X4:
        case VertexFormat::FLOAT16X4:
        case VertexFormat::UINT32X2:
        case VertexFormat::SINT32X2:
        case VertexFormat::FLOAT32X2: return 8;
        case VertexFormat::UINT32X3:
        case VertexFormat::SINT32X3:
        case VertexFormat::FLOAT32X3: return 12;
        case VertexFormat::UINT32X4:
        case VertexFormat::SINT32X4:
        case VertexFormat::FLOAT32X4: return 16;
        case VertexFormat::UNKNOWN: return 0;
    }
    Unreachable();
}

uint32_t GetIndexFormatSize(IndexFormat format) noexcept {
    switch (format) {
        case IndexFormat::UINT16: return 2;
        case IndexFormat::UINT32: return 4;
    }
    Unreachable();
}

PrimitiveState DefaultPrimitiveState() noexcept {
    return {
        PrimitiveTopology::TriangleList,
        FrontFace::CW,
        CullMode::Back,
        PolygonMode::Fill,
        std::nullopt,
        true,
        false};
}

DepthStencilState DefaultDepthStencilState() noexcept {
    return {
        TextureFormat::D32_FLOAT,
        CompareFunction::Less,
        {
            0,
            0.0f,
            0.0f,
        },
        std::nullopt,
        true};
}

StencilState DefaultStencilState() noexcept {
    return {
        {
            CompareFunction::Always,
            StencilOperation::Keep,
            StencilOperation::Keep,
            StencilOperation::Keep,
        },
        {
            CompareFunction::Always,
            StencilOperation::Keep,
            StencilOperation::Keep,
            StencilOperation::Keep,
        },
        0xFF,
        0xFF};
}

MultiSampleState DefaultMultiSampleState() noexcept {
    return {
        .Count = 1,
        .Mask = 0xFFFFFFFF,
        .AlphaToCoverageEnable = false};
}

ColorTargetState DefaultColorTargetState(TextureFormat format) noexcept {
    return {
        format,
        std::nullopt,
        ColorWrite::All};
}

BlendState DefaultBlendState() noexcept {
    return {
        {BlendFactor::One,
         BlendFactor::Zero,
         BlendOperation::Add},
        {BlendFactor::One,
         BlendFactor::Zero,
         BlendOperation::Add}};
}

IndexFormat MapIndexType(uint32_t size) noexcept {
    switch (size) {
        case 2: return IndexFormat::UINT16;
        case 4: return IndexFormat::UINT32;
        default: return IndexFormat::UINT32;
    }
}

std::optional<vector<VertexElement>> MapVertexElements(std::span<const VertexBufferEntry> layouts, std::span<const SemanticMapping> semantics) noexcept {
    vector<VertexElement> result;
    result.reserve(semantics.size());
    for (const auto& want : semantics) {
        uint32_t wantSize = GetVertexFormatSize(want.Format);
        const VertexBufferEntry* found = nullptr;
        for (const auto& l : layouts) {
            uint32_t preSize = GetVertexDataSizeInBytes(l.Type, l.ComponentCount);
            if (l.Semantic == want.Semantic && l.SemanticIndex == want.SemanticIndex && preSize == wantSize) {
                found = &l;
                break;
            }
        }
        if (!found) {
            return std::nullopt;
        }
        VertexElement& ve = result.emplace_back();
        ve.Offset = found->Offset;
        ve.Semantic = found->Semantic;
        ve.SemanticIndex = found->SemanticIndex;
        ve.Format = want.Format;
        ve.Location = want.Location;
    }
    return result;
}

Nullable<shared_ptr<RootSignature>> CreateSerializedRootSignature(Device* device_, std::span<const byte> data) noexcept {
#ifdef RADRAY_ENABLE_D3D12
    if (device_->GetBackend() != RenderBackend::D3D12) {
        RADRAY_ERR_LOG("{} {} {}", Errors::D3D12, Errors::InvalidOperation, "device");
        return nullptr;
    }
    auto device = d3d12::CastD3D12Object(device_);
    d3d12::ComPtr<ID3D12RootSignature> rootSig;
    if (HRESULT hr = device->_device->CreateRootSignature(0, data.data(), data.size(), IID_PPV_ARGS(&rootSig));
        FAILED(hr)) {
        RADRAY_ERR_LOG("{} {}::{} {}", Errors::D3D12, "ID3D12Device", "CreateRootSignature", hr);
        return nullptr;
    }
    DynamicLibrary d3d12Dll{"d3d12"};
    if (!d3d12Dll.IsValid()) {
        return nullptr;
    }
    auto D3D12CreateVersionedRootSignatureDeserializer_F = d3d12Dll.GetFunction<PFN_D3D12_CREATE_ROOT_SIGNATURE_DESERIALIZER>("D3D12CreateVersionedRootSignatureDeserializer");
    if (!D3D12CreateVersionedRootSignatureDeserializer_F) {
        return nullptr;
    }
    d3d12::ComPtr<ID3D12VersionedRootSignatureDeserializer> deserializer;
    if (HRESULT hr = D3D12CreateVersionedRootSignatureDeserializer_F(data.data(), data.size(), IID_PPV_ARGS(&deserializer));
        FAILED(hr)) {
        RADRAY_ERR_LOG("{} {}::{} {}", Errors::D3D12, "D3D12CreateVersionedRootSignatureDeserializer", d3d12::GetErrorName(hr), hr);
        return nullptr;
    }
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc;
    if (HRESULT hr = deserializer->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &desc);
        FAILED(hr)) {
        RADRAY_ERR_LOG("{} {}::{} {}", Errors::D3D12, "ID3D12VersionedRootSignatureDeserializer", "GetRootSignatureDescAtVersion", hr);
        return nullptr;
    }
    if (desc->Version != D3D_ROOT_SIGNATURE_VERSION_1_1) {
        RADRAY_ERR_LOG("{} {} {}", Errors::D3D12, "unknown version", desc->Version);
        return nullptr;
    }
    auto result = make_shared<d3d12::RootSigD3D12>(device, std::move(rootSig));
    result->_desc = d3d12::VersionedRootSignatureDescContainer{*desc};
    return result;
#else
    RADRAY_ERR_LOG("only d3d12 backend supports serialized root signature");
    return nullptr;
#endif
}

static ShaderStages _NormalizeStageMask(ShaderStage stage) noexcept {
    switch (stage) {
        case ShaderStage::UNKNOWN: {
            ShaderStages mask{};
            mask |= ShaderStage::Vertex;
            mask |= ShaderStage::Pixel;
            mask |= ShaderStage::Compute;
            return mask;
        }
        case ShaderStage::Graphics: {
            ShaderStages mask{};
            mask |= ShaderStage::Vertex;
            mask |= ShaderStage::Pixel;
            return mask;
        }
        default: return ShaderStages{stage};
    }
}

static bool _IsBufferDimension(HlslSRVDimension dim) noexcept {
    switch (dim) {
        case HlslSRVDimension::BUFFER:
        case HlslSRVDimension::BUFFEREX: return true;
        default: return false;
    }
}

static std::optional<ResourceBindType> _MapResourceBindType(const HlslInputBindDesc& bind) noexcept {
    switch (bind.Type) {
        case HlslShaderInputType::CBUFFER: return ResourceBindType::CBuffer;
        case HlslShaderInputType::TBUFFER:
        case HlslShaderInputType::STRUCTURED:
        case HlslShaderInputType::BYTEADDRESS:
            return ResourceBindType::Buffer;
        case HlslShaderInputType::TEXTURE: return ResourceBindType::Texture;
        case HlslShaderInputType::SAMPLER: return ResourceBindType::Sampler;
        case HlslShaderInputType::UAV_RWTYPED:
            return _IsBufferDimension(bind.Dimension) ? ResourceBindType::RWBuffer : ResourceBindType::RWTexture;
        case HlslShaderInputType::UAV_RWSTRUCTURED:
        case HlslShaderInputType::UAV_RWSTRUCTURED_WITH_COUNTER:
        case HlslShaderInputType::UAV_APPEND_STRUCTURED:
        case HlslShaderInputType::UAV_CONSUME_STRUCTURED:
        case HlslShaderInputType::UAV_RWBYTEADDRESS:
            return ResourceBindType::RWBuffer;
        case HlslShaderInputType::RTACCELERATIONSTRUCTURE: return ResourceBindType::Buffer;
        case HlslShaderInputType::UAV_FEEDBACKTEXTURE: return ResourceBindType::RWTexture;
        default: return std::nullopt;
    }
}

static ShaderStages _EnsureNonEmptyStages(ShaderStages stages) noexcept {
    if (static_cast<bool>(stages)) {
        return stages;
    }
    ShaderStages mask{};
    mask |= ShaderStage::Vertex;
    mask |= ShaderStage::Pixel;
    mask |= ShaderStage::Compute;
    return mask;
}

static const HlslShaderBufferDesc* _FindCBufferByName(const HlslShaderDesc& desc, std::string_view name) noexcept {
    auto it = std::find_if(
        desc.ConstantBuffers.begin(),
        desc.ConstantBuffers.end(),
        [&](const HlslShaderBufferDesc& cb) { return std::string_view{cb.Name} == name; });
    return it == desc.ConstantBuffers.end() ? nullptr : &(*it);
}

static uint32_t _StageUsageScore(ShaderStages stages) noexcept {
    uint32_t score = 0;
    if (stages.HasFlag(ShaderStage::Vertex)) {
        ++score;
    }
    if (stages.HasFlag(ShaderStage::Pixel)) {
        ++score;
    }
    if (stages.HasFlag(ShaderStage::Compute)) {
        ++score;
    }
    return score;
}

static uint32_t _BindTypePriority(ResourceBindType type) noexcept {
    switch (type) {
        case ResourceBindType::CBuffer: return 0;
        case ResourceBindType::Buffer: return 1;
        case ResourceBindType::RWBuffer: return 2;
        case ResourceBindType::Texture: return 3;
        case ResourceBindType::RWTexture: return 4;
        case ResourceBindType::Sampler: return 5;
        default: return 6;
    }
}

struct BindDescWithCBuffer {
    const HlslInputBindDesc* bind;
    const HlslShaderBufferDesc* cbuffer;
    ShaderStages stages{ShaderStage::UNKNOWN};
};

std::optional<RootSignatureDescriptorContainer> GenerateRSDescFromHlslShaderDescs(std::span<const StagedHlslShaderDesc> descs) noexcept {
    if (descs.empty()) {
        RADRAY_ERR_LOG("{} {} {}", Errors::D3D12, Errors::InvalidArgument, "descs");
        return std::nullopt;
    }

    struct CombinedBinding {
        ResourceBindType Type{ResourceBindType::UNKNOWN};
        uint32_t Slot{0};
        uint32_t Space{0};
        uint32_t Count{0};
        ShaderStages Stages{ShaderStage::UNKNOWN};
        const HlslShaderBufferDesc* CBuffer{nullptr};
        std::string Name;
    };

    size_t totalBindings = 0;
    for (const auto& staged : descs) {
        if (staged.Desc != nullptr) {
            totalBindings += staged.Desc->BoundResources.size();
        }
    }

    vector<CombinedBinding> bindings;
    bindings.reserve(totalBindings);

    for (const auto& staged : descs) {
        if (staged.Desc == nullptr) {
            RADRAY_ERR_LOG("{} {} {}", Errors::D3D12, Errors::InvalidArgument, "shader desc");
            return std::nullopt;
        }
        ShaderStages stageMask = _NormalizeStageMask(staged.Stage);
        for (const auto& bind : staged.Desc->BoundResources) {
            if (bind.BindCount == 0 || bind.BindCount == std::numeric_limits<uint32_t>::max()) {
                RADRAY_ERR_LOG("{} {} {} {}", Errors::D3D12, Errors::InvalidOperation, "unsupported bind count", bind.Name);
                return std::nullopt;
            }
            auto typeOpt = _MapResourceBindType(bind);
            if (!typeOpt.has_value()) {
                RADRAY_ERR_LOG("{} {} {} {}", Errors::D3D12, Errors::InvalidOperation, "unsupported resource", bind.Name);
                return std::nullopt;
            }
            ResourceBindType type = typeOpt.value();
            if (type == ResourceBindType::UNKNOWN) {
                continue;
            }
            auto it = std::find_if(
                bindings.begin(),
                bindings.end(),
                [&](const CombinedBinding& existing) {
                    return existing.Type == type && existing.Space == bind.Space && existing.Slot == bind.BindPoint;
                });
            if (it == bindings.end()) {
                CombinedBinding combined{};
                combined.Type = type;
                combined.Slot = bind.BindPoint;
                combined.Space = bind.Space;
                combined.Count = bind.BindCount;
                combined.Stages = stageMask;
                combined.Name = bind.Name;
                if (type == ResourceBindType::CBuffer) {
                    combined.CBuffer = _FindCBufferByName(*staged.Desc, bind.Name);
                }
                bindings.emplace_back(std::move(combined));
            } else {
                if (it->Count != bind.BindCount) {
                    RADRAY_ERR_LOG("{} {} {} {}", Errors::D3D12, Errors::InvalidOperation, "mismatched bind count", bind.Name);
                    return std::nullopt;
                }
                it->Stages |= stageMask;
                if (type == ResourceBindType::CBuffer && it->CBuffer == nullptr) {
                    it->CBuffer = _FindCBufferByName(*staged.Desc, bind.Name);
                }
            }
        }
    }

    if (bindings.empty()) {
        RootSignatureDescriptor desc{};
        return RootSignatureDescriptorContainer{desc};
    }

    enum class Placement { Table,
                           RootDescriptor,
                           RootConstant };
    vector<Placement> placements(bindings.size(), Placement::Table);

    constexpr uint32_t kPreferredRootConstBytes = 64;
    constexpr uint32_t kMaxRootConstBytes = 256;

    auto pickRootConstant = [&](uint32_t limit) -> std::optional<size_t> {
        std::optional<size_t> bestIndex;
        uint32_t bestSize = std::numeric_limits<uint32_t>::max();
        for (size_t i = 0; i < bindings.size(); ++i) {
            const auto& binding = bindings[i];
            if (binding.Type != ResourceBindType::CBuffer || binding.Count != 1 || binding.CBuffer == nullptr) {
                continue;
            }
            if (binding.CBuffer->Size == 0 || binding.CBuffer->Size % 4 != 0) {
                continue;
            }
            if (binding.CBuffer->Size > limit) {
                continue;
            }
            if (!bestIndex.has_value() || binding.CBuffer->Size < bestSize) {
                bestIndex = i;
                bestSize = binding.CBuffer->Size;
            }
        }
        return bestIndex;
    };

    std::optional<size_t> rootConstIndex = pickRootConstant(kPreferredRootConstBytes);
    if (!rootConstIndex.has_value()) {
        rootConstIndex = pickRootConstant(kMaxRootConstBytes);
    }

    std::optional<RootSignatureConstant> rootConstant;
    uint32_t rootConstCost = 0;
    if (rootConstIndex.has_value()) {
        const auto& binding = bindings[rootConstIndex.value()];
        RootSignatureConstant constant{};
        constant.Slot = binding.Slot;
        constant.Space = binding.Space;
        constant.Size = binding.CBuffer->Size;
        constant.Stages = _EnsureNonEmptyStages(binding.Stages);
        rootConstCost = constant.Size / 4;
        placements[rootConstIndex.value()] = Placement::RootConstant;
        rootConstant = constant;
    }

    vector<size_t> rootDescCandidates;
    rootDescCandidates.reserve(bindings.size());
    for (size_t i = 0; i < bindings.size(); ++i) {
        if (placements[i] != Placement::Table) {
            continue;
        }
        const auto& binding = bindings[i];
        if (binding.Count != 1) {
            continue;
        }
        bool eligible = false;
        switch (binding.Type) {
            case ResourceBindType::CBuffer: eligible = binding.CBuffer != nullptr; break;
            case ResourceBindType::Buffer:
            case ResourceBindType::RWBuffer: eligible = true; break;
            default: break;
        }
        if (eligible) {
            rootDescCandidates.push_back(i);
        }
    }

    std::sort(
        rootDescCandidates.begin(),
        rootDescCandidates.end(),
        [&](size_t lhs, size_t rhs) {
            const auto& l = bindings[lhs];
            const auto& r = bindings[rhs];
            uint32_t lp = _BindTypePriority(l.Type);
            uint32_t rp = _BindTypePriority(r.Type);
            if (lp != rp) {
                return lp < rp;
            }
            uint32_t ls = _StageUsageScore(l.Stages);
            uint32_t rs = _StageUsageScore(r.Stages);
            if (ls != rs) {
                return ls > rs;
            }
            if (l.Space != r.Space) {
                return l.Space < r.Space;
            }
            if (l.Slot != r.Slot) {
                return l.Slot < r.Slot;
            }
            return l.Name < r.Name;
        });

    vector<size_t> selectedRootDescs;
    constexpr uint32_t kMaxRootDwords = 64;
    uint32_t rootDescCount = 0;
    for (size_t idx : rootDescCandidates) {
        if (rootConstCost + (rootDescCount + 1) * 2 > kMaxRootDwords) {
            break;
        }
        placements[idx] = Placement::RootDescriptor;
        selectedRootDescs.push_back(idx);
        rootDescCount++;
    }

    struct RangeSpec {
        ResourceBindType Type{ResourceBindType::UNKNOWN};
        uint32_t Space{0};
        uint32_t Slot{0};
        uint32_t Count{0};
        ShaderStages Stages{ShaderStage::UNKNOWN};
    };

    auto buildRanges = [&](const vector<Placement>& layout) -> std::optional<vector<RangeSpec>> {
        struct SortKey {
            ResourceBindType Type;
            uint32_t Space;
            uint32_t Slot;
            size_t Index;
        };
        vector<SortKey> keys;
        keys.reserve(bindings.size());
        for (size_t i = 0; i < bindings.size(); ++i) {
            if (layout[i] != Placement::Table) {
                continue;
            }
            keys.push_back({bindings[i].Type, bindings[i].Space, bindings[i].Slot, i});
        }
        if (keys.empty()) {
            return vector<RangeSpec>{};
        }
        std::sort(
            keys.begin(),
            keys.end(),
            [&](const SortKey& lhs, const SortKey& rhs) {
                if (lhs.Space != rhs.Space) {
                    return lhs.Space < rhs.Space;
                }
                uint32_t lp = _BindTypePriority(lhs.Type);
                uint32_t rp = _BindTypePriority(rhs.Type);
                if (lp != rp) {
                    return lp < rp;
                }
                return lhs.Slot < rhs.Slot;
            });

        vector<RangeSpec> ranges;
        ranges.reserve(keys.size());
        RangeSpec current{};
        bool hasCurrent = false;
        auto flush = [&]() {
            if (hasCurrent) {
                ranges.push_back(current);
                hasCurrent = false;
            }
        };

        for (const auto& key : keys) {
            const auto& binding = bindings[key.Index];
            if (!hasCurrent) {
                current = {binding.Type, binding.Space, binding.Slot, binding.Count, binding.Stages};
                hasCurrent = true;
                continue;
            }
            if (binding.Type == current.Type && binding.Space == current.Space) {
                uint64_t expected = static_cast<uint64_t>(current.Slot) + current.Count;
                if (binding.Slot < expected) {
                    RADRAY_ERR_LOG("{} {} {}", Errors::D3D12, Errors::InvalidOperation, "overlap in resource registers");
                    return std::nullopt;
                }
                if (binding.Slot == expected) {
                    if (binding.Count > std::numeric_limits<uint32_t>::max() - current.Count) {
                        RADRAY_ERR_LOG("{} {} {}", Errors::D3D12, Errors::InvalidOperation, "descriptor range overflow");
                        return std::nullopt;
                    }
                    current.Count += binding.Count;
                    current.Stages |= binding.Stages;
                    continue;
                }
            }
            flush();
            current = {binding.Type, binding.Space, binding.Slot, binding.Count, binding.Stages};
            hasCurrent = true;
        }
        flush();
        return ranges;
    };

    auto rangesOpt = buildRanges(placements);
    if (!rangesOpt.has_value()) {
        return std::nullopt;
    }
    auto ranges = std::move(rangesOpt.value());
    uint32_t rootDescCost = rootDescCount * 2;
    uint32_t totalCost = rootConstCost + rootDescCost + static_cast<uint32_t>(ranges.size());

    while (totalCost > kMaxRootDwords && !selectedRootDescs.empty()) {
        size_t idx = selectedRootDescs.back();
        selectedRootDescs.pop_back();
        placements[idx] = Placement::Table;
        rootDescCount--;
        rootDescCost = rootDescCount * 2;
        auto rebuilt = buildRanges(placements);
        if (!rebuilt.has_value()) {
            return std::nullopt;
        }
        ranges = std::move(rebuilt.value());
        totalCost = rootConstCost + rootDescCost + static_cast<uint32_t>(ranges.size());
    }

    if (totalCost > kMaxRootDwords) {
        RADRAY_ERR_LOG("{} {} {}", Errors::D3D12, Errors::InvalidOperation, "root signature exceeds limit");
        return std::nullopt;
    }

    vector<RootSignatureRootDescriptor> rootDescriptors;
    if (!selectedRootDescs.empty()) {
        std::sort(
            selectedRootDescs.begin(),
            selectedRootDescs.end(),
            [&](size_t lhs, size_t rhs) {
                const auto& l = bindings[lhs];
                const auto& r = bindings[rhs];
                uint32_t lp = _BindTypePriority(l.Type);
                uint32_t rp = _BindTypePriority(r.Type);
                if (lp != rp) {
                    return lp < rp;
                }
                if (l.Space != r.Space) {
                    return l.Space < r.Space;
                }
                return l.Slot < r.Slot;
            });
        rootDescriptors.reserve(selectedRootDescs.size());
        for (size_t idx : selectedRootDescs) {
            const auto& binding = bindings[idx];
            RootSignatureRootDescriptor rd{};
            rd.Slot = binding.Slot;
            rd.Space = binding.Space;
            rd.Type = binding.Type;
            rd.Stages = _EnsureNonEmptyStages(binding.Stages);
            rootDescriptors.push_back(rd);
        }
    }

    vector<RootSignatureSetElement> setElements;
    setElements.reserve(ranges.size());
    vector<RootSignatureDescriptorSet> descriptorSets;
    descriptorSets.reserve(ranges.size());
    for (const auto& range : ranges) {
        RootSignatureSetElement elem{};
        elem.Slot = range.Slot;
        elem.Space = range.Space;
        elem.Type = range.Type;
        elem.Count = range.Count;
        elem.Stages = _EnsureNonEmptyStages(range.Stages);
        elem.StaticSamplers = std::span<SamplerDescriptor>{};
        setElements.push_back(elem);
        RootSignatureDescriptorSet set{};
        set.Elements = std::span<RootSignatureSetElement>{&setElements.back(), 1};
        descriptorSets.push_back(set);
    }

    RootSignatureDescriptor desc{};
    desc.RootDescriptors = std::span<const RootSignatureRootDescriptor>{rootDescriptors};
    desc.DescriptorSets = std::span<const RootSignatureDescriptorSet>{descriptorSets};
    if (rootConstant.has_value()) {
        desc.Constant = rootConstant;
    }

    return RootSignatureDescriptorContainer{desc};
}

}  // namespace radray::render
