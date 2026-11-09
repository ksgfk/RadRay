#include "gpu_test_fixture.h"

#include <radray/logger.h>
#include <radray/render/external_resource.h>

namespace radray::render {
namespace {

// Opaque handles are only defined for Vulkan to Vulkan; every other route shares a D3D12 resource handle.
struct Route {
    RenderBackend Source, Destination;
    ExternalMemoryHandleType Handle{ExternalMemoryHandleType::D3D12Resource};
};

constexpr Route AllRoutes[]{{RenderBackend::D3D12, RenderBackend::D3D12},
                            {RenderBackend::D3D12, RenderBackend::Vulkan},
                            {RenderBackend::Vulkan, RenderBackend::D3D12},
                            {RenderBackend::Vulkan, RenderBackend::Vulkan, ExternalMemoryHandleType::OpaqueWin32}};

auto RouteName(const testing::TestParamInfo<Route>& info) {
    const auto name = [](RenderBackend backend) { return backend == RenderBackend::D3D12 ? "D3D12" : "Vulkan"; };
    return std::string{radray::format("{}To{}", name(info.param.Source), name(info.param.Destination))};
}

constexpr uint32_t BufferSize = 256;
constexpr uint32_t TextureSize = 4;

const BufferDescriptor SharedBuffer{.Size = BufferSize, .Memory = MemoryType::Device, .Usage = BufferUse::CopySource | BufferUse::CopyDestination};
const TextureDescriptor SharedTexture{.Dim = TextureDimension::Dim2D, .Width = TextureSize, .Height = TextureSize, .DepthOrArraySize = 1, .MipLevels = 1,
                                      .SampleCount = 1, .Format = TextureFormat::RGBA8_UNORM, .Memory = MemoryType::Device,
                                      .Usage = TextureUse::CopySource | TextureUse::CopyDestination};

vector<byte> Pattern(size_t size, uint8_t seed) {
    vector<byte> bytes(size);
    for (size_t i = 0; i < size; ++i) bytes[i] = static_cast<byte>(seed + i * 13);
    return bytes;
}

template <class T>
unique_ptr<T> Take(ExternalResult<unique_ptr<T>> result) {
    if (auto* error = std::get_if<ExternalError>(&result)) {
        ADD_FAILURE() << "external resource error: " << error->Message << " (native " << error->NativeCode << ")";
        return nullptr;
    }
    return std::get<unique_ptr<T>>(std::move(result));
}

template <class T>
std::optional<T> TakeExport(ExternalResult<T> result) {
    if (auto* error = std::get_if<ExternalError>(&result)) {
        ADD_FAILURE() << "external resource error: " << error->Message << " (native " << error->NativeCode << ")";
        return std::nullopt;
    }
    return std::get<T>(std::move(result));
}

// Vulkan VK_ERROR_FORMAT_NOT_SUPPORTED is a capability answer; other native codes are query failures.
testing::AssertionResult QuerySucceeded(const ExternalSupport& support) {
    const auto& reason = support.Reason;
    if (reason.NativeApi == ExternalNativeApi::None || reason.NativeCode == 0 ||
        (reason.NativeApi == ExternalNativeApi::Vulkan && reason.NativeCode == -11))
        return testing::AssertionSuccess();
    return testing::AssertionFailure() << "support query failed: " << reason.Message << " (native " << reason.NativeCode << ")";
}

// Each side writes a pattern for the other and reads what the other wrote.
// The source creates and exports the resource; the destination imports it.
class ExternalResourceTest : public test::GpuTest, public testing::WithParamInterface<Route> {
protected:
    void SetUp() override {
        _source = OpenDevice(GetParam().Source, true).Get();
        if (!_source) return;
        _destination = OpenDevice(GetParam().Destination, true).Get();
        if (!_destination) return;
        _exporter = _source->GetExternalResourceExtension().Get();
        _importer = _destination->GetExternalResourceExtension().Get();
        if (!_exporter || !_importer) GTEST_SKIP() << "external resource extension is unavailable";
        const auto sourceIdentity = _exporter->GetDeviceIdentity();
        const auto destinationIdentity = _importer->GetDeviceIdentity();
        for (const auto* identity : {&sourceIdentity, &destinationIdentity})
            if (const auto* error = std::get_if<ExternalError>(identity)) GTEST_SKIP() << error->Message;
        if (std::get<ExternalDeviceIdentity>(sourceIdentity) != std::get<ExternalDeviceIdentity>(destinationIdentity))
            GTEST_SKIP() << "devices are on different adapters";
    }

    // Skips when either side reports the route as unsupported.
    void RequireSupport(const ExternalSupport& source, const ExternalSupport& destination) {
        ASSERT_TRUE(QuerySucceeded(source));
        ASSERT_TRUE(QuerySucceeded(destination));
        if (!source.CanCreateExportable || !destination.CanImport)
            GTEST_SKIP() << "route unsupported: " << source.Reason.Message << " " << destination.Reason.Message;
    }

    Device* _source{nullptr};
    Device* _destination{nullptr};
    ExternalResourceExtension* _exporter{nullptr};
    ExternalResourceExtension* _importer{nullptr};
};

ResourceBarrierDescriptor BufferBarrier(Buffer& buffer, BufferStates before, BufferStates after,
                                        ExternalOwnershipTransfer transfer = ExternalOwnershipTransfer::None) {
    return BarrierBufferDescriptor{.Target = &buffer, .Before = before, .After = after, .ExternalTransfer = transfer};
}

ResourceBarrierDescriptor TextureBarrier(Texture& texture, TextureStates before, TextureStates after,
                                         ExternalOwnershipTransfer transfer = ExternalOwnershipTransfer::None) {
    return BarrierTextureDescriptor{.Target = &texture, .Before = before, .After = after, .ExternalTransfer = transfer};
}

// One side's turn. With `acquire` it takes ownership and returns the current content;
// otherwise the content is undefined and the first write initializes it. A non-empty
// `next` is then written and ownership is released for the other side.
vector<byte> BufferTurn(Device& device, Buffer& shared, bool acquire, std::span<const byte> next) {
    auto readback = test::CreateReadbackBuffer(device, BufferSize).Unwrap();
    unique_ptr<Buffer> upload;
    if (!next.empty()) upload = test::CreateUploadBuffer(device, next, BufferUse::CopySource).Unwrap();
    auto recording = test::BeginRecording(device);
    auto& commands = *recording.Commands;
    if (acquire) {
        test::Barrier(commands, {BufferBarrier(shared, BufferState::Common, BufferState::CopySource, ExternalOwnershipTransfer::Acquire)});
        commands.CopyBufferToBuffer(readback.get(), 0, &shared, 0, BufferSize);
        test::Barrier(commands, {BufferBarrier(*readback, BufferState::CopyDestination, BufferState::HostRead)});
    }
    if (upload) {
        test::Barrier(commands, {BufferBarrier(shared, acquire ? BufferState::CopySource : BufferState::Undefined, BufferState::CopyDestination),
                                 BufferBarrier(*upload, BufferState::HostWrite, BufferState::CopySource)});
        commands.CopyBufferToBuffer(&shared, 0, upload.get(), 0, BufferSize);
        test::Barrier(commands, {BufferBarrier(shared, BufferState::CopyDestination, BufferState::Common, ExternalOwnershipTransfer::Release)});
    }
    test::SubmitAndWait(device, recording);
    return acquire ? test::MapAndRead(*readback) : vector<byte>{};
}

vector<byte> TextureTurn(Device& device, Texture& shared, bool acquire, std::span<const byte> next) {
    constexpr uint32_t rowSize = TextureSize * 4;
    const uint32_t pitch = test::CopyRowPitch(device, TextureSize, 4);
    vector<byte> pitched(size_t{pitch} * TextureSize);
    for (uint32_t y = 0; y < TextureSize && !next.empty(); ++y) std::memcpy(pitched.data() + y * pitch, next.data() + y * rowSize, rowSize);
    auto upload = test::CreateUploadBuffer(device, pitched, BufferUse::CopySource).Unwrap();
    auto readback = test::CreateReadbackBuffer(device, pitched.size()).Unwrap();
    auto recording = test::BeginRecording(device);
    auto& commands = *recording.Commands;
    if (acquire) {
        test::Barrier(commands, {TextureBarrier(shared, TextureState::Common, TextureState::CopySource, ExternalOwnershipTransfer::Acquire)});
        commands.CopyTextureToBuffer(readback.get(), 0, &shared, {0, 1, 0, 1});
        test::Barrier(commands, {BufferBarrier(*readback, BufferState::CopyDestination, BufferState::HostRead)});
    }
    if (!next.empty()) {
        test::Barrier(commands, {TextureBarrier(shared, acquire ? TextureState::CopySource : TextureState::Undefined, TextureState::CopyDestination),
                                 BufferBarrier(*upload, BufferState::HostWrite, BufferState::CopySource)});
        commands.CopyBufferToTexture(&shared, {0, 1, 0, 1}, upload.get(), 0);
        test::Barrier(commands, {TextureBarrier(shared, TextureState::CopyDestination, TextureState::Common, ExternalOwnershipTransfer::Release)});
    }
    test::SubmitAndWait(device, recording);
    if (!acquire) return {};
    const auto read = test::MapAndRead(*readback);
    vector<byte> texels;
    for (uint32_t y = 0; y < TextureSize && !read.empty(); ++y) {
        const auto row = read.begin() + ptrdiff_t(size_t{y} * pitch);
        texels.insert(texels.end(), row, row + rowSize);
    }
    return texels;
}

TEST_P(ExternalResourceTest, BufferRoundTrip) {
    const ExternalResourceDescriptor external{.HandleType = GetParam().Handle};
    RequireSupport(_exporter->QueryBufferSupport(SharedBuffer, external), _importer->QueryBufferSupport(SharedBuffer, external));
    if (IsSkipped() || HasFatalFailure()) return;
    const auto forward = Pattern(BufferSize, 1);
    const auto backward = Pattern(BufferSize, 2);
    auto exported = Take(_exporter->CreateExportableBuffer(SharedBuffer, external));
    ASSERT_TRUE(exported);
    BufferTurn(*_source, *exported, false, forward);
    auto handle = TakeExport(_exporter->ExportBuffer(exported.get()));
    ASSERT_TRUE(handle);
    auto imported = Take(_importer->ImportBuffer(handle->Handle.Borrow(), handle->Info));
    ASSERT_TRUE(imported);
    handle.reset();

    EXPECT_EQ(BufferTurn(*_destination, *imported, true, backward), forward);
    EXPECT_EQ(BufferTurn(*_source, *exported, true, {}), backward);
}

TEST_P(ExternalResourceTest, TextureRoundTrip) {
    const ExternalTextureDescriptor external{.HandleType = GetParam().Handle};
    RequireSupport(_exporter->QueryTextureSupport(SharedTexture, external), _importer->QueryTextureSupport(SharedTexture, external));
    if (IsSkipped() || HasFatalFailure()) return;
    const auto forward = Pattern(TextureSize * TextureSize * 4, 3);
    const auto backward = Pattern(TextureSize * TextureSize * 4, 4);
    auto exported = Take(_exporter->CreateExportableTexture(SharedTexture, external));
    ASSERT_TRUE(exported);
    TextureTurn(*_source, *exported, false, forward);
    auto handle = TakeExport(_exporter->ExportTexture(exported.get()));
    ASSERT_TRUE(handle);
    auto imported = Take(_importer->ImportTexture(handle->Handle.Borrow(), handle->Info));
    ASSERT_TRUE(imported);
    handle.reset();

    EXPECT_EQ(TextureTurn(*_destination, *imported, true, backward), forward);
    EXPECT_EQ(TextureTurn(*_source, *exported, true, {}), backward);
}

INSTANTIATE_TEST_SUITE_P(Routes, ExternalResourceTest, testing::ValuesIn(AllRoutes), RouteName);

}  // namespace
}  // namespace radray::render
