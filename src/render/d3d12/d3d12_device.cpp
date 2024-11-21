#include "d3d12_device.h"

#include <radray/basic_math.h>
#include "d3d12_shader.h"

namespace radray::render::d3d12 {

static void DestroyImpl(DeviceD3D12* d) noexcept {
    for (auto&& i : d->_queues) {
        i.clear();
    }
    d->_device = nullptr;
}

DeviceD3D12::~DeviceD3D12() noexcept { DestroyImpl(this); }

void DeviceD3D12::Destroy() noexcept { DestroyImpl(this); }

std::optional<CommandQueue*> DeviceD3D12::GetCommandQueue(QueueType type, uint32_t slot) noexcept {
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
        ComPtr<ID3D12CommandQueue> queue;
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type = MapType(type);
        if (HRESULT hr = _device->CreateCommandQueue(&desc, IID_PPV_ARGS(queue.GetAddressOf()));
            hr == S_OK) {
            auto ins = radray::make_unique<CmdQueueD3D12>(std::move(queue), this, desc.Type);
            radray::string debugName = radray::format("Queue {} {}", type, slot);
            SetObjectName(debugName, ins->_queue.Get());
            q = std::move(ins);
        } else {
            RADRAY_ERR_LOG("cannot create ID3D12CommandQueue, reason={} (code:{})", GetErrorName(hr), hr);
        }
    }
    return q->IsValid() ? std::make_optional(q.get()) : std::nullopt;
}

std::optional<radray::shared_ptr<Shader>> DeviceD3D12::CreateShader(
    std::span<const byte> blob,
    const ShaderReflection& refl,
    ShaderStage stage,
    std::string_view entryPoint,
    std::string_view name) noexcept {
    auto dxilRefl = std::get_if<DxilReflection>(&refl);
    if (dxilRefl == nullptr) {
        RADRAY_ERR_LOG("d3d12 can only use dxil shader");
        return std::nullopt;
    }
    return radray::make_shared<Dxil>(blob, *dxilRefl, entryPoint, name, stage);
}

std::optional<radray::shared_ptr<RootSignature>> DeviceD3D12::CreateRootSignature(std::span<Shader*> shaders) noexcept {
    class StageResource : public DxilReflection::BindResource {
    public:
        ShaderStages Stages;
    };
    // 收集所有 bind resource
    radray::vector<StageResource> resources;
    radray::vector<StageResource> samplers;
    radray::vector<StageResource> staticSamplers;
    ShaderStages shaderStages{ToFlags(ShaderStage::UNKNOWN)};
    for (Shader* i : shaders) {
        Dxil* dxil = static_cast<Dxil*>(i);
        shaderStages |= dxil->Stage;
        const auto& refl = dxil->_refl;
        for (const DxilReflection::BindResource& j : refl.Binds) {
            StageResource res{j, ToFlags(dxil->Stage)};
            if (j.Type == ShaderResourceType::Sampler) {
                const auto& stat = refl.StaticSamplers;
                auto iter = std::find_if(stat.begin(), stat.end(), [&](auto&& v) noexcept { return j.Name == v; });
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
    radray::vector<StageResource> mergeSamplers = merge(samplers), mergeStaticSamplers = merge(staticSamplers);
    radray::unordered_set<uint32_t> samplersSpaces;
    std::sort(mergeSamplers.begin(), mergeSamplers.end(), resCmp);
    std::sort(mergeStaticSamplers.begin(), mergeStaticSamplers.end(), resCmp);
    for (const StageResource& i : mergeSamplers) {
        samplersSpaces.emplace(i.Space);
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
    radray::unordered_map<radray::string, DxilReflection::CBuffer> cbufferMap{};
    for (Shader* i : shaders) {
        Dxil* dxil = static_cast<Dxil*>(i);
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
    }
    uint64_t useRC = 0, useRD = 0, useDT = 0;
    {
        // 尝试将第一个 cbuffer 用 root constant, 其他 cbuffer 用 root descriptor
        for (size_t i = 0; i < mergedCbuffers.size(); i++) {
            if (i == 0) {
                auto iter = cbufferMap.find(mergedCbuffers[i].Name);
                if (iter == cbufferMap.end()) {
                    RADRAY_ERR_LOG("cannot find cbuffer {}", mergedCbuffers[i].Name);
                    return std::nullopt;
                }
                const DxilReflection::CBuffer& cbuffer = iter->second;
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
    D3D12_ROOT_SIGNATURE_DESC1 rcDesc;
    return std::nullopt;
}

std::optional<radray::shared_ptr<GraphicsPipelineState>> DeviceD3D12::CreateGraphicsPipeline(
    const GraphicsPipelineStateDescriptor& desc) noexcept {
    return std::nullopt;
}

std::optional<radray::shared_ptr<DeviceD3D12>> CreateDevice(const D3D12DeviceDescriptor& desc) {
    uint32_t dxgiFactoryFlags = 0;
    if (desc.IsEnableDebugLayer) {
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
            debugController->EnableDebugLayer();
            dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
            if (desc.IsEnableGpuBasedValid) {
                ComPtr<ID3D12Debug1> debug1;
                if (debugController.As(&debug1) == S_OK) {
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
        return std::nullopt;
    }
    ComPtr<IDXGIAdapter1> adapter;
    if (desc.AdapterIndex.has_value()) {
        uint32_t index = desc.AdapterIndex.value();
        if (HRESULT hr = dxgiFactory->EnumAdapters1(index, adapter.GetAddressOf());
            FAILED(hr)) {
            RADRAY_ERR_LOG("cannot get IDXGIAdapter1 at index {}, reason={} (code:{})", index, GetErrorName(hr), hr);
            return std::nullopt;
        }
    } else {
        ComPtr<IDXGIFactory6> factory6;
        if (dxgiFactory.As(&factory6) == S_OK) {
            for (
                auto adapterIndex = 0u;
                factory6->EnumAdapterByGpuPreference(adapterIndex, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(adapter.GetAddressOf())) != DXGI_ERROR_NOT_FOUND;
                adapterIndex++) {
                DXGI_ADAPTER_DESC1 desc;
                adapter->GetDesc1(&desc);
                if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
                    radray::wstring s{desc.Description};
                    RADRAY_INFO_LOG("D3D12 find device: {}", ToMultiByte(s).value());
                }
            }
            for (
                auto adapterIndex = 0u;
                factory6->EnumAdapterByGpuPreference(adapterIndex, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(adapter.GetAddressOf())) != DXGI_ERROR_NOT_FOUND;
                adapterIndex++) {
                DXGI_ADAPTER_DESC1 desc;
                adapter->GetDesc1(&desc);
                if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
                    if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, _uuidof(ID3D12Device), nullptr))) {
                        break;
                    }
                }
                adapter = nullptr;
            }
        } else {
            if (dxgiFactory->EnumAdapters1(0, adapter.GetAddressOf())) {
                DXGI_ADAPTER_DESC1 desc;
                adapter->GetDesc1(&desc);
                if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 ||
                    FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, _uuidof(ID3D12Device), nullptr))) {
                    adapter = nullptr;
                }
            }
        }
    }
    if (adapter == nullptr) {
        RADRAY_ERR_LOG("cannot get IDXGIAdapter1");
        return std::nullopt;
    }
    ComPtr<ID3D12Device> device;
    if (HRESULT hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.GetAddressOf()));
        FAILED(hr)) {
        RADRAY_ERR_LOG("cannot create ID3D12Device, reason={} (code:{})", GetErrorName(hr), hr);
        return std::nullopt;
    }
    {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        radray::wstring s{desc.Description};
        RADRAY_INFO_LOG("select device: {}", ToMultiByte(s).value());
    }
    auto result = radray::make_shared<DeviceD3D12>(std::move(device));
    RADRAY_INFO_LOG("========== Feature ==========");
    {
        LARGE_INTEGER l;
        HRESULT hr = adapter->CheckInterfaceSupport(IID_IDXGIDevice, &l);
        if (hr == S_OK) {
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
    CD3DX12FeatureSupport fs{};
    if (HRESULT hr = fs.Init(result->_device.Get());
        hr == S_OK) {
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
