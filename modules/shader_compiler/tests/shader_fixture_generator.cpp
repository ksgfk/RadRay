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
    {"texture_sampler"},
    {"shadow_static_sampler"},
    {"spirv_push_constant"},
    {"nested_types"},
    {"compute"},
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

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: radray_shader_fixture_generator <output-directory>\n";
        return 1;
    }
    const std::filesystem::path outputRoot{argv[1]};
    const std::filesystem::path root{RADRAY_PROJECT_DIR};
    const std::filesystem::path sourceRoot = root / "modules/shader/tests/data/shader_sources";
    const std::filesystem::path artifactRoot = outputRoot / "shader_artifacts";
    const vector<std::filesystem::path> includePaths{root / "shaderlib"};
    std::error_code error;
    std::filesystem::create_directories(artifactRoot, error);
    if (error) {
        std::cerr << "cannot create " << artifactRoot << ": " << error.message() << '\n';
        return 1;
    }

    Client client;
    if (!client.IsAvailable()) {
        std::cerr << "RadRay DXC fork is unavailable; check the SDK package and DLL search path.\n";
        return 2;
    }

    for (const FixtureDescription& fixture : kFixtures) {
        vector<byte> source;
        const string fileName = string{fixture.Name} + ".hlsl";
        const auto sourcePath = sourceRoot / fileName;
        if (!ReadFile(sourcePath, source)) {
            std::cerr << "cannot read " << sourcePath << '\n';
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
            std::cerr << "discovery failed for " << fileName << '\n'
                      << discovery.Diagnostics << '\n';
            return 4;
        }

        CompileVariantRequest request{
            .SourceName = sourceName,
            .RootSource = source,
            .Defines = {},
            .Assignments = {},
            .Targets = ShaderTargetMask::All,
            .ExpectedContract = discovery.Value->Hash};
        const auto result = client.CompileVariant(request, includePaths);
        if (!result.Succeeded() || result.Value->size() != 2) {
            std::cerr << "compile failed for " << fileName << '\n'
                      << result.Diagnostics << '\n';
            return 5;
        }
        for (const ShaderTarget target : {ShaderTarget::DXIL, ShaderTarget::SPIRV}) {
            const auto lane = std::find_if(
                result.Value->begin(),
                result.Value->end(),
                [target](const CompileTargetLane& value) noexcept { return value.Target == target; });
            if (lane == result.Value->end() || lane->Metadata.empty()) {
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
