#include <radray/render/render_utility.h>

#ifdef RADRAY_ENABLE_D3D12
#include <radray/render/backend/d3d12_impl.h>
#endif
#ifdef RADRAY_ENABLE_VULKAN
#include <radray/render/backend/vulkan_impl.h>
#endif
#include <radray/render/dxc.h>
#include <radray/render/spvc.h>
#include <radray/render/msl.h>

namespace radray::render {

std::optional<vector<VertexElement>> MapVertexElements(std::span<const VertexBufferEntry> layouts, std::span<const SemanticMapping> semantics) noexcept {
    vector<VertexElement> result;
    result.reserve(semantics.size());
    for (const auto& want : semantics) {
        uint32_t wantSize = GetVertexFormatSizeInBytes(want.Format);
        const VertexBufferEntry* found = nullptr;
        for (const auto& l : layouts) {
            uint32_t preSize = vertex_utility::GetVertexDataSizeInBytes(l.Type, l.ComponentCount);
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
        ve.Semantic = want.Semantic;
        ve.SemanticIndex = found->SemanticIndex;
        ve.Format = want.Format;
        ve.Location = want.Location;
    }
    return result;
}

Nullable<unique_ptr<RootSignature>> CreateSerializedRootSignature(Device* device_, std::span<const byte> data) noexcept {
#ifdef RADRAY_ENABLE_D3D12
    if (device_->GetBackend() != RenderBackend::D3D12) {
        RADRAY_ERR_LOG("only d3d12 backend supports serialized root signature");
        return nullptr;
    }
    auto device = d3d12::CastD3D12Object(device_);
    d3d12::ComPtr<ID3D12RootSignature> rootSig;
    if (HRESULT hr = device->_device->CreateRootSignature(0, data.data(), data.size(), IID_PPV_ARGS(&rootSig));
        FAILED(hr)) {
        RADRAY_ERR_LOG("ID3D12Device::CreateRootSignature failed: {}", hr);
        return nullptr;
    }
    d3d12::ComPtr<ID3D12VersionedRootSignatureDeserializer> deserializer;
    if (HRESULT hr = ::D3D12CreateVersionedRootSignatureDeserializer(data.data(), data.size(), IID_PPV_ARGS(&deserializer));
        FAILED(hr)) {
        RADRAY_ERR_LOG("D3D12CreateVersionedRootSignatureDeserializer failed: {}", hr);
        return nullptr;
    }
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc;
    if (HRESULT hr = deserializer->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &desc);
        FAILED(hr)) {
        RADRAY_ERR_LOG("ID3D12VersionedRootSignatureDeserializer::GetRootSignatureDescAtVersion failed: {}", hr);
        return nullptr;
    }
    if (desc->Version != D3D_ROOT_SIGNATURE_VERSION_1_1) {
        RADRAY_ERR_LOG("unknown root signature version: {}", desc->Version);
        return nullptr;
    }
    auto result = make_unique<d3d12::RootSigD3D12>(device, std::move(rootSig));
    result->_desc = d3d12::VersionedRootSignatureDescContainer{*desc};
    return result;
#else
    RADRAY_ERR_LOG("d3d12 backend is not enabled, cannot create serialized root signature");
    return nullptr;
#endif
}

#if defined(RADRAY_ENABLE_SPIRV_CROSS) && defined(RADRAY_ENABLE_SPIRV_CROSS)
std::optional<ShaderCompileResult> CompileShaderFromHLSL(
    Dxc* dxc,
    Device* device,
    std::string_view hlsl,
    RenderBackend backend,
    const vector<StaticSamplerBinding> staticSamplers,
    std::string_view entryVS,
    std::string_view entryPS,
    HlslShaderModel shaderModel,
    const vector<string>& defines_,
    const vector<string>& includes_) noexcept {
    vector<std::string_view> defines;
    if (backend == RenderBackend::Vulkan) {
        defines.emplace_back("VULKAN");
    } else if (backend == RenderBackend::D3D12) {
        defines.emplace_back("D3D12");
    } else if (backend == RenderBackend::Metal) {
        defines.emplace_back("METAL");
    } else {
        RADRAY_ERR_LOG("unsupported backend for shader compilation {}", backend);
        return std::nullopt;
    }
    defines.insert(defines.end(), defines_.begin(), defines_.end());
    vector<std::string_view> includes;
    includes.emplace_back("shaderlib");
    includes.insert(includes.end(), includes_.begin(), includes_.end());
    auto vsOut = dxc->Compile(
        hlsl, entryVS, ShaderStage::Vertex,
        shaderModel, true, defines, includes,
        backend != RenderBackend::D3D12);
    if (!vsOut.has_value()) {
        return std::nullopt;
    }
    auto vsBin = std::move(vsOut.value());
    auto psOut = dxc->Compile(
        hlsl, entryPS, ShaderStage::Pixel,
        shaderModel, true, defines, includes,
        backend != RenderBackend::D3D12);
    if (!psOut.has_value()) {
        return std::nullopt;
    }
    auto psBin = std::move(psOut.value());
    unique_ptr<Shader> vsShader, psShader;
    if (backend == RenderBackend::Metal) {
        SpirvToMslOption mslOption{
            3,
            0,
            0,
#ifdef RADRAY_PLATFORM_MACOS
            MslPlatform::MacOS,
#elif defined(RADRAY_PLATFORM_IOS)
            MslPlatform::IOS,
#else
            MslPlatform::MacOS,
#endif
            true,
            false};
        auto vsMsl = ConvertSpirvToMsl(vsBin.Data, entryVS, ShaderStage::Vertex, mslOption).value();
        vsShader = device->CreateShader({vsMsl.GetBlob(), ShaderBlobCategory::MSL}).Unwrap();
        auto psMsl = ConvertSpirvToMsl(psBin.Data, entryPS, ShaderStage::Pixel, mslOption).value();
        psShader = device->CreateShader({psMsl.GetBlob(), ShaderBlobCategory::MSL}).Unwrap();
    } else {
        vsShader = device->CreateShader({vsBin.Data, vsBin.Category}).Unwrap();
        psShader = device->CreateShader({psBin.Data, psBin.Category}).Unwrap();
    }
    PipelineLayout bindLayout;
    if (backend == RenderBackend::D3D12) {
        auto vsRefl = dxc->GetShaderDescFromOutput(ShaderStage::Vertex, vsBin.Refl).value();
        auto psRefl = dxc->GetShaderDescFromOutput(ShaderStage::Pixel, psBin.Refl).value();
        const HlslShaderDesc* descs[] = {&vsRefl, &psRefl};
        auto merged = MergeHlslShaderDesc(descs).value();
        bindLayout = PipelineLayout{merged, staticSamplers};
    } else if (backend == RenderBackend::Vulkan) {
        SpirvBytecodeView spvs[] = {
            {vsBin.Data, entryVS, ShaderStage::Vertex},
            {psBin.Data, entryPS, ShaderStage::Pixel}};
        auto spirvDesc = ReflectSpirv(spvs).value();
        bindLayout = PipelineLayout{spirvDesc, staticSamplers};
    } else if (backend == RenderBackend::Metal) {
        MslReflectParams mslParams[] = {
            {vsBin.Data, entryVS, ShaderStage::Vertex, true},
            {psBin.Data, entryPS, ShaderStage::Pixel, true}};
        auto mslRefl = ReflectMsl(mslParams).value();
        bindLayout = PipelineLayout{mslRefl, staticSamplers};
    }
    return ShaderCompileResult{std::move(vsShader), std::move(psShader), std::move(bindLayout)};
}
#endif

#if defined(RADRAY_ENABLE_SPIRV_CROSS) && defined(RADRAY_ENABLE_SPIRV_CROSS)
std::optional<ComputeShaderCompileResult> CompileComputeShaderFromHLSL(
    Dxc* dxc,
    Device* device,
    std::string_view hlsl,
    RenderBackend backend,
    std::string_view entryCS,
    HlslShaderModel shaderModel,
    const vector<string>& defines_,
    const vector<string>& includes_) noexcept {
    vector<std::string_view> defines;
    if (backend == RenderBackend::Vulkan) {
        defines.emplace_back("VULKAN");
    } else if (backend == RenderBackend::D3D12) {
        defines.emplace_back("D3D12");
    } else if (backend == RenderBackend::Metal) {
        defines.emplace_back("METAL");
    } else {
        RADRAY_ERR_LOG("unsupported backend for shader compilation {}", backend);
        return std::nullopt;
    }
    defines.insert(defines.end(), defines_.begin(), defines_.end());
    vector<std::string_view> includes;
    includes.emplace_back("shaderlib");
    includes.insert(includes.end(), includes_.begin(), includes_.end());
    auto csOut = dxc->Compile(
        hlsl, entryCS, ShaderStage::Compute,
        shaderModel, true, defines, includes,
        backend != RenderBackend::D3D12);
    if (!csOut.has_value()) {
        return std::nullopt;
    }
    auto csBin = std::move(csOut.value());
    unique_ptr<Shader> csShader;
    PipelineLayout bindLayout;
    if (backend == RenderBackend::Metal) {
        SpirvToMslOption mslOption{
            3, 0, 0,
#ifdef RADRAY_PLATFORM_MACOS
            MslPlatform::MacOS,
#elif defined(RADRAY_PLATFORM_IOS)
            MslPlatform::IOS,
#else
            MslPlatform::MacOS,
#endif
            true, false};
        auto csMsl = ConvertSpirvToMsl(csBin.Data, entryCS, ShaderStage::Compute, mslOption).value();
        csShader = device->CreateShader({csMsl.GetBlob(), ShaderBlobCategory::MSL}).Unwrap();
        MslReflectParams mslParams[] = {{csBin.Data, entryCS, ShaderStage::Compute, true}};
        auto mslRefl = ReflectMsl(mslParams).value();
        bindLayout = PipelineLayout{mslRefl, {}};
    } else if (backend == RenderBackend::D3D12) {
        csShader = device->CreateShader({csBin.Data, csBin.Category}).Unwrap();
        auto csRefl = dxc->GetShaderDescFromOutput(ShaderStage::Compute, csBin.Refl).value();
        const HlslShaderDesc* descs[] = {&csRefl};
        auto merged = MergeHlslShaderDesc(descs).value();
        bindLayout = PipelineLayout{merged, {}};
    } else if (backend == RenderBackend::Vulkan) {
        csShader = device->CreateShader({csBin.Data, csBin.Category}).Unwrap();
        SpirvBytecodeView spvs[] = {{csBin.Data, entryCS, ShaderStage::Compute}};
        auto spirvDesc = ReflectSpirv(spvs).value();
        bindLayout = PipelineLayout{spirvDesc, {}};
    }
    return ComputeShaderCompileResult{std::move(csShader), std::move(bindLayout)};
}
#endif

}  // namespace radray::render
