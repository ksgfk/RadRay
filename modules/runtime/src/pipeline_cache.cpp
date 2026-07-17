#include <radray/runtime/pipeline_cache.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

#include <fmt/format.h>
#include <xxhash.h>

#include <radray/file.h>
#include <radray/logger.h>
#include <radray/render/shader_compiler/dxc.h>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/shader_asset.h>

#if defined(RADRAY_ENABLE_SPIRV_CROSS)
#include <radray/render/shader_compiler/spvc.h>
#endif

namespace radray {

namespace {

constexpr uint32_t kCacheFormatVersion = 1;
constexpr uint32_t kMaxSectionCount = 16;
constexpr uint32_t kMaxRecordCount = 1u << 20;
constexpr uint32_t kMaxStringSize = 64u << 20;
constexpr uint64_t kMaxBlobSize = 1ull << 30;
constexpr uint64_t kMaxCacheFileSize = 1ull << 30;
constexpr std::array<uint8_t, 8> kShaderCacheMagic{'R', 'D', 'R', 'S', 'H', 'D', 'R', 0};
constexpr std::array<uint8_t, 8> kGraphicsCacheMagic{'R', 'D', 'R', 'G', 'P', 'I', 'P', 0};

enum class CacheSectionKind : uint32_t {
    ShaderModules = 1,
    PipelineLayouts = 2,
    GraphicsPsos = 3,
    NativePipelineCache = 4,
};

class BinaryWriter {
public:
    void WriteU8(uint8_t value) { Data.emplace_back(static_cast<byte>(value)); }
    void WriteU32(uint32_t value) {
        for (uint32_t i = 0; i < 4; ++i) {
            WriteU8(static_cast<uint8_t>((value >> (i * 8)) & 0xffu));
        }
    }
    void WriteU64(uint64_t value) {
        for (uint32_t i = 0; i < 8; ++i) {
            WriteU8(static_cast<uint8_t>((value >> (i * 8)) & 0xffu));
        }
    }
    void WriteI32(int32_t value) { WriteU32(std::bit_cast<uint32_t>(value)); }
    void WriteFloat(float value) { WriteU32(std::bit_cast<uint32_t>(value)); }
    void WriteBool(bool value) { WriteU8(value ? 1 : 0); }
    void WriteBytes(std::span<const byte> value) { Data.insert(Data.end(), value.begin(), value.end()); }
    void WriteSizedBytes(std::span<const byte> value) {
        if (value.size() > kMaxBlobSize) {
            throw std::length_error{"pipeline cache blob is too large"};
        }
        WriteU64(static_cast<uint64_t>(value.size()));
        WriteBytes(value);
    }
    void WriteString(std::string_view value) {
        if (value.size() > kMaxStringSize) {
            throw std::length_error{"pipeline cache string is too large"};
        }
        WriteU32(static_cast<uint32_t>(value.size()));
        WriteBytes(std::as_bytes(std::span{value.data(), value.size()}));
    }

    vector<byte> Data{};
};

class BinaryReader {
public:
    explicit BinaryReader(std::span<const byte> data) noexcept : _data(data) {}
    bool ReadU8(uint8_t& value) noexcept {
        if (_offset >= _data.size()) return false;
        value = std::to_integer<uint8_t>(_data[_offset++]);
        return true;
    }
    bool ReadU32(uint32_t& value) noexcept {
        value = 0;
        for (uint32_t i = 0; i < 4; ++i) {
            uint8_t part = 0;
            if (!ReadU8(part)) return false;
            value |= static_cast<uint32_t>(part) << (i * 8);
        }
        return true;
    }
    bool ReadU64(uint64_t& value) noexcept {
        value = 0;
        for (uint32_t i = 0; i < 8; ++i) {
            uint8_t part = 0;
            if (!ReadU8(part)) return false;
            value |= static_cast<uint64_t>(part) << (i * 8);
        }
        return true;
    }
    bool ReadI32(int32_t& value) noexcept {
        uint32_t raw = 0;
        if (!ReadU32(raw)) return false;
        value = std::bit_cast<int32_t>(raw);
        return true;
    }
    bool ReadFloat(float& value) noexcept {
        uint32_t raw = 0;
        if (!ReadU32(raw)) return false;
        value = std::bit_cast<float>(raw);
        return true;
    }
    bool ReadBool(bool& value) noexcept {
        uint8_t raw = 0;
        if (!ReadU8(raw) || raw > 1) return false;
        value = raw != 0;
        return true;
    }
    bool ReadBytes(size_t size, std::span<const byte>& value) noexcept {
        if (size > _data.size() - _offset) return false;
        value = _data.subspan(_offset, size);
        _offset += size;
        return true;
    }
    bool ReadSizedBytes(vector<byte>& value) {
        uint64_t size = 0;
        if (!ReadU64(size) || size > kMaxBlobSize || size > Remaining()) return false;
        std::span<const byte> bytes;
        if (!ReadBytes(static_cast<size_t>(size), bytes)) return false;
        value.assign(bytes.begin(), bytes.end());
        return true;
    }
    bool ReadString(string& value) {
        uint32_t size = 0;
        if (!ReadU32(size) || size > kMaxStringSize || size > Remaining()) return false;
        std::span<const byte> bytes;
        if (!ReadBytes(size, bytes)) return false;
        value.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return true;
    }
    size_t Remaining() const noexcept { return _data.size() - _offset; }
    bool IsAtEnd() const noexcept { return _offset == _data.size(); }

private:
    std::span<const byte> _data{};
    size_t _offset{0};
};

struct CacheSection {
    CacheSectionKind Kind{};
    vector<byte> Data{};
};

void WriteShaderModuleKey(BinaryWriter&, const ShaderModuleKey&);
bool ReadShaderModuleKey(BinaryReader&, ShaderModuleKey&);
void WritePipelineLayoutKey(BinaryWriter&, const PipelineLayoutCacheKey&);
bool ReadPipelineLayoutKey(BinaryReader&, PipelineLayoutCacheKey&);
void WriteGraphicsPsoKey(BinaryWriter&, const GraphicsPsoCacheKey&);
bool ReadGraphicsPsoKey(BinaryReader&, GraphicsPsoCacheKey&);
void WriteHash(BinaryWriter&, const PipelineCacheHash&);
bool ReadHash(BinaryReader&, PipelineCacheHash&) noexcept;
PipelineCacheHash HashBytes(std::span<const byte>) noexcept;
string HashToHex(PipelineCacheHash);
bool WriteCacheFile(
    const std::filesystem::path&,
    const std::array<uint8_t, 8>&,
    render::RenderBackend,
    std::span<const CacheSection>) noexcept;
std::optional<vector<CacheSection>> ReadCacheFile(
    const std::filesystem::path&,
    const std::array<uint8_t, 8>&,
    render::RenderBackend) noexcept;
const CacheSection* FindSection(std::span<const CacheSection>, CacheSectionKind) noexcept;
std::filesystem::path ShaderCachePath(const std::filesystem::path&, render::RenderBackend);
std::filesystem::path GraphicsCachePath(const std::filesystem::path&, render::RenderBackend);
void NormalizeShaderModuleKey(ShaderModuleKey&);
std::optional<std::string_view> FindShaderEntryPoint(const ShaderPassDesc&, render::ShaderStage) noexcept;
bool AreShaderDefinesValid(const ShaderPassDesc&, render::ShaderStage, const vector<string>&) noexcept;
std::optional<string> SerializeReflection(const render::ShaderReflectionDesc&) noexcept;
std::optional<render::ShaderReflectionDesc> DeserializeReflection(uint8_t, std::string_view) noexcept;

}  // namespace

ShaderModuleCache::ShaderModuleCache(
    render::Device* device,
    render::Dxc* dxc,
    AssetManager* assetManager,
    string shaderSourceRoot,
    std::filesystem::path cacheDirectory) noexcept
    : _device(device),
      _dxc(dxc),
      _assetManager(assetManager),
      _shaderSourceRoot(std::move(shaderSourceRoot)),
      _cacheDirectory(std::move(cacheDirectory)) {
    LoadFromDisk();
}

ShaderModuleCache::~ShaderModuleCache() noexcept {
    FlushToDisk();
    Clear();
}

bool ShaderModuleCache::LoadFromDisk() noexcept {
    if (_device == nullptr || _cacheDirectory.empty()) {
        return false;
    }
    const std::filesystem::path path = ShaderCachePath(_cacheDirectory, _device->GetBackend());
    std::optional<vector<CacheSection>> sections =
        ReadCacheFile(path, kShaderCacheMagic, _device->GetBackend());
    if (!sections.has_value()) {
        return false;
    }
    const CacheSection* shaderSection = FindSection(*sections, CacheSectionKind::ShaderModules);
    if (shaderSection == nullptr) {
        RADRAY_WARN_LOG("shader cache file '{}' has no shader section", path.string());
        return false;
    }

    try {
        BinaryReader reader{shaderSection->Data};
        uint32_t count = 0;
        if (!reader.ReadU32(count) || count > kMaxRecordCount) {
            return false;
        }
        unordered_map<ShaderModuleKey, Entry> loaded;
        loaded.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            ShaderModuleKey key{};
            int32_t categoryValue = 0;
            Entry entry{};
            uint8_t reflectionKind = 0;
            if (!ReadShaderModuleKey(reader, key) || !reader.ReadI32(categoryValue) ||
                !ReadHash(reader, entry.BinaryHash) || !ReadHash(reader, entry.InterfaceHash) ||
                !reader.ReadSizedBytes(entry.Bytecode) || !reader.ReadU8(reflectionKind) ||
                !reader.ReadString(entry.ReflectionPayload)) {
                RADRAY_WARN_LOG("ignoring malformed shader cache file '{}'", path.string());
                return false;
            }
            ShaderModuleKey normalized = key;
            NormalizeShaderModuleKey(normalized);
            if (normalized != key || entry.Bytecode.empty() ||
                HashBytes(entry.Bytecode) != entry.BinaryHash ||
                HashBytes(std::as_bytes(std::span{entry.ReflectionPayload.data(), entry.ReflectionPayload.size()})) !=
                    entry.InterfaceHash) {
                RADRAY_WARN_LOG("ignoring invalid shader record in '{}'", path.string());
                return false;
            }
            entry.Category = static_cast<render::ShaderBlobCategory>(categoryValue);
            const render::RenderBackend backend = _device->GetBackend();
            if ((backend == render::RenderBackend::D3D12 &&
                 entry.Category != render::ShaderBlobCategory::DXIL) ||
                (backend == render::RenderBackend::Vulkan &&
                 entry.Category != render::ShaderBlobCategory::SPIRV)) {
                RADRAY_WARN_LOG("ignoring shader cache file '{}' with an invalid bytecode category", path.string());
                return false;
            }
            entry.Reflection = DeserializeReflection(reflectionKind, entry.ReflectionPayload);
            if (!entry.Reflection.has_value() || !loaded.emplace(std::move(key), std::move(entry)).second) {
                RADRAY_WARN_LOG("ignoring shader cache file '{}' with invalid reflection or duplicate keys", path.string());
                return false;
            }
        }
        if (!reader.IsAtEnd()) {
            RADRAY_WARN_LOG("ignoring shader cache file '{}' with trailing section data", path.string());
            return false;
        }
        _entries = std::move(loaded);
        return true;
    } catch (...) {
        RADRAY_WARN_LOG("ignoring shader cache file '{}' after a decode failure", path.string());
        return false;
    }
}

Nullable<render::Shader*> ShaderModuleCache::Materialize(
    const ShaderModuleKey& key,
    Entry& entry) noexcept {
    if (entry.Object != nullptr) {
        return entry.Object.get();
    }
    if (_device == nullptr || entry.Bytecode.empty() || !entry.Reflection.has_value()) {
        return nullptr;
    }
    render::ShaderDescriptor shaderDesc{
        .Source = entry.Bytecode,
        .Category = entry.Category,
        .Stages = key.Stage,
        .Reflection = entry.Reflection};
    auto shaderOpt = _device->CreateShader(shaderDesc);
    if (!shaderOpt.HasValue()) {
        RADRAY_ERR_LOG(
            "ShaderModuleCache::Materialize: Device::CreateShader failed for ShaderAsset {} pass {} stage {}",
            key.Shader,
            key.PassIndex,
            key.Stage);
        return nullptr;
    }
    entry.Object = shaderOpt.Release();
    render::Shader* result = entry.Object.get();
    _shaderKeys.insert_or_assign(result, key);
    return result;
}

Nullable<render::Shader*> ShaderModuleCache::GetOrCreate(const ShaderModuleKey& sourceKey) noexcept {
    ShaderModuleKey key = sourceKey;
    NormalizeShaderModuleKey(key);
    if (auto cached = _entries.find(key); cached != _entries.end()) {
        return Materialize(cached->first, cached->second);
    }

#if !defined(RADRAY_ENABLE_DXC)
    RADRAY_ERR_LOG("ShaderModuleCache::GetOrCreate: cache miss and DXC is not enabled");
    return nullptr;
#else
    if (_device == nullptr || _dxc == nullptr || _assetManager == nullptr) {
        RADRAY_ERR_LOG("ShaderModuleCache::GetOrCreate: device, DXC, or AssetManager is null");
        return nullptr;
    }
    if (key.Shader.IsEmpty()) {
        RADRAY_ERR_LOG("ShaderModuleCache::GetOrCreate: shader asset id is empty");
        return nullptr;
    }

    StreamingAssetRef<ShaderAsset> shaderRef = _assetManager->Get<ShaderAsset>(key.Shader);
    ShaderAsset* shaderAsset = shaderRef.Get();
    if (shaderAsset == nullptr || !shaderAsset->IsValid()) {
        RADRAY_ERR_LOG("ShaderModuleCache::GetOrCreate: ShaderAsset {} is not ready or invalid", key.Shader);
        return nullptr;
    }
    const vector<ShaderPassDesc>& passes = shaderAsset->GetPasses();
    if (key.PassIndex >= passes.size()) {
        RADRAY_ERR_LOG(
            "ShaderModuleCache::GetOrCreate: pass {} is out of range for ShaderAsset {}",
            key.PassIndex,
            key.Shader);
        return nullptr;
    }
    const ShaderPassDesc& pass = passes[key.PassIndex];
    const std::optional<std::string_view> entryPoint = FindShaderEntryPoint(pass, key.Stage);
    if (!entryPoint.has_value() || !AreShaderDefinesValid(pass, key.Stage, key.Defines)) {
        RADRAY_ERR_LOG(
            "ShaderModuleCache::GetOrCreate: invalid stage or defines for ShaderAsset {} pass {} stage {}",
            key.Shader,
            key.PassIndex,
            key.Stage);
        return nullptr;
    }

    const std::filesystem::path relativeSourcePath{pass.SourcePath};
    std::filesystem::path sourcePath;
    std::error_code fileError;
    const std::filesystem::path executableDirectory = GetExecutableDirectory();
    if (!executableDirectory.empty()) {
        const std::filesystem::path candidate = executableDirectory / relativeSourcePath;
        if (std::filesystem::is_regular_file(candidate, fileError)) {
            sourcePath = candidate;
        }
    }
    if (sourcePath.empty() && !_shaderSourceRoot.empty()) {
        const std::filesystem::path candidate =
            std::filesystem::path{_shaderSourceRoot} / relativeSourcePath;
        fileError.clear();
        if (std::filesystem::is_regular_file(candidate, fileError)) {
            sourcePath = candidate;
        }
    }
    if (sourcePath.empty()) {
        RADRAY_ERR_LOG("ShaderModuleCache::GetOrCreate: shader source '{}' not found", pass.SourcePath);
        return nullptr;
    }

    vector<std::string_view> defines;
    defines.reserve(key.Defines.size());
    for (const string& define : key.Defines) {
        defines.emplace_back(define);
    }
    const std::string_view includeRoot = _shaderSourceRoot;
    const std::array includeDirs{includeRoot};
    const render::RenderBackend backend = _device->GetBackend();
    if (backend != render::RenderBackend::D3D12 && backend != render::RenderBackend::Vulkan) {
        RADRAY_ERR_LOG("ShaderModuleCache::GetOrCreate: unsupported render backend {}", backend);
        return nullptr;
    }

    render::DxcCompileOptions compileOptions{};
    compileOptions.EntryPoint = *entryPoint;
    compileOptions.Stage = key.Stage;
    compileOptions.SM = pass.SM;
    compileOptions.Defines = defines;
    compileOptions.Includes = includeDirs;
    compileOptions.IsOptimize = pass.IsOptimize;
    compileOptions.IsSpirv = backend == render::RenderBackend::Vulkan;
    compileOptions.EnableUnbounded = pass.EnableUnbounded;
    std::optional<render::DxcOutput> outputOpt = _dxc->CompileFile(sourcePath, compileOptions);
    if (!outputOpt.has_value()) {
        RADRAY_ERR_LOG(
            "ShaderModuleCache::GetOrCreate: DXC failed for '{}' entry '{}' stage {}",
            sourcePath.string(),
            *entryPoint,
            key.Stage);
        return nullptr;
    }
    render::DxcOutput output = std::move(*outputOpt);

    render::ShaderReflectionDesc reflection{};
    if (backend == render::RenderBackend::D3D12) {
        std::optional<render::HlslShaderDesc> value = _dxc->GetShaderDescFromOutput(output.Refl);
        if (!value.has_value()) {
            RADRAY_ERR_LOG("ShaderModuleCache::GetOrCreate: DXIL reflection failed for '{}'", sourcePath.string());
            return nullptr;
        }
        reflection = std::move(*value);
    } else {
#if defined(RADRAY_ENABLE_SPIRV_CROSS)
        std::optional<render::SpirvShaderDesc> value = render::ReflectSpirv(render::SpirvBytecodeView{
            .Data = output.Data,
            .EntryPointName = *entryPoint,
            .Stage = key.Stage});
        if (!value.has_value()) {
            RADRAY_ERR_LOG("ShaderModuleCache::GetOrCreate: SPIR-V reflection failed for '{}'", sourcePath.string());
            return nullptr;
        }
        reflection = std::move(*value);
#else
        RADRAY_ERR_LOG("ShaderModuleCache::GetOrCreate: SPIR-V Cross is not enabled");
        return nullptr;
#endif
    }

    std::optional<string> reflectionPayload = SerializeReflection(reflection);
    if (!reflectionPayload.has_value()) {
        RADRAY_ERR_LOG("ShaderModuleCache::GetOrCreate: reflection serialization failed for '{}'", sourcePath.string());
        return nullptr;
    }
    const uint8_t reflectionKind = static_cast<uint8_t>(reflection.index());
    std::optional<render::ShaderReflectionDesc> normalizedReflection =
        DeserializeReflection(reflectionKind, *reflectionPayload);
    if (!normalizedReflection.has_value()) {
        RADRAY_ERR_LOG("ShaderModuleCache::GetOrCreate: reflection normalization failed for '{}'", sourcePath.string());
        return nullptr;
    }
    Entry entry{};
    entry.Bytecode = std::move(output.Data);
    entry.ReflectionPayload = std::move(*reflectionPayload);
    entry.Reflection = std::move(*normalizedReflection);
    entry.Category = output.Category;
    entry.BinaryHash = HashBytes(entry.Bytecode);
    entry.InterfaceHash = HashBytes(
        std::as_bytes(std::span{entry.ReflectionPayload.data(), entry.ReflectionPayload.size()}));
    if (!Materialize(key, entry).HasValue()) {
        return nullptr;
    }
    render::Shader* result = entry.Object.get();
    _entries.emplace(std::move(key), std::move(entry));
    return result;
#endif
}

bool ShaderModuleCache::FlushToDisk() noexcept {
    if (_cacheDirectory.empty()) {
        return true;
    }
    if (_device == nullptr || _entries.size() > kMaxRecordCount) {
        return false;
    }
    try {
        vector<const ShaderModuleKey*> orderedKeys;
        orderedKeys.reserve(_entries.size());
        for (const auto& [key, entry] : _entries) {
            (void)entry;
            orderedKeys.emplace_back(&key);
        }
        std::ranges::sort(orderedKeys, [](const ShaderModuleKey* lhs, const ShaderModuleKey* rhs) {
            const PipelineCacheHash lhsHash = GetPipelineCacheHash(*lhs);
            const PipelineCacheHash rhsHash = GetPipelineCacheHash(*rhs);
            if (lhsHash != rhsHash) {
                return lhsHash < rhsHash;
            }
            BinaryWriter lhsWriter;
            BinaryWriter rhsWriter;
            WriteShaderModuleKey(lhsWriter, *lhs);
            WriteShaderModuleKey(rhsWriter, *rhs);
            return std::lexicographical_compare(
                lhsWriter.Data.begin(), lhsWriter.Data.end(), rhsWriter.Data.begin(), rhsWriter.Data.end());
        });

        BinaryWriter payload;
        payload.WriteU32(static_cast<uint32_t>(orderedKeys.size()));
        for (const ShaderModuleKey* key : orderedKeys) {
            const Entry& entry = _entries.at(*key);
            if (entry.Bytecode.empty() || !entry.Reflection.has_value() || entry.ReflectionPayload.empty()) {
                return false;
            }
            WriteShaderModuleKey(payload, *key);
            payload.WriteI32(static_cast<int32_t>(entry.Category));
            WriteHash(payload, entry.BinaryHash);
            WriteHash(payload, entry.InterfaceHash);
            payload.WriteSizedBytes(entry.Bytecode);
            payload.WriteU8(static_cast<uint8_t>(entry.Reflection->index()));
            payload.WriteString(entry.ReflectionPayload);
        }
        const std::array sections{
            CacheSection{.Kind = CacheSectionKind::ShaderModules, .Data = std::move(payload.Data)}};
        return WriteCacheFile(
            ShaderCachePath(_cacheDirectory, _device->GetBackend()),
            kShaderCacheMagic,
            _device->GetBackend(),
            sections);
    } catch (...) {
        RADRAY_ERR_LOG("ShaderModuleCache::FlushToDisk failed");
        return false;
    }
}

void ShaderModuleCache::Clear() noexcept {
    _shaderKeys.clear();
    for (auto& [key, entry] : _entries) {
        (void)key;
        if (entry.Object != nullptr) {
            entry.Object->Destroy();
        }
    }
    _entries.clear();
}

GraphicsPipelineCache::GraphicsPipelineCache(
    render::Device* device,
    ShaderModuleCache* shaderModuleCache,
    std::filesystem::path cacheDirectory) noexcept
    : _device(device),
      _shaderModuleCache(shaderModuleCache),
      _cacheDirectory(std::move(cacheDirectory)) {
    const bool loaded = LoadFromDisk();
    if (!loaded && _device != nullptr) {
        Clear();
        _device->InitializeNativeGraphicsPipelineCache({});
    }
}

GraphicsPipelineCache::~GraphicsPipelineCache() noexcept {
    FlushToDisk();
    Clear();
}

void GraphicsPipelineCache::RegisterPipelineLayoutKey(const PipelineLayoutCacheKey& key) noexcept {
    const PipelineCacheHash hash = GetPipelineCacheHash(key);
    try {
        const auto [it, inserted] = _pipelineLayoutHashes.try_emplace(hash, key);
        if (!inserted && it->second != key) {
            _pipelineLayoutHashCollisions.emplace(hash);
            RADRAY_WARN_LOG("XXH3-128 collision detected between pipeline layout keys");
        }
    } catch (...) {
        _pipelineLayoutHashCollisions.emplace(hash);
    }
}

void GraphicsPipelineCache::RegisterGraphicsPsoKey(const GraphicsPsoCacheKey& key) noexcept {
    const PipelineCacheHash hash = GetPipelineCacheHash(key);
    try {
        const auto [it, inserted] = _graphicsPsoHashes.try_emplace(hash, key);
        if (!inserted && it->second != key) {
            _graphicsPsoHashCollisions.emplace(hash);
            RADRAY_WARN_LOG("XXH3-128 collision detected between graphics PSO keys; native lookup disabled");
        }
    } catch (...) {
        _graphicsPsoHashCollisions.emplace(hash);
    }
}

bool GraphicsPipelineCache::LoadFromDisk() noexcept {
    if (_device == nullptr) {
        return false;
    }
    if (_cacheDirectory.empty()) {
        return false;
    }
    const std::filesystem::path path = GraphicsCachePath(_cacheDirectory, _device->GetBackend());
    std::optional<vector<CacheSection>> sections =
        ReadCacheFile(path, kGraphicsCacheMagic, _device->GetBackend());
    if (!sections.has_value()) {
        return false;
    }

    try {
        if (const CacheSection* layoutSection = FindSection(*sections, CacheSectionKind::PipelineLayouts)) {
            BinaryReader reader{layoutSection->Data};
            uint32_t count = 0;
            if (!reader.ReadU32(count) || count > kMaxRecordCount) {
                return false;
            }
            for (uint32_t i = 0; i < count; ++i) {
                PipelineLayoutCacheKey key{};
                if (!ReadPipelineLayoutKey(reader, key) ||
                    !_pipelineLayouts.emplace(key, PipelineLayoutEntry{}).second) {
                    RADRAY_WARN_LOG("ignoring malformed pipeline layout metadata in '{}'", path.string());
                    _pipelineLayouts.clear();
                    return false;
                }
                RegisterPipelineLayoutKey(key);
            }
            if (!reader.IsAtEnd()) {
                return false;
            }
        }

        if (const CacheSection* psoSection = FindSection(*sections, CacheSectionKind::GraphicsPsos)) {
            BinaryReader reader{psoSection->Data};
            uint32_t count = 0;
            if (!reader.ReadU32(count) || count > kMaxRecordCount) {
                return false;
            }
            for (uint32_t i = 0; i < count; ++i) {
                GraphicsPsoCacheKey key{};
                if (!ReadGraphicsPsoKey(reader, key) ||
                    !_graphicsPsos.emplace(key, GraphicsPsoEntry{}).second) {
                    RADRAY_WARN_LOG("ignoring malformed graphics PSO metadata in '{}'", path.string());
                    _graphicsPsos.clear();
                    return false;
                }
                RegisterGraphicsPsoKey(key);
            }
            if (!reader.IsAtEnd()) {
                return false;
            }
        }

        std::span<const byte> nativeData{};
        if (const CacheSection* nativeSection = FindSection(*sections, CacheSectionKind::NativePipelineCache)) {
            nativeData = nativeSection->Data;
        }
        if (!_device->InitializeNativeGraphicsPipelineCache(nativeData)) {
            RADRAY_WARN_LOG("native graphics pipeline cache initialization failed; continuing without it");
        }
        return true;
    } catch (...) {
        RADRAY_WARN_LOG("ignoring graphics pipeline cache file '{}' after a decode failure", path.string());
        _pipelineLayouts.clear();
        _graphicsPsos.clear();
        _pipelineLayoutHashes.clear();
        _graphicsPsoHashes.clear();
        return false;
    }
}

std::optional<PipelineLayoutCacheKey> GraphicsPipelineCache::BuildPipelineLayoutKey(
    const render::PipelineLayoutDescriptor& desc) const noexcept {
    if (_shaderModuleCache == nullptr) {
        return std::nullopt;
    }
    try {
        PipelineLayoutCacheKey key{};
        key.Shaders.reserve(desc.Shaders.size());
        for (const render::Shader* shader : desc.Shaders) {
            const auto reverse = _shaderModuleCache->_shaderKeys.find(shader);
            if (shader == nullptr || reverse == _shaderModuleCache->_shaderKeys.end()) {
                RADRAY_ERR_LOG("pipeline layout shader is not owned by ShaderModuleCache");
                return std::nullopt;
            }
            const auto entry = _shaderModuleCache->_entries.find(reverse->second);
            if (entry == _shaderModuleCache->_entries.end()) {
                return std::nullopt;
            }
            key.Shaders.emplace_back(PipelineLayoutCacheKey::ShaderIdentity{
                .Module = reverse->second,
                .InterfaceHash = entry->second.InterfaceHash});
        }
        key.StaticSamplers.assign(desc.StaticSamplers.begin(), desc.StaticSamplers.end());
        key.BindingGroupLayouts.assign(desc.BindingGroupLayouts.begin(), desc.BindingGroupLayouts.end());
        key.BindingGroupLayoutReuses.reserve(desc.BindingGroupLayoutReuses.size());
        for (const render::BindingGroupLayoutReuse& reuse : desc.BindingGroupLayoutReuses) {
            const auto source = _pipelineLayoutKeys.find(reuse.Source);
            if (reuse.Source == nullptr || source == _pipelineLayoutKeys.end() ||
                _pipelineLayoutHashCollisions.contains(source->second)) {
                RADRAY_ERR_LOG("pipeline layout reuse source is not stably owned by this cache");
                return std::nullopt;
            }
            key.BindingGroupLayoutReuses.emplace_back(
                PipelineLayoutCacheKey::BindingGroupLayoutReuseIdentity{
                    .Group = reuse.Group,
                    .SourceLayoutHash = source->second,
                    .SourceGroup = reuse.SourceGroup});
        }
        key.DynamicBufferBindings.assign(
            desc.DynamicBufferBindings.begin(), desc.DynamicBufferBindings.end());
        key.PushConstantBindings.assign(
            desc.PushConstantBindings.begin(), desc.PushConstantBindings.end());
        return key;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<GraphicsPsoCacheKey> GraphicsPipelineCache::BuildGraphicsPsoKey(
    const render::GraphicsPipelineStateDescriptor& desc) const noexcept {
    if (_shaderModuleCache == nullptr || desc.PipelineLayout == nullptr) {
        return std::nullopt;
    }
    const auto layout = _pipelineLayoutKeys.find(desc.PipelineLayout);
    if (layout == _pipelineLayoutKeys.end() || _pipelineLayoutHashCollisions.contains(layout->second)) {
        RADRAY_ERR_LOG("graphics PSO pipeline layout is not stably owned by this cache");
        return std::nullopt;
    }
    try {
        GraphicsPsoCacheKey key{};
        key.PipelineLayoutHash = layout->second;
        const auto buildShader = [this](const render::ShaderEntry& shader)
            -> std::optional<GraphicsPsoCacheKey::ShaderEntryIdentity> {
            const auto reverse = _shaderModuleCache->_shaderKeys.find(shader.Target);
            if (shader.Target == nullptr || reverse == _shaderModuleCache->_shaderKeys.end()) {
                return std::nullopt;
            }
            const auto entry = _shaderModuleCache->_entries.find(reverse->second);
            if (entry == _shaderModuleCache->_entries.end()) {
                return std::nullopt;
            }
            return GraphicsPsoCacheKey::ShaderEntryIdentity{
                .Module = reverse->second,
                .BinaryHash = entry->second.BinaryHash,
                .EntryPoint = string{shader.EntryPoint}};
        };
        if (desc.VS.has_value()) {
            key.VS = buildShader(*desc.VS);
            if (!key.VS.has_value()) {
                RADRAY_ERR_LOG("graphics PSO vertex shader is not owned by ShaderModuleCache");
                return std::nullopt;
            }
        }
        if (desc.PS.has_value()) {
            key.PS = buildShader(*desc.PS);
            if (!key.PS.has_value()) {
                RADRAY_ERR_LOG("graphics PSO pixel shader is not owned by ShaderModuleCache");
                return std::nullopt;
            }
        }
        key.VertexLayouts.reserve(desc.VertexLayouts.size());
        for (const render::VertexBufferLayout& sourceLayout : desc.VertexLayouts) {
            GraphicsPsoCacheKey::VertexBufferLayout& targetLayout = key.VertexLayouts.emplace_back();
            targetLayout.ArrayStride = sourceLayout.ArrayStride;
            targetLayout.StepMode = sourceLayout.StepMode;
            targetLayout.Elements.reserve(sourceLayout.Elements.size());
            for (const render::VertexElement& sourceElement : sourceLayout.Elements) {
                targetLayout.Elements.emplace_back(GraphicsPsoCacheKey::VertexElement{
                    .Offset = sourceElement.Offset,
                    .Semantic = string{sourceElement.Semantic},
                    .SemanticIndex = sourceElement.SemanticIndex,
                    .Format = sourceElement.Format,
                    .Location = sourceElement.Location});
            }
        }
        key.Primitive = desc.Primitive;
        key.DepthStencil = desc.DepthStencil;
        key.MultiSample = desc.MultiSample;
        key.ColorTargets.assign(desc.ColorTargets.begin(), desc.ColorTargets.end());
        return key;
    } catch (...) {
        return std::nullopt;
    }
}

Nullable<render::PipelineLayout*> GraphicsPipelineCache::GetOrCreatePipelineLayout(
    const render::PipelineLayoutDescriptor& desc) noexcept {
    if (_device == nullptr) {
        RADRAY_ERR_LOG("GraphicsPipelineCache::GetOrCreatePipelineLayout: device is null");
        return nullptr;
    }
    const auto ownsUncachedLayout = [this](const render::PipelineLayout* layout) noexcept {
        return std::ranges::any_of(
            _uncachedPipelineLayouts,
            [layout](const unique_ptr<render::PipelineLayout>& candidate) noexcept {
                return candidate.get() == layout;
            });
    };
    bool requiresUncachedLayout = false;
    for (const render::BindingGroupLayoutReuse& reuse : desc.BindingGroupLayoutReuses) {
        const auto source = _pipelineLayoutKeys.find(reuse.Source);
        if (reuse.Source == nullptr ||
            (source == _pipelineLayoutKeys.end() && !ownsUncachedLayout(reuse.Source))) {
            RADRAY_ERR_LOG("pipeline layout reuse source is not owned by this cache");
            return nullptr;
        }
        requiresUncachedLayout = requiresUncachedLayout ||
                                 source == _pipelineLayoutKeys.end() ||
                                 _pipelineLayoutHashCollisions.contains(source->second);
    }
    if (requiresUncachedLayout) {
        if (_shaderModuleCache == nullptr) {
            return nullptr;
        }
        for (const render::Shader* shader : desc.Shaders) {
            if (shader == nullptr || !_shaderModuleCache->_shaderKeys.contains(shader)) {
                RADRAY_ERR_LOG("pipeline layout shader is not owned by ShaderModuleCache");
                return nullptr;
            }
        }
        ++_pipelineLayoutMisses;
        auto layoutOpt = _device->CreatePipelineLayout(desc);
        if (!layoutOpt.HasValue()) {
            RADRAY_ERR_LOG("GraphicsPipelineCache: uncached pipeline layout creation failed");
            return nullptr;
        }
        render::PipelineLayout* result = layoutOpt.Get();
        _uncachedPipelineLayouts.emplace_back(layoutOpt.Release());
        return result;
    }
    std::optional<PipelineLayoutCacheKey> keyOpt = BuildPipelineLayoutKey(desc);
    if (!keyOpt.has_value()) {
        return nullptr;
    }
    PipelineLayoutCacheKey key = std::move(*keyOpt);
    auto cached = _pipelineLayouts.find(key);
    if (cached != _pipelineLayouts.end()) {
        ++_pipelineLayoutHits;
        if (cached->second.Object != nullptr) {
            return cached->second.Object.get();
        }
        auto layoutOpt = _device->CreatePipelineLayout(desc);
        if (!layoutOpt.HasValue()) {
            RADRAY_ERR_LOG("GraphicsPipelineCache: failed to materialize a persisted pipeline layout");
            return nullptr;
        }
        cached->second.Object = layoutOpt.Release();
        render::PipelineLayout* result = cached->second.Object.get();
        _pipelineLayoutKeys.insert_or_assign(result, GetPipelineCacheHash(cached->first));
        return result;
    }

    ++_pipelineLayoutMisses;
    auto layoutOpt = _device->CreatePipelineLayout(desc);
    if (!layoutOpt.HasValue()) {
        RADRAY_ERR_LOG("GraphicsPipelineCache::GetOrCreatePipelineLayout: Device::CreatePipelineLayout failed");
        return nullptr;
    }
    PipelineLayoutEntry entry{.Object = layoutOpt.Release()};
    render::PipelineLayout* result = entry.Object.get();
    const PipelineCacheHash hash = GetPipelineCacheHash(key);
    const auto [inserted, success] = _pipelineLayouts.emplace(std::move(key), std::move(entry));
    if (!success) {
        return inserted->second.Object.get();
    }
    RegisterPipelineLayoutKey(inserted->first);
    _pipelineLayoutKeys.insert_or_assign(result, hash);
    return result;
}

Nullable<render::GraphicsPipelineState*> GraphicsPipelineCache::GetOrCreateGraphicsPso(
    const render::GraphicsPipelineStateDescriptor& desc) noexcept {
    if (_device == nullptr || desc.PipelineLayout == nullptr) {
        RADRAY_ERR_LOG("GraphicsPipelineCache::GetOrCreateGraphicsPso: device or pipeline layout is null");
        return nullptr;
    }
    if (desc.CompatibleRenderPass == nullptr ||
        !render::IsGraphicsPipelineCompatibleWithRenderPass(desc, *desc.CompatibleRenderPass)) {
        RADRAY_ERR_LOG("GraphicsPipelineCache: compatible render pass is null or incompatible");
        return nullptr;
    }
    const auto layout = _pipelineLayoutKeys.find(desc.PipelineLayout);
    const bool ownsUncachedLayout = std::ranges::any_of(
        _uncachedPipelineLayouts,
        [&desc](const unique_ptr<render::PipelineLayout>& candidate) noexcept {
            return candidate.get() == desc.PipelineLayout;
        });
    const bool requiresUncachedPso = ownsUncachedLayout ||
                                     (layout != _pipelineLayoutKeys.end() &&
                                      _pipelineLayoutHashCollisions.contains(layout->second));
    if (requiresUncachedPso) {
        if (_shaderModuleCache == nullptr) {
            return nullptr;
        }
        const auto shaderIsOwned = [this](const std::optional<render::ShaderEntry>& shader) noexcept {
            return !shader.has_value() ||
                   (shader->Target != nullptr && _shaderModuleCache->_shaderKeys.contains(shader->Target));
        };
        if (!shaderIsOwned(desc.VS) || !shaderIsOwned(desc.PS)) {
            RADRAY_ERR_LOG("uncached graphics PSO shader is not owned by ShaderModuleCache");
            return nullptr;
        }
        ++_graphicsPsoMisses;
        render::GraphicsPipelineStateDescriptor nativeDesc = desc;
        nativeDesc.NativeCacheKey = {};
        auto psoOpt = _device->CreateGraphicsPipelineState(nativeDesc);
        if (!psoOpt.HasValue()) {
            RADRAY_ERR_LOG("GraphicsPipelineCache: uncached graphics PSO creation failed");
            return nullptr;
        }
        render::GraphicsPipelineState* result = psoOpt.Get();
        _uncachedGraphicsPsos.emplace_back(psoOpt.Release());
        return result;
    }
    std::optional<GraphicsPsoCacheKey> keyOpt = BuildGraphicsPsoKey(desc);
    if (!keyOpt.has_value()) {
        return nullptr;
    }
    GraphicsPsoCacheKey key = std::move(*keyOpt);
    auto cached = _graphicsPsos.find(key);
    if (cached != _graphicsPsos.end() && cached->second.Object != nullptr) {
        ++_graphicsPsoHits;
        return cached->second.Object.get();
    }

    if (cached != _graphicsPsos.end()) {
        ++_graphicsPsoHits;
    } else {
        ++_graphicsPsoMisses;
        RegisterGraphicsPsoKey(key);
    }
    const PipelineCacheHash hash = GetPipelineCacheHash(key);
    string nativeCacheKey;
    if (!_graphicsPsoHashCollisions.contains(hash)) {
        nativeCacheKey = HashToHex(hash);
    }
    render::GraphicsPipelineStateDescriptor nativeDesc = desc;
    nativeDesc.NativeCacheKey = nativeCacheKey;
    auto psoOpt = _device->CreateGraphicsPipelineState(nativeDesc);
    if (!psoOpt.HasValue()) {
        RADRAY_ERR_LOG("GraphicsPipelineCache::GetOrCreateGraphicsPso: Device creation failed");
        return nullptr;
    }
    if (cached != _graphicsPsos.end()) {
        cached->second.Object = psoOpt.Release();
        return cached->second.Object.get();
    }
    GraphicsPsoEntry entry{.Object = psoOpt.Release()};
    render::GraphicsPipelineState* result = entry.Object.get();
    _graphicsPsos.emplace(std::move(key), std::move(entry));
    return result;
}

bool GraphicsPipelineCache::FlushToDisk() noexcept {
    if (_cacheDirectory.empty()) {
        return true;
    }
    if (_device == nullptr || _pipelineLayouts.size() > kMaxRecordCount ||
        _graphicsPsos.size() > kMaxRecordCount) {
        return false;
    }
    try {
        vector<const PipelineLayoutCacheKey*> layoutKeys;
        layoutKeys.reserve(_pipelineLayouts.size());
        for (const auto& [key, entry] : _pipelineLayouts) {
            (void)entry;
            layoutKeys.emplace_back(&key);
        }
        std::ranges::sort(layoutKeys, [](const auto* lhs, const auto* rhs) {
            return GetPipelineCacheHash(*lhs) < GetPipelineCacheHash(*rhs);
        });
        BinaryWriter layoutPayload;
        layoutPayload.WriteU32(static_cast<uint32_t>(layoutKeys.size()));
        for (const PipelineLayoutCacheKey* key : layoutKeys) {
            WritePipelineLayoutKey(layoutPayload, *key);
        }

        vector<const GraphicsPsoCacheKey*> psoKeys;
        psoKeys.reserve(_graphicsPsos.size());
        for (const auto& [key, entry] : _graphicsPsos) {
            (void)entry;
            psoKeys.emplace_back(&key);
        }
        std::ranges::sort(psoKeys, [](const auto* lhs, const auto* rhs) {
            return GetPipelineCacheHash(*lhs) < GetPipelineCacheHash(*rhs);
        });
        BinaryWriter psoPayload;
        psoPayload.WriteU32(static_cast<uint32_t>(psoKeys.size()));
        for (const GraphicsPsoCacheKey* key : psoKeys) {
            WriteGraphicsPsoKey(psoPayload, *key);
        }

        vector<byte> nativeData;
        std::optional<vector<byte>> nativeDataOpt = _device->SerializeNativeGraphicsPipelineCache();
        if (nativeDataOpt.has_value()) {
            nativeData = std::move(*nativeDataOpt);
        }
        const std::array sections{
            CacheSection{.Kind = CacheSectionKind::PipelineLayouts, .Data = std::move(layoutPayload.Data)},
            CacheSection{.Kind = CacheSectionKind::GraphicsPsos, .Data = std::move(psoPayload.Data)},
            CacheSection{.Kind = CacheSectionKind::NativePipelineCache, .Data = std::move(nativeData)}};
        return WriteCacheFile(
            GraphicsCachePath(_cacheDirectory, _device->GetBackend()),
            kGraphicsCacheMagic,
            _device->GetBackend(),
            sections);
    } catch (...) {
        RADRAY_ERR_LOG("GraphicsPipelineCache::FlushToDisk failed");
        return false;
    }
}

void GraphicsPipelineCache::Clear() noexcept {
    for (unique_ptr<render::GraphicsPipelineState>& pso : _uncachedGraphicsPsos) {
        if (pso != nullptr) {
            pso->Destroy();
        }
    }
    _uncachedGraphicsPsos.clear();
    for (auto& [key, entry] : _graphicsPsos) {
        (void)key;
        if (entry.Object != nullptr) {
            entry.Object->Destroy();
        }
    }
    _graphicsPsos.clear();
    _graphicsPsoHashes.clear();
    _graphicsPsoHashCollisions.clear();

    _pipelineLayoutKeys.clear();
    for (unique_ptr<render::PipelineLayout>& layout : _uncachedPipelineLayouts) {
        if (layout != nullptr) {
            layout->Destroy();
        }
    }
    _uncachedPipelineLayouts.clear();
    for (auto& [key, entry] : _pipelineLayouts) {
        (void)key;
        if (entry.Object != nullptr) {
            entry.Object->Destroy();
        }
    }
    _pipelineLayouts.clear();
    _pipelineLayoutHashes.clear();
    _pipelineLayoutHashCollisions.clear();
}

}  // namespace radray

namespace radray {

namespace {

template <typename T>
void WriteEnum(BinaryWriter& writer, T value) {
    static_assert(std::is_enum_v<T>);
    writer.WriteU32(static_cast<uint32_t>(value));
}

template <typename T>
bool ReadEnum(BinaryReader& reader, T& value) noexcept {
    static_assert(std::is_enum_v<T>);
    uint32_t raw = 0;
    if (!reader.ReadU32(raw)) {
        return false;
    }
    value = static_cast<T>(raw);
    return true;
}

template <typename T, typename WriteElement>
void WriteVector(BinaryWriter& writer, const vector<T>& values, WriteElement&& writeElement) {
    if (values.size() > kMaxRecordCount) {
        throw std::length_error{"pipeline cache array is too large"};
    }
    writer.WriteU32(static_cast<uint32_t>(values.size()));
    for (const T& value : values) {
        writeElement(writer, value);
    }
}

template <typename T, typename ReadElement>
bool ReadVector(BinaryReader& reader, vector<T>& values, ReadElement&& readElement) {
    uint32_t count = 0;
    if (!reader.ReadU32(count) || count > kMaxRecordCount) {
        return false;
    }
    values.clear();
    values.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        T value{};
        if (!readElement(reader, value)) {
            return false;
        }
        values.emplace_back(std::move(value));
    }
    return true;
}

void WriteHash(BinaryWriter& writer, const PipelineCacheHash& value) {
    writer.WriteU64(value.Low);
    writer.WriteU64(value.High);
}

bool ReadHash(BinaryReader& reader, PipelineCacheHash& value) noexcept {
    return reader.ReadU64(value.Low) && reader.ReadU64(value.High);
}

PipelineCacheHash HashBytes(std::span<const byte> data) noexcept {
    const XXH128_hash_t hash = XXH3_128bits(data.empty() ? nullptr : data.data(), data.size());
    return PipelineCacheHash{.Low = hash.low64, .High = hash.high64};
}

size_t FoldHash(PipelineCacheHash value) noexcept {
    const uint64_t folded = value.Low ^ value.High;
    if constexpr (sizeof(size_t) == sizeof(uint64_t)) {
        return static_cast<size_t>(folded);
    } else {
        return static_cast<size_t>(folded ^ (folded >> 32));
    }
}

string HashToHex(PipelineCacheHash value) {
    return fmt::format("{:016x}{:016x}", value.High, value.Low);
}

void WriteGuid(BinaryWriter& writer, const Guid& value) {
    for (uint8_t part : value.Bytes()) {
        writer.WriteU8(part);
    }
}

bool ReadGuid(BinaryReader& reader, Guid& value) noexcept {
    Guid::ByteArray bytes{};
    for (uint8_t& part : bytes) {
        if (!reader.ReadU8(part)) {
            return false;
        }
    }
    value = Guid{bytes};
    return true;
}

void WriteShaderModuleKey(BinaryWriter& writer, const ShaderModuleKey& key) {
    WriteGuid(writer, key.Shader);
    WriteVector(writer, key.Defines, [](BinaryWriter& target, const string& value) {
        target.WriteString(value);
    });
    writer.WriteU32(key.PassIndex);
    WriteEnum(writer, key.Stage);
}

bool ReadShaderModuleKey(BinaryReader& reader, ShaderModuleKey& key) {
    return ReadGuid(reader, key.Shader) &&
           ReadVector(reader, key.Defines, [](BinaryReader& source, string& value) {
               return source.ReadString(value);
           }) &&
           reader.ReadU32(key.PassIndex) && ReadEnum(reader, key.Stage);
}

void WriteSamplerDescriptor(BinaryWriter& writer, const render::SamplerDescriptor& value) {
    WriteEnum(writer, value.AddressS);
    WriteEnum(writer, value.AddressT);
    WriteEnum(writer, value.AddressR);
    WriteEnum(writer, value.MinFilter);
    WriteEnum(writer, value.MagFilter);
    WriteEnum(writer, value.MipmapFilter);
    writer.WriteFloat(value.LodMin);
    writer.WriteFloat(value.LodMax);
    writer.WriteBool(value.Compare.has_value());
    if (value.Compare.has_value()) {
        WriteEnum(writer, *value.Compare);
    }
    writer.WriteU32(value.AnisotropyClamp);
}

bool ReadSamplerDescriptor(BinaryReader& reader, render::SamplerDescriptor& value) {
    bool hasCompare = false;
    if (!ReadEnum(reader, value.AddressS) || !ReadEnum(reader, value.AddressT) ||
        !ReadEnum(reader, value.AddressR) || !ReadEnum(reader, value.MinFilter) ||
        !ReadEnum(reader, value.MagFilter) || !ReadEnum(reader, value.MipmapFilter) ||
        !reader.ReadFloat(value.LodMin) || !reader.ReadFloat(value.LodMax) ||
        !reader.ReadBool(hasCompare)) {
        return false;
    }
    if (hasCompare) {
        render::CompareFunction compare{};
        if (!ReadEnum(reader, compare)) {
            return false;
        }
        value.Compare = compare;
    } else {
        value.Compare.reset();
    }
    return reader.ReadU32(value.AnisotropyClamp);
}

void WriteShaderParameterInfo(BinaryWriter& writer, const render::ShaderParameterInfo& value) {
    writer.WriteString(value.Name);
    WriteEnum(writer, value.Kind);
    writer.WriteU32(value.Stages.value());
    WriteEnum(writer, value.Type);
    writer.WriteU32(value.Count);
    writer.WriteU32(value.ByteSize);
    writer.WriteBool(value.IsReadOnly);
    writer.WriteBool(value.IsBindless);
}

bool ReadShaderParameterInfo(BinaryReader& reader, render::ShaderParameterInfo& value) {
    uint32_t stages = 0;
    if (!reader.ReadString(value.Name) || !ReadEnum(reader, value.Kind) ||
        !reader.ReadU32(stages) || !ReadEnum(reader, value.Type) ||
        !reader.ReadU32(value.Count) || !reader.ReadU32(value.ByteSize) ||
        !reader.ReadBool(value.IsReadOnly) || !reader.ReadBool(value.IsBindless)) {
        return false;
    }
    value.Stages = render::ShaderStages{stages};
    return true;
}

void WriteBindingGroupLayout(BinaryWriter& writer, const render::BindingGroupLayout& value) {
    writer.WriteU32(value.GroupIndex);
    WriteVector(writer, value.Entries, [](BinaryWriter& target, const render::BindingGroupLayoutEntry& entry) {
        WriteShaderParameterInfo(target, entry.Parameter);
        target.WriteU32(entry.Binding);
        target.WriteBool(entry.HasDynamicOffset);
        target.WriteBool(entry.IsStaticSampler);
    });
}

bool ReadBindingGroupLayout(BinaryReader& reader, render::BindingGroupLayout& value) {
    return reader.ReadU32(value.GroupIndex) &&
           ReadVector(reader, value.Entries, [](BinaryReader& source, render::BindingGroupLayoutEntry& entry) {
               return ReadShaderParameterInfo(source, entry.Parameter) &&
                      source.ReadU32(entry.Binding) &&
                      source.ReadBool(entry.HasDynamicOffset) &&
                      source.ReadBool(entry.IsStaticSampler);
           });
}

void WritePipelineLayoutKey(BinaryWriter& writer, const PipelineLayoutCacheKey& key) {
    WriteVector(writer, key.Shaders, [](BinaryWriter& target, const PipelineLayoutCacheKey::ShaderIdentity& value) {
        WriteShaderModuleKey(target, value.Module);
        WriteHash(target, value.InterfaceHash);
    });
    WriteVector(writer, key.StaticSamplers, [](BinaryWriter& target, const render::StaticSamplerDescriptor& value) {
        target.WriteString(value.Name);
        WriteSamplerDescriptor(target, value.Desc);
    });
    WriteVector(writer, key.BindingGroupLayouts, [](BinaryWriter& target, const render::BindingGroupLayout& value) {
        WriteBindingGroupLayout(target, value);
    });
    WriteVector(
        writer,
        key.BindingGroupLayoutReuses,
        [](BinaryWriter& target, const PipelineLayoutCacheKey::BindingGroupLayoutReuseIdentity& value) {
            target.WriteU32(value.Group);
            WriteHash(target, value.SourceLayoutHash);
            target.WriteU32(value.SourceGroup);
        });
    WriteVector(writer, key.DynamicBufferBindings, [](BinaryWriter& target, const render::DynamicBufferBinding& value) {
        target.WriteU32(value.Group);
        target.WriteU32(value.Binding);
    });
    WriteVector(writer, key.PushConstantBindings, [](BinaryWriter& target, const render::PushConstantBinding& value) {
        target.WriteU32(value.Group);
        target.WriteU32(value.Binding);
    });
}

bool ReadPipelineLayoutKey(BinaryReader& reader, PipelineLayoutCacheKey& key) {
    return ReadVector(reader, key.Shaders, [](BinaryReader& source, PipelineLayoutCacheKey::ShaderIdentity& value) {
               return ReadShaderModuleKey(source, value.Module) && ReadHash(source, value.InterfaceHash);
           }) &&
           ReadVector(reader, key.StaticSamplers, [](BinaryReader& source, render::StaticSamplerDescriptor& value) {
               return source.ReadString(value.Name) && ReadSamplerDescriptor(source, value.Desc);
           }) &&
           ReadVector(reader, key.BindingGroupLayouts, [](BinaryReader& source, render::BindingGroupLayout& value) {
               return ReadBindingGroupLayout(source, value);
           }) &&
           ReadVector(
               reader,
               key.BindingGroupLayoutReuses,
               [](BinaryReader& source, PipelineLayoutCacheKey::BindingGroupLayoutReuseIdentity& value) {
                   return source.ReadU32(value.Group) && ReadHash(source, value.SourceLayoutHash) &&
                          source.ReadU32(value.SourceGroup);
               }) &&
           ReadVector(reader, key.DynamicBufferBindings, [](BinaryReader& source, render::DynamicBufferBinding& value) {
               return source.ReadU32(value.Group) && source.ReadU32(value.Binding);
           }) &&
           ReadVector(reader, key.PushConstantBindings, [](BinaryReader& source, render::PushConstantBinding& value) {
               return source.ReadU32(value.Group) && source.ReadU32(value.Binding);
           });
}

void WritePrimitiveState(BinaryWriter& writer, const render::PrimitiveState& value) {
    WriteEnum(writer, value.Topology);
    WriteEnum(writer, value.FaceClockwise);
    WriteEnum(writer, value.Cull);
    WriteEnum(writer, value.Poly);
    writer.WriteBool(value.StripIndexFormat.has_value());
    if (value.StripIndexFormat.has_value()) {
        WriteEnum(writer, *value.StripIndexFormat);
    }
    writer.WriteBool(value.UnclippedDepth);
    writer.WriteBool(value.Conservative);
}

bool ReadPrimitiveState(BinaryReader& reader, render::PrimitiveState& value) {
    bool hasStripIndex = false;
    if (!ReadEnum(reader, value.Topology) || !ReadEnum(reader, value.FaceClockwise) ||
        !ReadEnum(reader, value.Cull) || !ReadEnum(reader, value.Poly) ||
        !reader.ReadBool(hasStripIndex)) {
        return false;
    }
    if (hasStripIndex) {
        render::IndexFormat format{};
        if (!ReadEnum(reader, format)) {
            return false;
        }
        value.StripIndexFormat = format;
    } else {
        value.StripIndexFormat.reset();
    }
    return reader.ReadBool(value.UnclippedDepth) && reader.ReadBool(value.Conservative);
}

void WriteStencilFaceState(BinaryWriter& writer, const render::StencilFaceState& value) {
    WriteEnum(writer, value.Compare);
    WriteEnum(writer, value.FailOp);
    WriteEnum(writer, value.DepthFailOp);
    WriteEnum(writer, value.PassOp);
}

bool ReadStencilFaceState(BinaryReader& reader, render::StencilFaceState& value) noexcept {
    return ReadEnum(reader, value.Compare) && ReadEnum(reader, value.FailOp) &&
           ReadEnum(reader, value.DepthFailOp) && ReadEnum(reader, value.PassOp);
}

void WriteDepthStencilState(BinaryWriter& writer, const render::DepthStencilState& value) {
    WriteEnum(writer, value.Format);
    WriteEnum(writer, value.DepthCompare);
    writer.WriteI32(value.DepthBias.Constant);
    writer.WriteFloat(value.DepthBias.SlopScale);
    writer.WriteFloat(value.DepthBias.Clamp);
    writer.WriteBool(value.Stencil.has_value());
    if (value.Stencil.has_value()) {
        WriteStencilFaceState(writer, value.Stencil->Front);
        WriteStencilFaceState(writer, value.Stencil->Back);
        writer.WriteU32(value.Stencil->ReadMask);
        writer.WriteU32(value.Stencil->WriteMask);
    }
    writer.WriteBool(value.DepthTestEnable);
    writer.WriteBool(value.DepthWriteEnable);
}

bool ReadDepthStencilState(BinaryReader& reader, render::DepthStencilState& value) {
    bool hasStencil = false;
    if (!ReadEnum(reader, value.Format) || !ReadEnum(reader, value.DepthCompare) ||
        !reader.ReadI32(value.DepthBias.Constant) || !reader.ReadFloat(value.DepthBias.SlopScale) ||
        !reader.ReadFloat(value.DepthBias.Clamp) || !reader.ReadBool(hasStencil)) {
        return false;
    }
    if (hasStencil) {
        render::StencilState stencil{};
        if (!ReadStencilFaceState(reader, stencil.Front) || !ReadStencilFaceState(reader, stencil.Back) ||
            !reader.ReadU32(stencil.ReadMask) || !reader.ReadU32(stencil.WriteMask)) {
            return false;
        }
        value.Stencil = stencil;
    } else {
        value.Stencil.reset();
    }
    return reader.ReadBool(value.DepthTestEnable) && reader.ReadBool(value.DepthWriteEnable);
}

void WriteBlendComponent(BinaryWriter& writer, const render::BlendComponent& value) {
    WriteEnum(writer, value.Src);
    WriteEnum(writer, value.Dst);
    WriteEnum(writer, value.Op);
}

bool ReadBlendComponent(BinaryReader& reader, render::BlendComponent& value) noexcept {
    return ReadEnum(reader, value.Src) && ReadEnum(reader, value.Dst) && ReadEnum(reader, value.Op);
}

void WriteColorTargetState(BinaryWriter& writer, const render::ColorTargetState& value) {
    WriteEnum(writer, value.Format);
    writer.WriteBool(value.Blend.has_value());
    if (value.Blend.has_value()) {
        WriteBlendComponent(writer, value.Blend->Color);
        WriteBlendComponent(writer, value.Blend->Alpha);
    }
    writer.WriteU32(value.WriteMask.value());
}

bool ReadColorTargetState(BinaryReader& reader, render::ColorTargetState& value) {
    bool hasBlend = false;
    if (!ReadEnum(reader, value.Format) || !reader.ReadBool(hasBlend)) {
        return false;
    }
    if (hasBlend) {
        render::BlendState blend{};
        if (!ReadBlendComponent(reader, blend.Color) || !ReadBlendComponent(reader, blend.Alpha)) {
            return false;
        }
        value.Blend = blend;
    } else {
        value.Blend.reset();
    }
    uint32_t writeMask = 0;
    if (!reader.ReadU32(writeMask)) {
        return false;
    }
    value.WriteMask = render::ColorWrites{writeMask};
    return true;
}

void WriteGraphicsPsoKey(BinaryWriter& writer, const GraphicsPsoCacheKey& key) {
    WriteHash(writer, key.PipelineLayoutHash);
    const auto writeShader = [](BinaryWriter& target, const std::optional<GraphicsPsoCacheKey::ShaderEntryIdentity>& value) {
        target.WriteBool(value.has_value());
        if (value.has_value()) {
            WriteShaderModuleKey(target, value->Module);
            WriteHash(target, value->BinaryHash);
            target.WriteString(value->EntryPoint);
        }
    };
    writeShader(writer, key.VS);
    writeShader(writer, key.PS);
    WriteVector(writer, key.VertexLayouts, [](BinaryWriter& target, const GraphicsPsoCacheKey::VertexBufferLayout& value) {
        target.WriteU64(value.ArrayStride);
        WriteEnum(target, value.StepMode);
        WriteVector(target, value.Elements, [](BinaryWriter& elementTarget, const GraphicsPsoCacheKey::VertexElement& element) {
            elementTarget.WriteU64(element.Offset);
            elementTarget.WriteString(element.Semantic);
            elementTarget.WriteU32(element.SemanticIndex);
            WriteEnum(elementTarget, element.Format);
            elementTarget.WriteU32(element.Location);
        });
    });
    WritePrimitiveState(writer, key.Primitive);
    writer.WriteBool(key.DepthStencil.has_value());
    if (key.DepthStencil.has_value()) {
        WriteDepthStencilState(writer, *key.DepthStencil);
    }
    writer.WriteU32(key.MultiSample.Count);
    writer.WriteU64(key.MultiSample.Mask);
    writer.WriteBool(key.MultiSample.AlphaToCoverageEnable);
    WriteVector(writer, key.ColorTargets, [](BinaryWriter& target, const render::ColorTargetState& value) {
        WriteColorTargetState(target, value);
    });
}

bool ReadGraphicsPsoKey(BinaryReader& reader, GraphicsPsoCacheKey& key) {
    const auto readShader = [](BinaryReader& source, std::optional<GraphicsPsoCacheKey::ShaderEntryIdentity>& value) {
        bool hasValue = false;
        if (!source.ReadBool(hasValue)) {
            return false;
        }
        if (!hasValue) {
            value.reset();
            return true;
        }
        GraphicsPsoCacheKey::ShaderEntryIdentity result{};
        if (!ReadShaderModuleKey(source, result.Module) || !ReadHash(source, result.BinaryHash) ||
            !source.ReadString(result.EntryPoint)) {
            return false;
        }
        value = std::move(result);
        return true;
    };

    if (!ReadHash(reader, key.PipelineLayoutHash) || !readShader(reader, key.VS) || !readShader(reader, key.PS) ||
        !ReadVector(reader, key.VertexLayouts, [](BinaryReader& source, GraphicsPsoCacheKey::VertexBufferLayout& value) {
            return source.ReadU64(value.ArrayStride) && ReadEnum(source, value.StepMode) &&
                   ReadVector(source, value.Elements, [](BinaryReader& elementSource, GraphicsPsoCacheKey::VertexElement& element) {
                       return elementSource.ReadU64(element.Offset) && elementSource.ReadString(element.Semantic) &&
                              elementSource.ReadU32(element.SemanticIndex) && ReadEnum(elementSource, element.Format) &&
                              elementSource.ReadU32(element.Location);
                   });
        }) ||
        !ReadPrimitiveState(reader, key.Primitive)) {
        return false;
    }

    bool hasDepthStencil = false;
    if (!reader.ReadBool(hasDepthStencil)) {
        return false;
    }
    if (hasDepthStencil) {
        render::DepthStencilState value{};
        if (!ReadDepthStencilState(reader, value)) {
            return false;
        }
        key.DepthStencil = value;
    } else {
        key.DepthStencil.reset();
    }
    return reader.ReadU32(key.MultiSample.Count) && reader.ReadU64(key.MultiSample.Mask) &&
           reader.ReadBool(key.MultiSample.AlphaToCoverageEnable) &&
           ReadVector(reader, key.ColorTargets, [](BinaryReader& source, render::ColorTargetState& value) {
               return ReadColorTargetState(source, value);
           });
}

template <typename T, void (*WriteValue)(BinaryWriter&, const T&)>
PipelineCacheHash HashCanonicalValue(const T& value) noexcept {
    try {
        BinaryWriter writer;
        writer.Data.reserve(512);
        WriteValue(writer, value);
        return HashBytes(writer.Data);
    } catch (...) {
        return {};
    }
}

bool WriteCacheFile(
    const std::filesystem::path& path,
    const std::array<uint8_t, 8>& magic,
    render::RenderBackend backend,
    std::span<const CacheSection> sections) noexcept {
    try {
        uint64_t fileSize = 8 + 4 + 4 + 4;
        if (sections.size() > kMaxSectionCount) {
            return false;
        }
        for (const CacheSection& section : sections) {
            constexpr uint64_t sectionHeaderSize = 4 + 8 + 16;
            if (section.Data.size() > kMaxBlobSize ||
                sectionHeaderSize > kMaxCacheFileSize - fileSize ||
                section.Data.size() > kMaxCacheFileSize - fileSize - sectionHeaderSize) {
                return false;
            }
            fileSize += sectionHeaderSize + section.Data.size();
        }
        BinaryWriter writer;
        writer.WriteBytes(std::as_bytes(std::span{magic}));
        writer.WriteU32(kCacheFormatVersion);
        writer.WriteI32(static_cast<int32_t>(backend));
        writer.WriteU32(static_cast<uint32_t>(sections.size()));
        for (const CacheSection& section : sections) {
            writer.WriteU32(static_cast<uint32_t>(section.Kind));
            writer.WriteU64(static_cast<uint64_t>(section.Data.size()));
            WriteHash(writer, HashBytes(section.Data));
            writer.WriteBytes(section.Data);
        }

        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) {
            RADRAY_ERR_LOG("cannot create render cache directory '{}': {}", path.parent_path().string(), error.message());
            return false;
        }

        std::filesystem::path temporaryPath = path;
        temporaryPath += ".tmp";
        if (!WriteBinaryFile(temporaryPath, writer.Data)) {
            return false;
        }
        std::filesystem::rename(temporaryPath, path, error);
        if (!error) {
            return true;
        }

        error.clear();
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(temporaryPath, path, error);
        if (error) {
            RADRAY_ERR_LOG("cannot replace render cache file '{}': {}", path.string(), error.message());
            std::error_code ignored;
            std::filesystem::remove(temporaryPath, ignored);
            return false;
        }
        return true;
    } catch (...) {
        RADRAY_ERR_LOG("failed to serialize render cache file '{}'", path.string());
        return false;
    }
}

std::optional<vector<CacheSection>> ReadCacheFile(
    const std::filesystem::path& path,
    const std::array<uint8_t, 8>& magic,
    render::RenderBackend backend) noexcept {
    try {
        std::error_code fileSizeError;
        const uintmax_t fileSize = std::filesystem::file_size(path, fileSizeError);
        if (fileSizeError) {
            return std::nullopt;
        }
        if (fileSize > kMaxCacheFileSize) {
            RADRAY_WARN_LOG("ignoring oversized render cache file '{}'", path.string());
            return std::nullopt;
        }
        std::optional<vector<byte>> file = ReadBinaryFile(path);
        if (!file.has_value()) {
            return std::nullopt;
        }
        BinaryReader reader{*file};
        std::span<const byte> fileMagic;
        uint32_t version = 0;
        int32_t fileBackend = 0;
        uint32_t sectionCount = 0;
        if (!reader.ReadBytes(magic.size(), fileMagic) ||
            std::memcmp(fileMagic.data(), magic.data(), magic.size()) != 0 ||
            !reader.ReadU32(version) || version != kCacheFormatVersion ||
            !reader.ReadI32(fileBackend) || fileBackend != static_cast<int32_t>(backend) ||
            !reader.ReadU32(sectionCount) || sectionCount > kMaxSectionCount) {
            RADRAY_WARN_LOG("ignoring incompatible render cache file '{}'", path.string());
            return std::nullopt;
        }

        vector<CacheSection> sections;
        sections.reserve(sectionCount);
        unordered_set<uint32_t> kinds;
        for (uint32_t i = 0; i < sectionCount; ++i) {
            uint32_t kind = 0;
            uint64_t size = 0;
            PipelineCacheHash expectedHash{};
            if (!reader.ReadU32(kind) || !reader.ReadU64(size) || size > kMaxBlobSize ||
                !ReadHash(reader, expectedHash) || size > reader.Remaining() || !kinds.emplace(kind).second) {
                RADRAY_WARN_LOG("ignoring malformed render cache file '{}'", path.string());
                return std::nullopt;
            }
            std::span<const byte> data;
            if (!reader.ReadBytes(static_cast<size_t>(size), data) || HashBytes(data) != expectedHash) {
                RADRAY_WARN_LOG("ignoring render cache file '{}' with a corrupt section", path.string());
                return std::nullopt;
            }
            sections.emplace_back(CacheSection{
                .Kind = static_cast<CacheSectionKind>(kind),
                .Data = vector<byte>{data.begin(), data.end()}});
        }
        if (!reader.IsAtEnd()) {
            RADRAY_WARN_LOG("ignoring render cache file '{}' with trailing data", path.string());
            return std::nullopt;
        }
        return sections;
    } catch (...) {
        RADRAY_WARN_LOG("ignoring unreadable render cache file '{}'", path.string());
        return std::nullopt;
    }
}

const CacheSection* FindSection(std::span<const CacheSection> sections, CacheSectionKind kind) noexcept {
    const auto it = std::ranges::find_if(sections, [kind](const CacheSection& section) noexcept {
        return section.Kind == kind;
    });
    return it == sections.end() ? nullptr : &*it;
}

string BackendFileSuffix(render::RenderBackend backend) {
    switch (backend) {
        case render::RenderBackend::D3D12: return "d3d12";
        case render::RenderBackend::Vulkan: return "vulkan";
        default: return "unknown";
    }
}

std::filesystem::path ShaderCachePath(
    const std::filesystem::path& directory,
    render::RenderBackend backend) {
    return directory / fmt::format("shader_modules.{}.bin", BackendFileSuffix(backend));
}

std::filesystem::path GraphicsCachePath(
    const std::filesystem::path& directory,
    render::RenderBackend backend) {
    return directory / fmt::format("graphics_pipelines.{}.bin", BackendFileSuffix(backend));
}

void NormalizeShaderModuleKey(ShaderModuleKey& key) {
    std::erase_if(key.Defines, [](const string& define) noexcept { return define.empty(); });
    std::ranges::sort(key.Defines);
    key.Defines.erase(std::unique(key.Defines.begin(), key.Defines.end()), key.Defines.end());
}

std::optional<std::string_view> FindShaderEntryPoint(
    const ShaderPassDesc& pass,
    render::ShaderStage stage) noexcept {
    if (const auto* graphics = std::get_if<ShaderGraphicsPassDesc>(&pass.Program)) {
        if (stage == render::ShaderStage::Vertex) {
            return graphics->VertexEntry;
        }
        if (stage == render::ShaderStage::Pixel && graphics->PixelEntry.has_value()) {
            return *graphics->PixelEntry;
        }
        return std::nullopt;
    }
    const auto* compute = std::get_if<ShaderComputePassDesc>(&pass.Program);
    if (compute != nullptr && stage == render::ShaderStage::Compute) {
        return compute->EntryPoint;
    }
    return std::nullopt;
}

bool AreShaderDefinesValid(
    const ShaderPassDesc& pass,
    render::ShaderStage stage,
    const vector<string>& defines) noexcept {
    for (const string& define : defines) {
        const bool declaredForStage = std::ranges::any_of(
            pass.KeywordGroups,
            [stage, &define](const ShaderKeywordGroupDesc& group) noexcept {
                const bool affectsStage = group.Stages == render::ShaderStage::UNKNOWN ||
                                          group.Stages.HasFlag(stage);
                return affectsStage && std::ranges::find(group.Alternatives, define) != group.Alternatives.end();
            });
        if (!declaredForStage) {
            return false;
        }
    }
    for (const ShaderKeywordGroupDesc& group : pass.KeywordGroups) {
        if (group.Stages != render::ShaderStage::UNKNOWN && !group.Stages.HasFlag(stage)) {
            continue;
        }
        const size_t selectedCount = std::ranges::count_if(
            group.Alternatives,
            [&defines](const string& alternative) noexcept {
                return !alternative.empty() &&
                       std::ranges::find(defines, alternative) != defines.end();
            });
        if (selectedCount > 1) {
            return false;
        }
    }
    return true;
}

std::optional<string> SerializeReflection(const render::ShaderReflectionDesc& reflection) noexcept {
    if (const auto* hlsl = std::get_if<render::HlslShaderDesc>(&reflection)) {
        return render::SerializeHlslShaderDesc(*hlsl);
    }
    return render::SerializeSpirvShaderDesc(std::get<render::SpirvShaderDesc>(reflection));
}

std::optional<render::ShaderReflectionDesc> DeserializeReflection(
    uint8_t kind,
    std::string_view payload) noexcept {
    if (kind == 0) {
        std::optional<render::HlslShaderDesc> value = render::DeserializeHlslShaderDesc(payload);
        if (value.has_value()) {
            return render::ShaderReflectionDesc{std::move(*value)};
        }
    } else if (kind == 1) {
        std::optional<render::SpirvShaderDesc> value = render::DeserializeSpirvShaderDesc(payload);
        if (value.has_value()) {
            return render::ShaderReflectionDesc{std::move(*value)};
        }
    }
    return std::nullopt;
}

}  // namespace

PipelineCacheHash GetPipelineCacheHash(const ShaderModuleKey& key) noexcept {
    return HashCanonicalValue<ShaderModuleKey, WriteShaderModuleKey>(key);
}

PipelineCacheHash GetPipelineCacheHash(const PipelineLayoutCacheKey& key) noexcept {
    return HashCanonicalValue<PipelineLayoutCacheKey, WritePipelineLayoutKey>(key);
}

PipelineCacheHash GetPipelineCacheHash(const GraphicsPsoCacheKey& key) noexcept {
    return HashCanonicalValue<GraphicsPsoCacheKey, WriteGraphicsPsoKey>(key);
}

namespace {

bool EqualFloatBits(float lhs, float rhs) noexcept {
    return std::bit_cast<uint32_t>(lhs) == std::bit_cast<uint32_t>(rhs);
}

bool EqualSamplerDescriptor(
    const render::SamplerDescriptor& lhs,
    const render::SamplerDescriptor& rhs) noexcept {
    return lhs.AddressS == rhs.AddressS && lhs.AddressT == rhs.AddressT &&
           lhs.AddressR == rhs.AddressR && lhs.MinFilter == rhs.MinFilter &&
           lhs.MagFilter == rhs.MagFilter && lhs.MipmapFilter == rhs.MipmapFilter &&
           EqualFloatBits(lhs.LodMin, rhs.LodMin) && EqualFloatBits(lhs.LodMax, rhs.LodMax) &&
           lhs.Compare == rhs.Compare && lhs.AnisotropyClamp == rhs.AnisotropyClamp;
}

bool EqualDepthStencilState(
    const render::DepthStencilState& lhs,
    const render::DepthStencilState& rhs) noexcept {
    return lhs.Format == rhs.Format && lhs.DepthCompare == rhs.DepthCompare &&
           lhs.DepthBias.Constant == rhs.DepthBias.Constant &&
           EqualFloatBits(lhs.DepthBias.SlopScale, rhs.DepthBias.SlopScale) &&
           EqualFloatBits(lhs.DepthBias.Clamp, rhs.DepthBias.Clamp) &&
           lhs.Stencil == rhs.Stencil && lhs.DepthTestEnable == rhs.DepthTestEnable &&
           lhs.DepthWriteEnable == rhs.DepthWriteEnable;
}

}  // namespace

bool operator==(
    const PipelineLayoutCacheKey& lhs,
    const PipelineLayoutCacheKey& rhs) noexcept {
    return lhs.Shaders == rhs.Shaders &&
           std::ranges::equal(
               lhs.StaticSamplers,
               rhs.StaticSamplers,
               [](const render::StaticSamplerDescriptor& left,
                  const render::StaticSamplerDescriptor& right) noexcept {
                   return left.Name == right.Name && EqualSamplerDescriptor(left.Desc, right.Desc);
               }) &&
           lhs.BindingGroupLayouts == rhs.BindingGroupLayouts &&
           lhs.BindingGroupLayoutReuses == rhs.BindingGroupLayoutReuses &&
           lhs.DynamicBufferBindings == rhs.DynamicBufferBindings &&
           lhs.PushConstantBindings == rhs.PushConstantBindings;
}

bool operator==(
    const GraphicsPsoCacheKey& lhs,
    const GraphicsPsoCacheKey& rhs) noexcept {
    const bool depthStencilEqual = lhs.DepthStencil.has_value() == rhs.DepthStencil.has_value() &&
                                   (!lhs.DepthStencil.has_value() ||
                                    EqualDepthStencilState(*lhs.DepthStencil, *rhs.DepthStencil));
    return lhs.PipelineLayoutHash == rhs.PipelineLayoutHash && lhs.VS == rhs.VS &&
           lhs.PS == rhs.PS && lhs.VertexLayouts == rhs.VertexLayouts &&
           lhs.Primitive == rhs.Primitive && depthStencilEqual &&
           lhs.MultiSample == rhs.MultiSample && lhs.ColorTargets == rhs.ColorTargets;
}

}  // namespace radray

size_t std::hash<radray::PipelineCacheHash>::operator()(
    const radray::PipelineCacheHash& value) const noexcept {
    return radray::FoldHash(value);
}

size_t std::hash<radray::ShaderModuleKey>::operator()(
    const radray::ShaderModuleKey& key) const noexcept {
    return radray::FoldHash(radray::GetPipelineCacheHash(key));
}

size_t std::hash<radray::PipelineLayoutCacheKey>::operator()(
    const radray::PipelineLayoutCacheKey& key) const noexcept {
    return radray::FoldHash(radray::GetPipelineCacheHash(key));
}

size_t std::hash<radray::GraphicsPsoCacheKey>::operator()(
    const radray::GraphicsPsoCacheKey& key) const noexcept {
    return radray::FoldHash(radray::GetPipelineCacheHash(key));
}
