#include <radray/shader_compiler/client.h>

#if defined(_WIN32)

#include <radray/text_encoding.h>

#include <windows.h>
#include <objidl.h>
#include <unknwn.h>

#include <dxc/dxcapi.h>
#include <dxc/dxcapi_radrayext.h>
#include <wrl/client.h>

#include <cstring>
#include <limits>
#include <string_view>

namespace radray::shader_compiler {
namespace {

using std::string_view;
using shader::CompileDiagnostic;
using shader::CompileTargetLane;
using shader::CompileVariantRequest;
using shader::CompileVariantResult;
using shader::ContractHash;
using shader::ShaderTarget;

using Microsoft::WRL::ComPtr;

void AddForkError(vector<CompileDiagnostic>& diagnostics, uint32_t code, string_view message);

struct MarshaledIncludePaths {
    vector<string> Storage;
    vector<shader::RadRayDxcBlobView> Views;

    shader::RadRayDxcIncludePathListView View() const noexcept {
        return {
            Views.empty() ? nullptr : Views.data(),
            static_cast<uint32_t>(Views.size())};
    }
};

bool MarshalIncludePaths(
    std::span<const std::filesystem::path> includePaths,
    MarshaledIncludePaths& marshaled,
    vector<CompileDiagnostic>& diagnostics) {
    if (includePaths.size() > std::numeric_limits<uint32_t>::max()) {
        AddForkError(diagnostics, 2000, "include path list is too large");
        return false;
    }
    marshaled.Storage.reserve(includePaths.size());
    marshaled.Views.reserve(includePaths.size());
    for (const std::filesystem::path& path : includePaths) {
        const std::optional<string> encoded = ToMultiByte(std::wstring_view{path.native()});
        if (!encoded.has_value() || encoded->empty() || encoded->find('\0') != string::npos ||
            encoded->size() > std::numeric_limits<uint32_t>::max()) {
            AddForkError(diagnostics, 2000, "include path is not a valid UTF-8 path");
            return false;
        }
        marshaled.Storage.push_back(*encoded);
    }
    for (const string& path : marshaled.Storage) {
        marshaled.Views.push_back({
            reinterpret_cast<const uint8_t*>(path.data()),
            static_cast<uint32_t>(path.size())});
    }
    return true;
}

void AddForkError(vector<CompileDiagnostic>& diagnostics, uint32_t code, string_view message) {
    diagnostics.push_back({code, string{message}});
}

bool IsValidForkAbi(const shader::RadRayDxcAbiInfo& info) noexcept {
    if (info.AbiVersion != shader::kShaderCompilerAbiVersion ||
        info.MetadataSchemaVersion != shader::kShaderMetadataSchemaVersion ||
        info.ToolchainMajor != 1 || info.ToolchainMinor != 9) {
        return false;
    }
    for (const uint8_t value : info.ToolchainIdentity.Bytes) {
        if (value != 0) {
            return true;
        }
    }
    return false;
}

bool AcquireForkCompiler(
    const DynamicLibrary& library,
    ComPtr<shader::IRadRayDxcCompiler>& compiler,
    vector<CompileDiagnostic>* diagnostics = nullptr) {
    using DxcCreateInstanceFunction = decltype(&DxcCreateInstance);
    const DxcCreateInstanceFunction createInstance =
        library.GetFunction<DxcCreateInstanceFunction>("DxcCreateInstance");
    if (createInstance == nullptr) {
        if (diagnostics != nullptr) {
            AddForkError(*diagnostics, 2001, "RadRay DXC fork is missing DxcCreateInstance");
        }
        return false;
    }
    ComPtr<shader::IRadRayDxcCompiler> candidate;
    const HRESULT createResult = createInstance(
        shader::CLSID_RadRayDxcCompiler,
        shader::IID_IRadRayDxcCompiler,
        reinterpret_cast<void**>(candidate.GetAddressOf()));
    if (FAILED(createResult) || candidate == nullptr) {
        if (diagnostics != nullptr) {
            AddForkError(*diagnostics, 2002, "RadRay DXC fork rejected its extension CLSID");
        }
        return false;
    }
    shader::RadRayDxcAbiInfo info{};
    if (FAILED(candidate->GetAbiInfo(&info)) || !IsValidForkAbi(info)) {
        if (diagnostics != nullptr) {
            AddForkError(*diagnostics, 2008, "RadRay DXC fork ABI or toolchain identity is invalid");
        }
        return false;
    }
    compiler = std::move(candidate);
    return true;
}

void AppendForkDiagnostics(
    const ComPtr<shader::IRadRayDxcResult>& result,
    vector<CompileDiagnostic>& diagnostics) {
    if (result == nullptr) {
        return;
    }
    uint32_t count = 0;
    if (FAILED(result->GetDiagnosticCount(&count))) {
        AddForkError(diagnostics, 2008, "RadRay DXC result did not expose diagnostics");
        return;
    }
    for (uint32_t index = 0; index < count; ++index) {
        shader::RadRayDxcDiagnosticView diagnostic{};
        if (FAILED(result->GetDiagnostic(index, &diagnostic))) {
            AddForkError(diagnostics, 2008, "RadRay DXC result diagnostic access failed");
            return;
        }
        const char* data = reinterpret_cast<const char*>(diagnostic.MessageUtf8.Data);
        diagnostics.push_back({
            diagnostic.Code,
            string{data == nullptr ? "" : data, diagnostic.MessageUtf8.Size}});
    }
}

shader::CompileStatus MapForkStatus(shader::RadRayDxcCompileStatus status) noexcept {
    switch (status) {
        case shader::RadRayDxcCompileStatus::Success:
            return shader::CompileStatus::Success;
        case shader::RadRayDxcCompileStatus::InvalidRequest:
            return shader::CompileStatus::InvalidRequest;
        case shader::RadRayDxcCompileStatus::ContractMismatch:
            return shader::CompileStatus::ContractMismatch;
        case shader::RadRayDxcCompileStatus::TargetFailure:
            return shader::CompileStatus::TargetFailure;
    }
    return shader::CompileStatus::InvalidRequest;
}

bool DecodeForkContract(
    const ComPtr<shader::IRadRayDxcResult>& result,
    shader::ShaderContract& contract,
    vector<CompileDiagnostic>& diagnostics) {
    shader::RadRayDxcBlobView blob{};
    if (FAILED(result->GetContractBlob(&blob)) || blob.Data == nullptr || blob.Size == 0) {
        AddForkError(diagnostics, 2008, "RadRay DXC result has no contract blob");
        return false;
    }
    const DiscoveryResult decoded = DecodeWireShaderContract(
        std::span<const byte>{reinterpret_cast<const byte*>(blob.Data), blob.Size});
    if (!decoded.Succeeded()) {
        diagnostics.insert(diagnostics.end(), decoded.Diagnostics.begin(), decoded.Diagnostics.end());
        return false;
    }
    contract = decoded.Contract;
    return true;
}

bool PopulateForkStages(
    CompileTargetLane& lane,
    const shader::ShaderContract& contract,
    vector<CompileDiagnostic>& diagnostics) {
    if (lane.Metadata.size() < sizeof(shader::WireMetadataEnvelope)) {
        AddForkError(diagnostics, 2008, "RadRay metadata envelope is truncated");
        return false;
    }
    shader::WireMetadataEnvelope envelope{};
    std::memcpy(&envelope, lane.Metadata.data(), sizeof(envelope));
    if (envelope.Target != static_cast<uint8_t>(lane.Target) ||
        envelope.EntryRecords.Size != contract.EntryPoints.size() * sizeof(shader::WireEntryRecord) ||
        !envelope.EntryRecords.IsWithin(static_cast<uint32_t>(lane.Metadata.size())) ||
        !envelope.VertexInputRecords.IsWithin(static_cast<uint32_t>(lane.Metadata.size())) ||
        !envelope.RootSignature.IsWithin(static_cast<uint32_t>(lane.Metadata.size())) ||
        (lane.Target == shader::ShaderTarget::SPIRV && envelope.RootSignature.Size != 0) ||
        !envelope.Bytecode.IsWithin(static_cast<uint32_t>(lane.Metadata.size())) ||
        envelope.Bytecode.Size != lane.Bytecode.size()) {
        AddForkError(diagnostics, 2008, "RadRay metadata entry or bytecode range is invalid");
        return false;
    }
    const auto* entries = reinterpret_cast<const shader::WireEntryRecord*>(
        lane.Metadata.data() + envelope.EntryRecords.Offset);
    lane.Stages.clear();
    lane.Stages.reserve(contract.EntryPoints.size());
    for (size_t index = 0; index < contract.EntryPoints.size(); ++index) {
        const shader::WireEntryRecord& record = entries[index];
        const shader::EntryPoint& expected = contract.EntryPoints[index];
        if (record.Stage != static_cast<uint8_t>(expected.Stage) ||
            !record.Name.IsWithin(static_cast<uint32_t>(lane.Metadata.size())) ||
            record.InterfaceOffset > lane.Bytecode.size() ||
            record.InterfaceSize > lane.Bytecode.size() - record.InterfaceOffset) {
            AddForkError(diagnostics, 2008, "RadRay metadata entry interface is invalid");
            return false;
        }
        const string_view name{
            reinterpret_cast<const char*>(lane.Metadata.data() + record.Name.Offset),
            record.Name.Size};
        if (name != expected.Name) {
            AddForkError(diagnostics, 2008, "RadRay metadata entry name does not match contract");
            return false;
        }
        lane.Stages.push_back({
            expected.Stage,
            expected.Name,
            vector<byte>{
                lane.Bytecode.begin() + record.InterfaceOffset,
                lane.Bytecode.begin() + record.InterfaceOffset + record.InterfaceSize}});
    }
    return true;
}

bool CopyForkLane(
    const ComPtr<shader::IRadRayDxcResult>& result,
    shader::ShaderTarget target,
    const shader::ShaderContract& contract,
    CompileTargetLane& lane,
    vector<CompileDiagnostic>& diagnostics) {
    const shader::RadRayDxcTarget forkTarget = target == ShaderTarget::DXIL
        ? shader::RadRayDxcTarget::DXIL
        : shader::RadRayDxcTarget::SPIRV;
    shader::RadRayDxcLaneView view{};
    if (FAILED(result->GetTargetLane(forkTarget, &view)) || view.Bytecode.Data == nullptr ||
        view.Metadata.Data == nullptr || view.Bytecode.Size == 0 || view.Metadata.Size == 0) {
        AddForkError(diagnostics, 2008, "RadRay DXC result did not return the requested target lane");
        return false;
    }
    lane.Target = target;
    lane.Bytecode.assign(
        reinterpret_cast<const byte*>(view.Bytecode.Data),
        reinterpret_cast<const byte*>(view.Bytecode.Data) + view.Bytecode.Size);
    lane.Metadata.assign(
        reinterpret_cast<const byte*>(view.Metadata.Data),
        reinterpret_cast<const byte*>(view.Metadata.Data) + view.Metadata.Size);
    return PopulateForkStages(lane, contract, diagnostics);
}

}  // namespace

Client::Client(std::string_view compilerLibraryName) noexcept : _compilerLibrary(compilerLibraryName) {}

bool Client::IsAvailable() const noexcept {
    ComPtr<shader::IRadRayDxcCompiler> compiler;
    return _compilerLibrary.IsValid() && AcquireForkCompiler(_compilerLibrary, compiler);
}

DiscoveryResult Client::DiscoverSourceContract(
    std::string_view sourceName,
    std::span<const byte> source,
    shader::ShaderTarget target,
    std::span<const std::filesystem::path> includePaths) const {
    DiscoveryResult result;
    shader::SourceContractRequest discoveryRequest;
    discoveryRequest.SourceName = string{sourceName};
    discoveryRequest.RootSource.assign(source.begin(), source.end());
    discoveryRequest.Targets = static_cast<shader::ShaderTargetMask>(shader::ToTargetMask(target));
    const auto encoded = shader::EncodeSourceContractRequest(discoveryRequest);
    if (!encoded.has_value()) {
        result.Diagnostics.push_back({2000, "discovery request is invalid"});
        return result;
    }
    MarshaledIncludePaths marshaled;
    if (!MarshalIncludePaths(includePaths, marshaled, result.Diagnostics)) {
        return result;
    }
    ComPtr<shader::IRadRayDxcCompiler> compiler;
    if (!AcquireForkCompiler(_compilerLibrary, compiler, &result.Diagnostics)) {
        result.Diagnostics.push_back({2000, "RadRay DXC fork is unavailable"});
        return result;
    }
    const shader::RadRayDxcBlobView wireRequest{
        reinterpret_cast<const uint8_t*>(encoded->data()),
        static_cast<uint32_t>(encoded->size())};
    ComPtr<shader::IRadRayDxcResult> forkResult;
    const HRESULT hr = compiler->DiscoverSourceContract(
        wireRequest,
        marshaled.View(),
        reinterpret_cast<shader::IRadRayDxcResult**>(forkResult.GetAddressOf()));
    if (FAILED(hr) || forkResult == nullptr) {
        result.Diagnostics.push_back({2001, "RadRay DXC fork discovery call failed"});
        return result;
    }
    shader::RadRayDxcCompileStatus status{};
    if (FAILED(forkResult->GetStatus(&status))) {
        result.Diagnostics.push_back({2008, "RadRay DXC fork discovery result has no status"});
        return result;
    }
    AppendForkDiagnostics(forkResult, result.Diagnostics);
    result.Status = MapForkStatus(status);
    if (status != shader::RadRayDxcCompileStatus::Success) {
        return result;
    }
    if (!DecodeForkContract(forkResult, result.Contract, result.Diagnostics)) {
        result.Status = shader::CompileStatus::InvalidRequest;
    }
    return result;
}

CompileVariantResult Client::CompileVariant(
    const CompileVariantRequest& request,
    std::span<const std::filesystem::path> includePaths) const {
    CompileVariantResult result;
    const auto canonical = shader::CanonicalizeCompileVariantRequest(request);
    if (!canonical.has_value() || canonical->RootSource.empty() || !IsAvailable()) {
        result.Status = shader::CompileStatus::InvalidRequest;
        result.Diagnostics.push_back({2000, "compile request is invalid or RadRay DXC fork is unavailable"});
        return result;
    }
    MarshaledIncludePaths marshaled;
    if (!MarshalIncludePaths(includePaths, marshaled, result.Diagnostics)) {
        result.Status = shader::CompileStatus::InvalidRequest;
        return result;
    }
    const auto encoded = shader::EncodeCanonicalCompileVariantRequest(*canonical);
    if (!encoded.has_value()) {
        result.Status = shader::CompileStatus::InvalidRequest;
        result.Diagnostics.push_back({2000, "compile request canonicalization failed"});
        return result;
    }
    ComPtr<shader::IRadRayDxcCompiler> compiler;
    if (!AcquireForkCompiler(_compilerLibrary, compiler, &result.Diagnostics)) {
        result.Status = shader::CompileStatus::InvalidRequest;
        return result;
    }
    const shader::RadRayDxcBlobView wireRequest{
        reinterpret_cast<const uint8_t*>(encoded->data()),
        static_cast<uint32_t>(encoded->size())};
    ComPtr<shader::IRadRayDxcResult> forkResult;
    const HRESULT hr = compiler->CompileVariant(
        wireRequest,
        marshaled.View(),
        reinterpret_cast<shader::IRadRayDxcResult**>(forkResult.GetAddressOf()));
    if (FAILED(hr) || forkResult == nullptr) {
        result.Status = shader::CompileStatus::TargetFailure;
        result.Diagnostics.push_back({2003, "RadRay DXC fork compile call failed"});
        return result;
    }
    shader::RadRayDxcCompileStatus status{};
    if (FAILED(forkResult->GetStatus(&status))) {
        result.Status = shader::CompileStatus::TargetFailure;
        result.Diagnostics.push_back({2008, "RadRay DXC fork compile result has no status"});
        return result;
    }
    AppendForkDiagnostics(forkResult, result.Diagnostics);
    result.Status = MapForkStatus(status);
    if (status != shader::RadRayDxcCompileStatus::Success) {
        return result;
    }
    shader::ShaderContract contract;
    if (!DecodeForkContract(forkResult, contract, result.Diagnostics)) {
        result.Status = shader::CompileStatus::TargetFailure;
        result.Lanes.clear();
        return result;
    }
    for (const ShaderTarget target : {ShaderTarget::DXIL, ShaderTarget::SPIRV}) {
        if (!shader::HasTarget(canonical->Targets, target)) {
            continue;
        }
        CompileTargetLane lane;
        if (!CopyForkLane(forkResult, target, contract, lane, result.Diagnostics)) {
            result.Status = shader::CompileStatus::TargetFailure;
            result.Lanes.clear();
            return result;
        }
        result.Lanes.push_back(std::move(lane));
    }
    result.Status = result.Lanes.empty() ? shader::CompileStatus::InvalidRequest : shader::CompileStatus::Success;
    return result;
}

}  // namespace radray::shader_compiler

#else

namespace radray::shader_compiler {

Client::Client(std::string_view) noexcept : _compilerLibrary{} {}
bool Client::IsAvailable() const noexcept { return false; }
DiscoveryResult Client::DiscoverSourceContract(
    std::string_view sourceName,
    std::span<const byte> source,
    shader::ShaderTarget target,
    std::span<const std::filesystem::path>) const {
    (void)sourceName;
    (void)source;
    (void)target;
    DiscoveryResult result;
    result.Status = shader::CompileStatus::InvalidRequest;
    result.Diagnostics.push_back({2001, "RadRay DXC fork is unavailable on this platform"});
    return result;
}
shader::CompileVariantResult Client::CompileVariant(
    const shader::CompileVariantRequest&,
    std::span<const std::filesystem::path>) const {
    shader::CompileVariantResult result;
    result.Status = shader::CompileStatus::InvalidRequest;
    result.Diagnostics.push_back({2000, "shader compiler client is unavailable on this platform"});
    return result;
}

}  // namespace radray::shader_compiler

#endif
