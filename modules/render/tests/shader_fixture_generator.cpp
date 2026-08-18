#include <radray/shader_compiler/client.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {

using radray::byte;
using radray::string;
using radray::vector;
using radray::shader::CompileTargetLane;
using radray::shader::CompileVariantRequest;
using radray::shader::ShaderTarget;
using radray::shader::ShaderTargetMask;
using radray::shader_compiler::Client;

struct FixtureDescription {
    const char* Name;
};

constexpr FixtureDescription kFixtures[] = {
    {"no_resource_graphics"},
    {"vertex_only"},
    {"depth_only"},
    {"texture_sampler"},
    {"shadow_static_sampler"},
    {"multiple_root_constants"},
    {"spirv_push_constant"},
    {"target_specific_bindings"},
    {"nested_types"},
    {"multiple_cbuffers"},
    {"compute"},
    {"unused_resource"},
};

bool ReadFile(const std::filesystem::path& path, vector<byte>& output) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    if (size <= 0) {
        return false;
    }
    file.seekg(0, std::ios::beg);
    output.resize(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(output.data()), size);
    return file.good() || file.eof();
}

bool WriteArtifact(
    const std::filesystem::path& outputPath,
    const CompileTargetLane& lane) {
    std::ofstream file(outputPath, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    file.write(
        reinterpret_cast<const char*>(lane.Metadata.data()),
        static_cast<std::streamsize>(lane.Metadata.size()));
    return file.good();
}

}  // namespace

int main() {
    const std::filesystem::path root{RADRAY_PROJECT_DIR};
    const std::filesystem::path sourceRoot = root / "modules/render/tests/data/shader_sources";
    const std::filesystem::path artifactRoot = root / "modules/render/tests/data/shader_artifacts";
    const vector<std::filesystem::path> includePaths{root / "shaderlib"};
    std::filesystem::create_directories(artifactRoot);

    Client client;
    if (!client.IsAvailable()) {
        return 2;
    }

    for (const FixtureDescription& fixture : kFixtures) {
        vector<byte> source;
        const string fileName = string{fixture.Name} + ".hlsl";
        if (!ReadFile(sourceRoot / fileName, source)) {
            return 3;
        }

        const string sourceName = "fixtures/" + fileName;
        const auto discovery = client.DiscoverSourceContract(
            radray::shader::SourceContractRequest{
                .SourceName = sourceName,
                .RootSource = source,
                .Defines = {},
                .Targets = ShaderTargetMask::All,
                .Policy = {}},
            includePaths);
        if (!discovery.Succeeded()) {
            std::cerr << "discovery failed for " << fileName << '\n';
            for (const auto& diagnostic : discovery.Diagnostics) {
                std::cerr << diagnostic.Code << ": " << diagnostic.Message << '\n';
            }
            return 4;
        }

        CompileVariantRequest request{
            .SourceName = sourceName,
            .RootSource = source,
            .Defines = {},
            .Assignments = {},
            .Targets = ShaderTargetMask::All,
            .ExpectedContract = discovery.Contract.Hash};
        const auto result = client.CompileVariant(request, includePaths);
        if (result.Status != radray::shader::CompileStatus::Success || result.Lanes.size() != 2) {
            std::cerr << "compile failed for " << fileName << " status "
                      << static_cast<int>(result.Status) << '\n';
            for (const auto& diagnostic : result.Diagnostics) {
                std::cerr << diagnostic.Code << ": " << diagnostic.Message << '\n';
            }
            return 5;
        }
        for (const ShaderTarget target : {ShaderTarget::DXIL, ShaderTarget::SPIRV}) {
            const auto lane = std::find_if(
                result.Lanes.begin(),
                result.Lanes.end(),
                [target](const CompileTargetLane& value) noexcept { return value.Target == target; });
            if (lane == result.Lanes.end() || lane->Metadata.empty()) {
                return 6;
            }
            const string suffix = target == ShaderTarget::DXIL ? ".dxil.bin" : ".spirv.bin";
            if (!WriteArtifact(artifactRoot / (string{fixture.Name} + suffix), *lane)) {
                return 7;
            }
        }
    }
    return 0;
}
