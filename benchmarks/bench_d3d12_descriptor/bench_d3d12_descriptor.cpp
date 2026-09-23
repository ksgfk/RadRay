#include <benchmark/benchmark.h>

#include <cstdlib>
#include <string_view>

#include <radray/render/backend/d3d12_impl.h>
#include <radray/render/shader_layout.h>

namespace radray::render {
namespace {

ResolvedD3D12Binding MakeCBufferBinding(std::string_view name, uint32_t binding, uint32_t count, ShaderStages stages) {
    ResolvedD3D12Binding result{};
    result.Name = string{name};
    result.LogicalKind = shader::ShaderBindingKind::CBuffer;
    result.Group = 0;
    result.Binding = binding;
    result.Count = count;
    result.Stages = stages;
    result.Placement = shader::ShaderBindingPlacement::Table;
    return result;
}

bool MakeLayout(uint32_t scenario, ResolvedD3D12Layout& description) {
    description.Bindings = {MakeCBufferBinding("A", 0, scenario == 0 ? 1u : 64u, ShaderStage::Compute)};
    if (scenario == 4) {
        description.Bindings[0].Count = 32;
        description.Bindings[0].Stages = ShaderStage::Vertex;
        description.Bindings.push_back(MakeCBufferBinding("B", 32, 32, ShaderStage::Pixel));
    }
    if (scenario != 5) return true;
    description.Bindings[0].Stages = ShaderStage::Vertex | ShaderStage::Pixel;
    D3D12_DESCRIPTOR_RANGE1 range{D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 64, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_NONE, 0};
    D3D12_ROOT_PARAMETER1 parameters[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        parameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[i].DescriptorTable = {1, &range};
        parameters[i].ShaderVisibility = i == 0 ? D3D12_SHADER_VISIBILITY_VERTEX : D3D12_SHADER_VISIBILITY_PIXEL;
    }
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC native{};
    native.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    native.Desc_1_1.NumParameters = 2;
    native.Desc_1_1.pParameters = parameters;
    Microsoft::WRL::ComPtr<ID3DBlob> blob;
    Microsoft::WRL::ComPtr<ID3DBlob> error;
    if (FAILED(D3D12SerializeVersionedRootSignature(&native, &blob, &error))) return false;
    const auto* begin = static_cast<const byte*>(blob->GetBufferPointer());
    description.SerializedRootSignature.assign(begin, begin + blob->GetBufferSize());
    return true;
}

struct DescriptorFixture {
    unique_ptr<DXGIFactory> Factory;
    shared_ptr<Device> RenderDevice;
    unique_ptr<PipelineLayout> Layout;
    unique_ptr<Buffer> UploadBuffer;

    bool Initialize(uint32_t scenario) {
        const char* gpuValidation = std::getenv("RADRAY_TEST_GPU_VALIDATION");
        DXGIFactoryDescriptor factoryDesc{};
        factoryDesc.IsEnableDebugLayer = true;
        factoryDesc.IsEnableGpuBasedValid = gpuValidation && std::string_view{gpuValidation} == "1";
        auto factory = DXGIFactory::Create(factoryDesc);
        if (!factory) return false;
        Factory = factory.Release();
        const auto adapter = Factory->SelectHighPerformanceAdapter();
        if (!adapter) return false;
        D3D12DeviceDescriptor deviceDesc{};
        deviceDesc.Factory = Factory.get();
        deviceDesc.AdapterIndex = adapter;
        auto device = Device::Create(DeviceDescriptor{deviceDesc});
        if (!device) return false;
        RenderDevice = device.Release();
        ResolvedD3D12Layout description;
        if (!MakeLayout(scenario, description)) return false;
        auto layout = static_cast<d3d12::DeviceD3D12*>(RenderDevice.get())->CreatePipelineLayout(description);
        if (!layout) return false;
        Layout = layout.Release();
        auto buffer = RenderDevice->CreateBuffer({.Size = 512, .Memory = MemoryType::Upload, .Usage = BufferUse::CBuffer});
        if (!buffer) return false;
        UploadBuffer = buffer.Release();
        return true;
    }
};

void BM_DescriptorAllocation(benchmark::State& state, bool& failed) {
    DescriptorFixture fixture;
    if (!fixture.Initialize(uint32_t(state.range(0)))) {
        failed = true;
        state.SkipWithError("D3D12 descriptor fixture unavailable");
        return;
    }
    for (auto _ : state) {
        auto allocated = fixture.RenderDevice->CreateShaderParameterSet({.Layout = fixture.Layout.get(), .GroupIndex = 0});
        if (!allocated) {
            failed = true;
            state.SkipWithError("descriptor allocation failed");
            return;
        }
        benchmark::DoNotOptimize(allocated.Get());
    }
}

void BM_DescriptorPublish(benchmark::State& state, bool& failed) {
    const uint32_t scenario = uint32_t(state.range(0));
    DescriptorFixture fixture;
    if (!fixture.Initialize(scenario)) {
        failed = true;
        state.SkipWithError("D3D12 descriptor fixture unavailable");
        return;
    }
    auto result = fixture.RenderDevice->CreateShaderParameterSet({.Layout = fixture.Layout.get(), .GroupIndex = 0});
    if (!result) {
        failed = true;
        state.SkipWithError("descriptor set creation failed");
        return;
    }
    auto set = result.Release();
    const auto a = fixture.Layout->FindBinding("A");
    const auto b = fixture.Layout->FindBinding("B");
    uint64_t iteration = 0;
    for (auto _ : state) {
        const uint32_t count = scenario == 0 ? 1 : 64;
        const uint32_t step = scenario == 2 ? 7 : 1;
        for (uint32_t element = 0; element < count; element += step) {
            const auto handle = scenario == 4 && element >= 32 ? b : a;
            const auto index = scenario == 4 ? element % 32 : element;
            const ShaderBufferBinding value{fixture.UploadBuffer.get(), {uint64_t(iteration % 2) * 256, 256}, 0};
            if (!set->Set(handle, index, value)) {
                failed = true;
                state.SkipWithError("descriptor Set failed");
                return;
            }
            if (scenario == 3 && (!set->Set(handle, index, value) || !set->Set(handle, index, value))) {
                failed = true;
                state.SkipWithError("repeated descriptor Set failed");
                return;
            }
        }
        if (!set->FlushWrites()) {
            failed = true;
            state.SkipWithError("descriptor FlushWrites failed");
            return;
        }
        ++iteration;
    }
}

}  // namespace

int RunDescriptorBenchmarks(int argc, char** argv) {
    bool failed = false;
    for (int scenario = 0; scenario < 6; ++scenario) {
        benchmark::RegisterBenchmark("D3D12Descriptor/Allocation", [&failed](benchmark::State& state) { BM_DescriptorAllocation(state, failed); })->Arg(scenario)->Iterations(1000);
        benchmark::RegisterBenchmark("D3D12Descriptor/Publish", [&failed](benchmark::State& state) { BM_DescriptorPublish(state, failed); })->Arg(scenario)->Iterations(5000);
    }
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return failed ? 1 : 0;
}
}  // namespace radray::render

int main(int argc, char** argv) { return radray::render::RunDescriptorBenchmarks(argc, argv); }
