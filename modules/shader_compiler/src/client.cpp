#include <radray/shader_compiler/client.h>

#include <fmt/format.h>

#if defined(_WIN32)

#include <radray/logger.h>
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

#endif

namespace radray::shader_compiler {
namespace {

template <typename T>
shader::DiagnosticResult<T> WithSourceContext(
    shader::DiagnosticResult<T> result, std::string_view sourceName, std::string_view operation) {
    if (!result.Diagnostics.empty()) {
        result.Diagnostics = fmt::format("{} '{}':\n{}", operation, sourceName, result.Diagnostics);
    }
    return result;
}

}  // namespace
}  // namespace radray::shader_compiler

#if defined(_WIN32)

namespace radray::shader_compiler {
namespace {

using std::string_view;
using shader::AppendDiagnostic;
using shader::CompileTargetLane;
using shader::CompileVariantRequest;
using shader::CompileVariantResult;
using shader::ContractHash;
using shader::ShaderTarget;

using Microsoft::WRL::ComPtr;

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
    string& diagnostics) {
    if (includePaths.size() > std::numeric_limits<uint32_t>::max()) {
        AppendDiagnostic(diagnostics, "include path list is too large");
        return false;
    }
    marshaled.Storage.reserve(includePaths.size());
    marshaled.Views.reserve(includePaths.size());
    for (const std::filesystem::path& path : includePaths) {
        const std::optional<string> encoded = ToMultiByte(std::wstring_view{path.native()});
        if (!encoded.has_value() || encoded->empty() || encoded->find('\0') != string::npos ||
            encoded->size() > std::numeric_limits<uint32_t>::max()) {
            AppendDiagnostic(diagnostics, "include path is not a valid UTF-8 path");
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
    string& diagnostics) {
    if (!library.IsValid()) {
        AppendDiagnostic(diagnostics, "RadRay DXC compiler library is unavailable");
        return false;
    }
    using DxcCreateInstanceFunction = decltype(&DxcCreateInstance);
    const DxcCreateInstanceFunction createInstance =
        library.GetFunction<DxcCreateInstanceFunction>("DxcCreateInstance");
    if (createInstance == nullptr) {
        AppendDiagnostic(diagnostics, "RadRay DXC fork is missing DxcCreateInstance");
        return false;
    }
    ComPtr<shader::IRadRayDxcCompiler> candidate;
    const HRESULT createResult = createInstance(
        shader::CLSID_RadRayDxcCompiler,
        shader::IID_IRadRayDxcCompiler,
        reinterpret_cast<void**>(candidate.GetAddressOf()));
    if (FAILED(createResult) || candidate == nullptr) {
        AppendDiagnostic(diagnostics, "RadRay DXC fork rejected its extension CLSID");
        return false;
    }
    shader::RadRayDxcAbiInfo info{};
    if (FAILED(candidate->GetAbiInfo(&info)) || !IsValidForkAbi(info)) {
        AppendDiagnostic(diagnostics, "RadRay DXC fork ABI or toolchain identity is invalid");
        return false;
    }
    compiler = std::move(candidate);
    return true;
}

bool AppendForkDiagnostics(
    const ComPtr<shader::IRadRayDxcResult>& result,
    string& diagnostics) {
    uint32_t count = 0;
    if (result == nullptr || FAILED(result->GetDiagnosticCount(&count))) {
        AppendDiagnostic(diagnostics, "RadRay DXC result did not expose diagnostics");
        return false;
    }
    for (uint32_t index = 0; index < count; ++index) {
        shader::RadRayDxcDiagnosticView diagnostic{};
        if (FAILED(result->GetDiagnostic(index, &diagnostic)) ||
            (diagnostic.MessageUtf8.Data == nullptr && diagnostic.MessageUtf8.Size != 0)) {
            AppendDiagnostic(diagnostics, "RadRay DXC result diagnostic access failed");
            return false;
        }
        if (diagnostic.MessageUtf8.Size != 0) {
            AppendDiagnostic(diagnostics, string_view{
                                              reinterpret_cast<const char*>(diagnostic.MessageUtf8.Data), diagnostic.MessageUtf8.Size});
        }
    }
    return true;
}

bool DecodeForkContract(
    const ComPtr<shader::IRadRayDxcResult>& result,
    shader::ShaderContract& contract,
    string& diagnostics) {
    shader::RadRayDxcBlobView blob{};
    if (FAILED(result->GetContractBlob(&blob)) || blob.Data == nullptr || blob.Size == 0) {
        AppendDiagnostic(diagnostics, "RadRay DXC result has no contract blob");
        return false;
    }
    DiscoveryResult decoded = DecodeWireShaderContract(
        std::span<const byte>{reinterpret_cast<const byte*>(blob.Data), blob.Size});
    if (!decoded.Succeeded()) {
        AppendDiagnostic(diagnostics, decoded.Diagnostics);
        return false;
    }
    contract = std::move(*decoded.Value);
    return true;
}

bool PopulateForkStages(
    CompileTargetLane& lane,
    const shader::ShaderContract& contract,
    string& diagnostics) {
    if (lane.Metadata.size() < sizeof(shader::WireMetadataEnvelope)) {
        AppendDiagnostic(diagnostics, "RadRay metadata envelope is truncated");
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
        AppendDiagnostic(diagnostics, "RadRay metadata entry or bytecode range is invalid");
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
            AppendDiagnostic(diagnostics, "RadRay metadata entry interface is invalid");
            return false;
        }
        const string_view name{
            reinterpret_cast<const char*>(lane.Metadata.data() + record.Name.Offset),
            record.Name.Size};
        if (name != expected.Name) {
            AppendDiagnostic(diagnostics, "RadRay metadata entry name does not match contract");
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
    string& diagnostics) {
    const shader::RadRayDxcTarget forkTarget = target == ShaderTarget::DXIL
        ? shader::RadRayDxcTarget::DXIL
        : shader::RadRayDxcTarget::SPIRV;
    shader::RadRayDxcLaneView view{};
    if (FAILED(result->GetTargetLane(forkTarget, &view)) || view.Bytecode.Data == nullptr ||
        view.Metadata.Data == nullptr || view.Bytecode.Size == 0 || view.Metadata.Size == 0) {
        AppendDiagnostic(diagnostics, "RadRay DXC result did not return the requested target lane");
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

Client::Client(std::string_view compilerLibraryName) : _compilerLibrary(compilerLibraryName) {
    ComPtr<shader::IRadRayDxcCompiler> compiler;
    string diagnostics;
    if (!_compilerLibrary.IsValid()) return;
    if (!AcquireForkCompiler(_compilerLibrary, compiler, diagnostics)) {
        RADRAY_ERR_LOG("shader compiler unavailable: {}", diagnostics);
        return;
    }
    shader::RadRayDxcAbiInfo info{};
    if (FAILED(compiler->GetAbiInfo(&info))) return;
    shader::Hash128 identity{};
    static_assert(sizeof(info.ToolchainIdentity) == sizeof(identity.Bytes));
    std::memcpy(identity.Bytes.data(), &info.ToolchainIdentity, sizeof(identity.Bytes));
    _toolchainIdentity = identity;
}

bool Client::IsAvailable() const noexcept {
    return _toolchainIdentity.has_value();
}

std::optional<shader::Hash128> Client::GetToolchainIdentity() const noexcept {
    return _toolchainIdentity;
}

DiscoveryResult Client::DiscoverSourceContract(
    std::string_view sourceName,
    std::span<const byte> source,
    shader::ShaderTarget target,
    std::span<const std::filesystem::path> includePaths) const {
    shader::SourceContractRequest discoveryRequest;
    discoveryRequest.SourceName = string{sourceName};
    discoveryRequest.RootSource.assign(source.begin(), source.end());
    discoveryRequest.Targets = static_cast<shader::ShaderTargetMask>(shader::ToTargetMask(target));
    return DiscoverSourceContract(discoveryRequest, includePaths);
}

DiscoveryResult Client::DiscoverSourceContract(
    const shader::SourceContractRequest& request,
    std::span<const std::filesystem::path> includePaths) const {
    DiscoveryResult result;
    const auto encoded = shader::EncodeSourceContractRequest(request);
    if (!encoded.Succeeded()) {
        shader::AppendDiagnostic(result.Diagnostics, encoded.Diagnostics);
        return WithSourceContext(std::move(result), request.SourceName, "discover");
    }
    MarshaledIncludePaths marshaled;
    if (!MarshalIncludePaths(includePaths, marshaled, result.Diagnostics)) {
        return WithSourceContext(std::move(result), request.SourceName, "discover");
    }
    ComPtr<shader::IRadRayDxcCompiler> compiler;
    if (!AcquireForkCompiler(_compilerLibrary, compiler, result.Diagnostics)) {
        shader::AppendDiagnostic(result.Diagnostics, "RadRay DXC fork is unavailable");
        return WithSourceContext(std::move(result), request.SourceName, "discover");
    }
    const shader::RadRayDxcBlobView wireRequest{
        reinterpret_cast<const uint8_t*>(encoded.Value->data()),
        static_cast<uint32_t>(encoded.Value->size())};
    ComPtr<shader::IRadRayDxcResult> forkResult;
    const HRESULT hr = compiler->DiscoverSourceContract(
        wireRequest,
        marshaled.View(),
        reinterpret_cast<shader::IRadRayDxcResult**>(forkResult.GetAddressOf()));
    if (FAILED(hr) || forkResult == nullptr) {
        shader::AppendDiagnostic(result.Diagnostics, "RadRay DXC fork discovery call failed");
        return WithSourceContext(std::move(result), request.SourceName, "discover");
    }
    shader::RadRayDxcCompileStatus status{};
    if (FAILED(forkResult->GetStatus(&status))) {
        shader::AppendDiagnostic(result.Diagnostics, "RadRay DXC fork discovery result has no status");
        return WithSourceContext(std::move(result), request.SourceName, "discover");
    }
    if (!AppendForkDiagnostics(forkResult, result.Diagnostics)) return WithSourceContext(std::move(result), request.SourceName, "discover");
    if (status != shader::RadRayDxcCompileStatus::Success) {
        if (result.Diagnostics.empty()) {
            AppendDiagnostic(result.Diagnostics, "RadRay DXC rejected the request without diagnostic text");
        }
        return WithSourceContext(std::move(result), request.SourceName, "discover");
    }
    shader::ShaderContract contract;
    if (!DecodeForkContract(forkResult, contract, result.Diagnostics)) return WithSourceContext(std::move(result), request.SourceName, "discover");
    result.Value = std::move(contract);
    return WithSourceContext(std::move(result), request.SourceName, "discover");
}

CompileVariantResult Client::CompileVariant(
    const CompileVariantRequest& request,
    std::span<const std::filesystem::path> includePaths) const {
    CompileVariantResult result;
    const auto canonical = shader::CanonicalizeCompileVariantRequest(request);
    if (!canonical.Succeeded()) {
        shader::AppendDiagnostic(result.Diagnostics, canonical.Diagnostics);
        return WithSourceContext(std::move(result), request.SourceName, "compile");
    }
    if (canonical.Value->RootSource.empty()) {
        shader::AppendDiagnostic(result.Diagnostics, "RootSource must not be empty");
        return WithSourceContext(std::move(result), request.SourceName, "compile");
    }
    MarshaledIncludePaths marshaled;
    if (!MarshalIncludePaths(includePaths, marshaled, result.Diagnostics)) {
        return WithSourceContext(std::move(result), request.SourceName, "compile");
    }
    const auto encoded = shader::EncodeCanonicalCompileVariantRequest(*canonical.Value);
    if (!encoded.Succeeded()) {
        shader::AppendDiagnostic(result.Diagnostics, encoded.Diagnostics);
        return WithSourceContext(std::move(result), request.SourceName, "compile");
    }
    ComPtr<shader::IRadRayDxcCompiler> compiler;
    if (!AcquireForkCompiler(_compilerLibrary, compiler, result.Diagnostics)) {
        return WithSourceContext(std::move(result), request.SourceName, "compile");
    }
    const shader::RadRayDxcBlobView wireRequest{
        reinterpret_cast<const uint8_t*>(encoded.Value->data()),
        static_cast<uint32_t>(encoded.Value->size())};
    ComPtr<shader::IRadRayDxcResult> forkResult;
    const HRESULT hr = compiler->CompileVariant(
        wireRequest,
        marshaled.View(),
        reinterpret_cast<shader::IRadRayDxcResult**>(forkResult.GetAddressOf()));
    if (FAILED(hr) || forkResult == nullptr) {
        shader::AppendDiagnostic(result.Diagnostics, "RadRay DXC fork compile call failed");
        return WithSourceContext(std::move(result), request.SourceName, "compile");
    }
    shader::RadRayDxcCompileStatus status{};
    if (FAILED(forkResult->GetStatus(&status))) {
        shader::AppendDiagnostic(result.Diagnostics, "RadRay DXC fork compile result has no status");
        return WithSourceContext(std::move(result), request.SourceName, "compile");
    }
    if (!AppendForkDiagnostics(forkResult, result.Diagnostics)) return WithSourceContext(std::move(result), request.SourceName, "compile");
    if (status != shader::RadRayDxcCompileStatus::Success) {
        if (result.Diagnostics.empty()) {
            AppendDiagnostic(result.Diagnostics, "RadRay DXC rejected the request without diagnostic text");
        }
        return WithSourceContext(std::move(result), request.SourceName, "compile");
    }
    shader::ShaderContract contract;
    if (!DecodeForkContract(forkResult, contract, result.Diagnostics)) {
        return WithSourceContext(std::move(result), request.SourceName, "compile");
    }
    vector<CompileTargetLane> lanes;
    for (const ShaderTarget target : {ShaderTarget::DXIL, ShaderTarget::SPIRV}) {
        if (!shader::HasTarget(canonical.Value->Targets, target)) {
            continue;
        }
        CompileTargetLane lane;
        if (!CopyForkLane(forkResult, target, contract, lane, result.Diagnostics)) {
            AppendDiagnostic(result.Diagnostics, target == ShaderTarget::DXIL ? "reading DXIL lane failed" : "reading SPIR-V lane failed");
            return WithSourceContext(std::move(result), request.SourceName, "compile");
        }
        lanes.push_back(std::move(lane));
    }
    if (lanes.empty()) {
        AppendDiagnostic(result.Diagnostics, "Targets did not select any compilation lane");
        return WithSourceContext(std::move(result), request.SourceName, "compile");
    }
    result.Value = std::move(lanes);
    return WithSourceContext(std::move(result), request.SourceName, "compile");
}

}  // namespace radray::shader_compiler

#else

namespace radray::shader_compiler {

Client::Client(std::string_view) : _compilerLibrary{} {}
bool Client::IsAvailable() const noexcept { return false; }
std::optional<shader::Hash128> Client::GetToolchainIdentity() const noexcept { return std::nullopt; }
DiscoveryResult Client::DiscoverSourceContract(
    std::string_view sourceName,
    std::span<const byte> source,
    shader::ShaderTarget target,
    std::span<const std::filesystem::path> includePaths) const {
    shader::SourceContractRequest request;
    request.SourceName = string{sourceName};
    request.RootSource.assign(source.begin(), source.end());
    request.Targets = static_cast<shader::ShaderTargetMask>(shader::ToTargetMask(target));
    return DiscoverSourceContract(request, includePaths);
}
DiscoveryResult Client::DiscoverSourceContract(
    const shader::SourceContractRequest& request,
    std::span<const std::filesystem::path>) const {
    DiscoveryResult result;
    shader::AppendDiagnostic(result.Diagnostics, "RadRay DXC fork is unavailable on this platform");
    return WithSourceContext(std::move(result), request.SourceName, "discover");
}
shader::CompileVariantResult Client::CompileVariant(
    const shader::CompileVariantRequest& request,
    std::span<const std::filesystem::path>) const {
    shader::CompileVariantResult result;
    shader::AppendDiagnostic(result.Diagnostics, "shader compiler client is unavailable on this platform");
    return WithSourceContext(std::move(result), request.SourceName, "compile");
}

}  // namespace radray::shader_compiler

#endif
