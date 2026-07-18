#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string_view>

#include <radray/nullable.h>
#include <radray/shader/common.h>
#include <radray/shader/hlsl.h>
#include <radray/types.h>

namespace radray::shader {

class Dxc;

struct DxcOutput {
    vector<byte> Data;
    vector<byte> Refl;
    ShaderBlobCategory Category{ShaderBlobCategory::DXIL};
};

struct DxcCompileOptions {
    std::string_view EntryPoint{};
    ShaderStage Stage{ShaderStage::UNKNOWN};
    HlslShaderModel SM{HlslShaderModel::SM60};
    std::span<const std::string_view> Defines{};
    std::span<const std::string_view> Includes{};
    bool IsOptimize{false};
    bool IsSpirv{false};
    bool EnableUnbounded{false};
};

#if defined(RADRAY_ENABLE_DXC)

class Dxc : public enable_shared_from_this<Dxc> {
public:
    Dxc(const Dxc&) = delete;
    Dxc(Dxc&&) = delete;
    Dxc& operator=(const Dxc&) = delete;
    Dxc& operator=(Dxc&&) = delete;
    ~Dxc() noexcept;

    bool IsValid() const noexcept { return _impl != nullptr; }
    void Destroy() noexcept;

    std::optional<DxcOutput> CompileMemory(
        std::string_view code,
        std::string_view sourceName,
        const DxcCompileOptions& options) noexcept;

    std::optional<DxcOutput> CompileFile(
        const std::filesystem::path& path,
        const DxcCompileOptions& options) noexcept;

    std::optional<HlslShaderDesc> GetShaderDescFromOutput(std::span<const byte> refl) noexcept;

private:
    class Impl;

    explicit Dxc(unique_ptr<Impl> impl) noexcept;

    unique_ptr<Impl> _impl;

    friend Nullable<shared_ptr<Dxc>> CreateDxc() noexcept;
};

Nullable<shared_ptr<Dxc>> CreateDxc() noexcept;

#endif

}  // namespace radray::shader
