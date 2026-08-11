#include "gpu_test_fixture.h"

#include <radray/runtime/asset_manager.h>
#include <radray/runtime/application.h>
#include <radray/file.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/image_asset.h>
#include <radray/runtime/shader_asset.h>
#include <radray/runtime/static_mesh.h>
#include <radray/runtime/texture_asset.h>

#include <fmt/format.h>

#include <gtest/gtest.h>

#include <cstring>
#include <fstream>
#include <optional>

namespace radray {
class BundleTestAsset;

template <>
struct RuntimeTypeTrait<BundleTestAsset> {
    static constexpr RuntimeTypeId value{
        0x5aaf1011,
        0x3c22,
        0x4d73,
        0x9e,
        0x01,
        0x17,
        0xa0,
        0x4b,
        0x55,
        0x80,
        0x02};
    using Bases = std::tuple<Asset>;
};

class BundleTestAsset final : public Asset {
public:
    void OnUnload(AssetManager&) override {}

    RuntimeTypeId GetTypeId() const noexcept override { return runtime_type_id_v<BundleTestAsset>; }
};

namespace {

constexpr Guid MakeId(uint32_t value) noexcept {
    return Guid{value, 0x0001, 0x4000, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
}

BundleAssetEntry MakeEntry(const AssetId& id, std::string_view path, BundleEntryState state = BundleEntryState::Valid) {
    BundleAssetEntry entry;
    entry.Asset = id;
    entry.TypeId = runtime_type_id_v<BundleTestAsset>;
    entry.TypeName = "test";
    entry.Locator = BundleLocator::TryCreate(path);
    entry.State = state;
    return entry;
}

BundleCatalog MakeCatalog(const BundleId& id, vector<BundleAssetEntry> entries) {
    BundleCatalog catalog;
    catalog.Id = id;
    catalog.Entries = std::move(entries);
    return catalog;
}

vector<BundleAssetEntry> OneEntry(BundleAssetEntry entry) {
    vector<BundleAssetEntry> entries;
    entries.push_back(std::move(entry));
    return entries;
}

void AppendU16(vector<byte>& bytes, uint16_t value) {
    bytes.push_back(static_cast<byte>(value & 0xffu));
    bytes.push_back(static_cast<byte>((value >> 8u) & 0xffu));
}

void AppendU32(vector<byte>& bytes, uint32_t value) {
    for (uint32_t i = 0; i < 4; ++i) {
        bytes.push_back(static_cast<byte>((value >> (8u * i)) & 0xffu));
    }
}

void AppendU64(vector<byte>& bytes, uint64_t value) {
    for (uint32_t i = 0; i < 8; ++i) {
        bytes.push_back(static_cast<byte>((value >> (8u * i)) & 0xffu));
    }
}

void AppendString(vector<byte>& bytes, std::string_view value) {
    AppendU32(bytes, static_cast<uint32_t>(value.size()));
    const auto* first = reinterpret_cast<const byte*>(value.data());
    bytes.insert(bytes.end(), first, first + value.size());
}

vector<byte> MakeSimpleMeshPayload() {
    constexpr float vertices[] = {
        0.0f, 0.0f, 0.0f,
        1.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f};
    constexpr uint32_t indices[] = {0, 1, 2};

    vector<byte> vertexBytes(sizeof(vertices));
    vector<byte> indexBytes(sizeof(indices));
    std::memcpy(vertexBytes.data(), vertices, sizeof(vertices));
    std::memcpy(indexBytes.data(), indices, sizeof(indices));

    vector<byte> payload;
    constexpr std::string_view magic = "RRMESH01";
    payload.insert(
        payload.end(),
        reinterpret_cast<const byte*>(magic.data()),
        reinterpret_cast<const byte*>(magic.data()) + magic.size());
    AppendU32(payload, 1);  // version
    AppendU32(payload, 2);  // bin count
    AppendU32(payload, 1);  // primitive count
    AppendString(payload, "triangle");

    AppendU64(payload, vertexBytes.size());
    payload.insert(payload.end(), vertexBytes.begin(), vertexBytes.end());
    AppendU64(payload, indexBytes.size());
    payload.insert(payload.end(), indexBytes.begin(), indexBytes.end());

    AppendU32(payload, 3);  // vertex count
    AppendU32(payload, 1);  // index buffer bin
    AppendU32(payload, 3);  // index count
    AppendU32(payload, 0);  // index offset
    AppendU32(payload, sizeof(uint32_t));
    AppendU32(payload, 1);  // attribute count
    AppendString(payload, VertexSemantics::POSITION);
    AppendU32(payload, 0);  // semantic index
    AppendU32(payload, 0);  // vertex buffer bin
    AppendU16(payload, static_cast<uint16_t>(VertexDataType::FLOAT));
    AppendU16(payload, 3);  // component count
    AppendU32(payload, 0);  // attribute offset
    AppendU32(payload, sizeof(float) * 3);
    return payload;
}

bool WriteBytes(const std::filesystem::path& path, std::span<const byte> bytes) {
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return file.good();
}

std::optional<render::RenderBackend> ProbeAssetGpuBackend() {
    render::test::DeviceContext context;
    if (render::test::TryCreateAnyDevice(context)) {
        return context.Device->GetBackend();
    }
    return std::nullopt;
}

GpuSystemDescriptor MakeAssetGpuDescriptor(render::RenderBackend backend) {
    GpuSystemDescriptor descriptor;
    descriptor.BackBufferCount = 1;
    descriptor.FlightDataCount = 2;
    if (backend == render::RenderBackend::D3D12) {
        descriptor.Device = render::DeviceDescriptor{render::D3D12DeviceDescriptor{}};
    } else {
        static constexpr render::VulkanCommandQueueDescriptor queue{
            .Type = render::QueueType::Direct,
            .Count = 1};
        render::VulkanDeviceDescriptor device;
        device.Queues = std::span{&queue, 1};
        descriptor.Device = device;
    }
    return descriptor;
}

void PumpAssetGpuFrame(AssetManager& manager, GpuSystem& gpu, uint32_t flightIndex) {
    gpu.BeginUpdateForFlight(flightIndex);
    manager.Pump();
    (void)gpu.BeginFrameRecord(flightIndex, {}, {}, false);
    gpu.EndFrameRecordAndSubmit(flightIndex);
    ASSERT_TRUE(gpu.CompleteFlightIfReady(flightIndex, true));
    gpu.PumpFrameUploadScheduler();
    manager.Pump();
}

task<AssetLoadResult> LoadBundleTestAsset(
    AssetManager&,
    const BundleAssetEntry& entry,
    const std::filesystem::path& root) {
    // The callback observes the entry synchronously and the task only owns the resulting object.
    (void)entry;
    (void)root;
    co_return AssetLoadResult::Success(make_unique<BundleTestAsset>());
}

task<AssetLoadResult> FailBundleTestAsset(
    AssetManager&, const BundleAssetEntry&, const std::filesystem::path&) {
    co_return AssetLoadResult::Failure("intentional bundle test failure");
}

}  // namespace

TEST(AssetBundleValueTest, LocatorRejectsAbsoluteAndEscapeForms) {
    EXPECT_TRUE(BundleLocator::TryCreate("passes/example.hlsl").has_value());
    EXPECT_TRUE(BundleLocator::TryCreate("目录/文件.hlsl").has_value());

    EXPECT_FALSE(BundleLocator::TryCreate("").has_value());
    EXPECT_FALSE(BundleLocator::TryCreate("/passes/example.hlsl").has_value());
    EXPECT_FALSE(BundleLocator::TryCreate("\\passes\\example.hlsl").has_value());
    EXPECT_FALSE(BundleLocator::TryCreate("C:/passes/example.hlsl").has_value());
    EXPECT_FALSE(BundleLocator::TryCreate("//server/share/example.hlsl").has_value());
    EXPECT_FALSE(BundleLocator::TryCreate("passes//example.hlsl").has_value());
    EXPECT_FALSE(BundleLocator::TryCreate("passes/./example.hlsl").has_value());
    EXPECT_FALSE(BundleLocator::TryCreate("passes/../example.hlsl").has_value());
    EXPECT_FALSE(BundleLocator::TryCreate("passes/").has_value());

    const std::string_view embeddedNull{"passes/\0/example.hlsl", 20};
    EXPECT_FALSE(BundleLocator::TryCreate(embeddedNull).has_value());
    const std::string_view invalidUtf8{"passes/\xc0\xaf.hlsl", 15};
    EXPECT_FALSE(BundleLocator::TryCreate(invalidUtf8).has_value());

    EXPECT_EQ(MakeBundleLocatorCollisionKey("Passes/Example.hlsl"), "passes/example.hlsl");
    EXPECT_EQ(MakeBundleLocatorCollisionKey("Passes/Example.hlsl"), MakeBundleLocatorCollisionKey("passes/example.hlsl"));
}

TEST(AssetBundleMountTest, CatalogOutlivesSourceAndIsCollectedByPump) {
    AssetManager manager;
    const BundleId bundleId = MakeId(1);
    const AssetId assetId = MakeId(2);
    BundleCatalog catalog = MakeCatalog(bundleId, OneEntry(MakeEntry(assetId, "passes/example.hlsl")));

    BundleMountResult mount = manager.MountBundle(
        "relative-bundle-root",
        make_unique<MemoryBundleCatalogSource>(std::move(catalog)));
    ASSERT_TRUE(mount.IsSuccess());
    ASSERT_TRUE(mount.Reference.has_value());
    EXPECT_EQ(manager.GetBundleCount(), 1u);

    BundleRef held = *mount.Reference;
    mount.Reference.reset();
    ASSERT_TRUE(held.GetRoot());
    EXPECT_TRUE(held.GetRoot()->is_absolute());
    ASSERT_TRUE(held.GetCatalog());
    EXPECT_EQ(held.GetCatalog()->Id, bundleId);
    ASSERT_EQ(held.GetCatalog()->Entries.size(), 1u);
    EXPECT_EQ(held.GetCatalog()->Entries.front().Asset, assetId);
    EXPECT_EQ(held.GetCatalog()->Entries.front().Locator->GetValue(), "passes/example.hlsl");

    EXPECT_TRUE(manager.FindBundle(bundleId) == held);
    held.Reset();
    BundleRef reacquired = manager.FindBundle(bundleId);
    ASSERT_TRUE(reacquired.IsValid());
    reacquired.Reset();
    EXPECT_EQ(manager.GetBundleCount(), 1u) << "Bundle collection is Pump-aligned";
    manager.Pump();
    EXPECT_EQ(manager.GetBundleCount(), 0u);
    EXPECT_FALSE(manager.FindBundle(bundleId).IsValid());
}

TEST(AssetBundleMountTest, IdConflictsRejectTheWholeMount) {
    AssetManager manager;
    const AssetId existingAssetId = MakeId(10);
    BundleCatalog firstCatalog = MakeCatalog(MakeId(11), OneEntry(MakeEntry(existingAssetId, "a/test.asset")));
    BundleMountResult first = manager.MountBundle("first", make_unique<MemoryBundleCatalogSource>(std::move(firstCatalog)));
    ASSERT_TRUE(first.IsSuccess());

    BundleCatalog duplicateAssetCatalog = MakeCatalog(MakeId(12), OneEntry(MakeEntry(existingAssetId, "b/test.asset")));
    BundleMountResult duplicateAsset = manager.MountBundle(
        "second", make_unique<MemoryBundleCatalogSource>(std::move(duplicateAssetCatalog)));
    ASSERT_FALSE(duplicateAsset.IsSuccess());
    ASSERT_FALSE(duplicateAsset.Diagnostics.empty());
    EXPECT_EQ(duplicateAsset.Diagnostics.front().Code, BundleDiagnosticCode::AssetIdAlreadyInUse);
    EXPECT_EQ(manager.GetBundleCount(), 1u);

    vector<BundleAssetEntry> duplicateEntries;
    duplicateEntries.push_back(MakeEntry(MakeId(20), "a/test.asset"));
    duplicateEntries.push_back(MakeEntry(MakeId(20), "b/test.asset"));
    BundleMountResult duplicateInsideCatalog = manager.MountBundle(
        "third",
        make_unique<MemoryBundleCatalogSource>(MakeCatalog(MakeId(13), std::move(duplicateEntries))));
    ASSERT_FALSE(duplicateInsideCatalog.IsSuccess());
    EXPECT_EQ(duplicateInsideCatalog.Diagnostics.front().Code, BundleDiagnosticCode::DuplicateAssetId);
    EXPECT_EQ(manager.GetBundleCount(), 1u);

    StreamingAssetRef<BundleTestAsset> addReady = manager.AddReady<BundleTestAsset>(
        existingAssetId,
        make_unique<BundleTestAsset>());
    EXPECT_FALSE(addReady.IsValid()) << "a mounted Catalog owns the AssetId";
    EXPECT_EQ(manager.GetAssetCount(), 0u);
}

TEST(AssetBundleMountTest, ExistingSlotAlsoBlocksMount) {
    AssetManager manager;
    const AssetId assetId = MakeId(30);
    StreamingAssetRef<BundleTestAsset> ready = manager.AddReady<BundleTestAsset>(
        assetId,
        make_unique<BundleTestAsset>());
    ASSERT_TRUE(ready.IsReady());

    BundleCatalog conflictCatalog = MakeCatalog(MakeId(31), OneEntry(MakeEntry(assetId, "a/test.asset")));
    BundleMountResult conflict = manager.MountBundle(
        "slot-conflict", make_unique<MemoryBundleCatalogSource>(std::move(conflictCatalog)));
    ASSERT_FALSE(conflict.IsSuccess());
    ASSERT_FALSE(conflict.Diagnostics.empty());
    EXPECT_EQ(conflict.Diagnostics.front().Code, BundleDiagnosticCode::AssetIdAlreadyInUse);
    EXPECT_EQ(manager.GetBundleCount(), 0u);
    ready.Reset();
}

TEST(AssetBundleMountTest, LoadingAndFaultedSlotsAlsoBlockMount) {
    AssetManager manager;
    static const BundleAssetEntry dummyEntry{};
    static const std::filesystem::path dummyRoot{"."};
    const AssetId loadingId = MakeId(32);
    StreamingAssetRefAny loading = manager.Load(AssetLoadRequest{
        .Id = loadingId,
        .Task = LoadBundleTestAsset(manager, dummyEntry, dummyRoot),
        .DebugName = "loading-conflict"});
    ASSERT_TRUE(loading.IsValid());
    EXPECT_FALSE(loading.IsCompleted());

    BundleMountResult loadingConflict = manager.MountBundle(
        "loading-conflict",
        make_unique<MemoryBundleCatalogSource>(
            MakeCatalog(MakeId(33), OneEntry(MakeEntry(loadingId, "a.asset")))));
    ASSERT_FALSE(loadingConflict.IsSuccess());
    ASSERT_FALSE(loadingConflict.Diagnostics.empty());
    EXPECT_EQ(loadingConflict.Diagnostics.front().Code, BundleDiagnosticCode::AssetIdAlreadyInUse);

    manager.Pump();
    EXPECT_TRUE(loading.IsReady());
    loading.Reset();

    const AssetId faultedId = MakeId(34);
    StreamingAssetRefAny faulted = manager.Load(AssetLoadRequest{
        .Id = faultedId,
        .Task = FailBundleTestAsset(manager, dummyEntry, dummyRoot),
        .DebugName = "faulted-conflict"});
    manager.Pump();
    ASSERT_TRUE(faulted.IsFaulted());
    BundleMountResult faultedConflict = manager.MountBundle(
        "faulted-conflict",
        make_unique<MemoryBundleCatalogSource>(
            MakeCatalog(MakeId(35), OneEntry(MakeEntry(faultedId, "b.asset")))));
    ASSERT_FALSE(faultedConflict.IsSuccess());
    ASSERT_FALSE(faultedConflict.Diagnostics.empty());
    EXPECT_EQ(faultedConflict.Diagnostics.front().Code, BundleDiagnosticCode::AssetIdAlreadyInUse);
    faulted.Reset();
}

TEST(AssetBundleMountTest, UnknownAndInvalidEntriesRemainQueryableMetadata) {
    BundleAssetEntry unknown = MakeEntry(MakeId(40), "unknown/asset.bin", BundleEntryState::Unknown);
    unknown.TypeName = "future-type";
    unknown.Descriptor.reset();

    BundleAssetEntry invalid = MakeEntry(MakeId(41), "", BundleEntryState::Invalid);
    invalid.Diagnostics.push_back(BundleDiagnostic{
        .Code = BundleDiagnosticCode::InvalidDescriptor,
        .Message = "descriptor is not valid for this entry",
        .Asset = invalid.Asset,
    });

    vector<BundleAssetEntry> entries;
    entries.push_back(std::move(unknown));
    entries.push_back(std::move(invalid));

    AssetManager manager;
    BundleMountResult mount = manager.MountBundle(
        "metadata-only",
        make_unique<MemoryBundleCatalogSource>(MakeCatalog(MakeId(42), std::move(entries))));
    ASSERT_TRUE(mount.IsSuccess());
    ASSERT_TRUE(mount.Reference.has_value());
    ASSERT_TRUE(mount.Reference->GetCatalog());
    const BundleCatalog& catalog = *mount.Reference->GetCatalog();
    ASSERT_EQ(catalog.Entries.size(), 2u);
    EXPECT_EQ(catalog.Entries[0].State, BundleEntryState::Unknown);
    EXPECT_EQ(catalog.Entries[0].Asset, MakeId(40));
    EXPECT_EQ(catalog.Entries[1].State, BundleEntryState::Invalid);
    EXPECT_FALSE(catalog.Entries[1].Locator.has_value());
    ASSERT_EQ(catalog.Entries[1].Diagnostics.size(), 1u);
}

TEST(AssetBundleLoadTest, DispatchesByCatalogTypeAndRetainsStructuredFaults) {
    AssetManager manager;
    ASSERT_TRUE(manager.RegisterBundleLoader(runtime_type_id_v<BundleTestAsset>, &LoadBundleTestAsset));
    EXPECT_FALSE(manager.RegisterBundleLoader(runtime_type_id_v<BundleTestAsset>, &LoadBundleTestAsset));

    const AssetId validId = MakeId(50);
    const AssetId unknownId = MakeId(51);
    const AssetId invalidId = MakeId(52);
    BundleAssetEntry unknown = MakeEntry(unknownId, "future/asset.bin", BundleEntryState::Unknown);
    unknown.TypeName = "future";
    BundleAssetEntry invalid = MakeEntry(invalidId, "", BundleEntryState::Invalid);
    invalid.Locator.reset();
    invalid.Diagnostics.push_back(BundleDiagnostic{
        .Code = BundleDiagnosticCode::InvalidDescriptor,
        .Message = "invalid test descriptor",
        .Asset = invalidId,
    });
    vector<BundleAssetEntry> entries;
    entries.push_back(MakeEntry(validId, "valid.asset"));
    entries.back().State = BundleEntryState::Valid;
    entries.push_back(std::move(unknown));
    entries.push_back(std::move(invalid));

    BundleMountResult mount = manager.MountBundle(
        "loader-bundle",
        make_unique<MemoryBundleCatalogSource>(MakeCatalog(MakeId(53), std::move(entries))));
    ASSERT_TRUE(mount.IsSuccess());
    EXPECT_FALSE(manager.RegisterBundleLoader(MakeId(54), &LoadBundleTestAsset));

    AssetRequestResult missing = manager.LoadCatalog(MakeId(55));
    ASSERT_TRUE(missing.HasError());
    EXPECT_EQ(missing.Error->Code, AssetLoadErrorCode::NotFound);
    EXPECT_FALSE(missing.IsSubmitted());
    EXPECT_EQ(manager.GetAssetCount(), 0u);

    AssetRequestResult mismatch = manager.LoadCatalog(validId, MakeId(56));
    ASSERT_TRUE(mismatch.HasError());
    EXPECT_EQ(mismatch.Error->Code, AssetLoadErrorCode::RequestTypeMismatch);
    EXPECT_FALSE(mismatch.IsSubmitted());
    EXPECT_EQ(manager.GetAssetCount(), 0u);

    AssetRequestResult valid = manager.LoadCatalog(validId, runtime_type_id_v<BundleTestAsset>);
    ASSERT_TRUE(valid.IsSubmitted());
    ASSERT_TRUE(valid.Reference.has_value());
    EXPECT_FALSE(valid.Reference->IsReady());
    manager.Pump();
    EXPECT_TRUE(valid.Reference->IsReady());
    EXPECT_EQ(valid.Reference->GetErrorCode(), AssetLoadErrorCode::None);

    AssetRequestResult unknownRequest = manager.LoadCatalog(unknownId);
    ASSERT_TRUE(unknownRequest.IsSubmitted());
    ASSERT_TRUE(unknownRequest.Reference.has_value());
    EXPECT_TRUE(unknownRequest.Reference->IsFaulted());
    EXPECT_EQ(unknownRequest.Reference->GetErrorCode(), AssetLoadErrorCode::UnknownType);
    const string unknownMessage = unknownRequest.Reference->GetErrorMessage();

    AssetRequestResult unknownAgain = manager.LoadCatalog(unknownId);
    ASSERT_TRUE(unknownAgain.IsSubmitted());
    EXPECT_TRUE(*unknownAgain.Reference == *unknownRequest.Reference);
    EXPECT_EQ(unknownAgain.Reference->GetErrorCode(), AssetLoadErrorCode::UnknownType);
    EXPECT_EQ(unknownAgain.Reference->GetErrorMessage(), unknownMessage);

    AssetRequestResult invalidRequest = manager.LoadCatalog(invalidId);
    ASSERT_TRUE(invalidRequest.IsSubmitted());
    ASSERT_TRUE(invalidRequest.Reference.has_value());
    EXPECT_TRUE(invalidRequest.Reference->IsFaulted());
    EXPECT_EQ(invalidRequest.Reference->GetErrorCode(), AssetLoadErrorCode::InvalidDescriptor);
}

TEST(AssetBundleLoadTest, BundleRemovalLeavesExistingSlotUntilItIsCollected) {
    const AssetId assetId = MakeId(57);
    BundleAssetEntry unknown = MakeEntry(assetId, "future.asset", BundleEntryState::Unknown);
    unknown.TypeName = "future";
    unknown.Descriptor.reset();

    AssetManager manager;
    BundleMountResult mount = manager.MountBundle(
        "removal",
        make_unique<MemoryBundleCatalogSource>(MakeCatalog(MakeId(58), OneEntry(std::move(unknown)))));
    ASSERT_TRUE(mount.IsSuccess());
    AssetRequestResult first = manager.LoadCatalog(assetId);
    ASSERT_TRUE(first.IsSubmitted());
    ASSERT_TRUE(first.Reference.has_value());
    EXPECT_TRUE(first.Reference->IsFaulted());

    mount.Reference.reset();
    manager.Pump();
    EXPECT_EQ(manager.GetBundleCount(), 0u);
    AssetRequestResult whileSlotLives = manager.LoadCatalog(assetId);
    ASSERT_TRUE(whileSlotLives.IsSubmitted());
    ASSERT_TRUE(whileSlotLives.Reference.has_value());
    EXPECT_TRUE(*whileSlotLives.Reference == *first.Reference);

    first.Reference->Reset();
    whileSlotLives.Reference->Reset();
    manager.Pump();
    EXPECT_EQ(manager.GetAssetCount(), 0u);
    AssetRequestResult afterCollection = manager.LoadCatalog(assetId);
    ASSERT_TRUE(afterCollection.HasError());
    EXPECT_EQ(afterCollection.Error->Code, AssetLoadErrorCode::NotFound);
}

TEST(AssetBundleLoadTest, SafeImageLoaderOwnsItsDataAfterBundleRemoval) {
    BundleAssetEntry image;
    image.Asset = MakeId(59);
    image.TypeId = runtime_type_id_v<ImageAsset>;
    image.TypeName = "image";
    image.Locator = BundleLocator::TryCreate("missing.png");
    image.State = BundleEntryState::Valid;
    image.Descriptor = make_unique<ImageAssetDescriptor>();

    AssetManager manager;
    BundleMountResult mount = manager.MountBundle(
        "image-root", make_unique<MemoryBundleCatalogSource>(MakeCatalog(MakeId(60), OneEntry(std::move(image)))));
    ASSERT_TRUE(mount.IsSuccess());
    mount.Reference.reset();

    AssetRequestResult request = manager.LoadCatalog(MakeId(59), runtime_type_id_v<ImageAsset>);
    ASSERT_TRUE(request.IsSubmitted());
    ASSERT_TRUE(request.Reference.has_value());
    manager.Pump();
    EXPECT_TRUE(request.Reference->IsFaulted());
    EXPECT_EQ(request.Reference->GetErrorCode(), AssetLoadErrorCode::PayloadFailure);
    EXPECT_EQ(manager.GetBundleCount(), 0u);
}

TEST(AssetBundleLoadTest, ImageCatalogEntryLoadsWithExplicitStableAssetId) {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "radray_bundle_image_asset_test";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    ASSERT_TRUE(std::filesystem::create_directories(root, error));
    ImageData sourceImage = MakeSolidImage(10, 20, 30, 255);
    const std::filesystem::path imagePath = root / "image.png";
    ASSERT_TRUE(sourceImage.WritePNG(PNGWriteSettings{.FilePath = imagePath.string()}));

    BundleAssetEntry image;
    image.Asset = MakeId(61);
    image.TypeId = runtime_type_id_v<ImageAsset>;
    image.TypeName = "image";
    image.Locator = BundleLocator::TryCreate("image.png");
    image.State = BundleEntryState::Valid;
    image.Descriptor = make_unique<ImageAssetDescriptor>();

    AssetManager manager;
    BundleMountResult mount = manager.MountBundle(
        root, make_unique<MemoryBundleCatalogSource>(MakeCatalog(MakeId(62), OneEntry(std::move(image)))));
    ASSERT_TRUE(mount.IsSuccess());
    AssetRequestResult request = manager.LoadCatalog(MakeId(61), runtime_type_id_v<ImageAsset>);
    ASSERT_TRUE(request.IsSubmitted());
    ASSERT_TRUE(request.Reference.has_value());
    manager.Pump();
    ASSERT_TRUE(request.Reference->IsReady());
    const auto* loaded = static_cast<const ImageAsset*>(request.Reference->Get());
    ASSERT_NE(loaded, nullptr);
    EXPECT_TRUE(loaded->IsValid());

    request.Reference->Reset();
    mount.Reference.reset();
    manager.Pump();
    std::filesystem::remove_all(root, error);
}

TEST(AssetBundleLoadTest, AssetIdStaysStableWhenLocatorAndBundleMove) {
    const AssetId assetId = MakeId(65);
    BundleAssetEntry firstEntry = MakeEntry(assetId, "old/location.png", BundleEntryState::Unknown);
    firstEntry.TypeName = "future";
    firstEntry.Descriptor.reset();

    AssetManager manager;
    BundleMountResult first = manager.MountBundle(
        "bundle-one", make_unique<MemoryBundleCatalogSource>(MakeCatalog(MakeId(66), OneEntry(std::move(firstEntry)))));
    ASSERT_TRUE(first.IsSuccess());
    first.Reference.reset();
    manager.Pump();
    EXPECT_EQ(manager.GetBundleCount(), 0u);

    BundleAssetEntry movedEntry = MakeEntry(assetId, "new/location.png", BundleEntryState::Unknown);
    movedEntry.TypeName = "future";
    movedEntry.Descriptor.reset();
    BundleMountResult second = manager.MountBundle(
        "bundle-two", make_unique<MemoryBundleCatalogSource>(MakeCatalog(MakeId(67), OneEntry(std::move(movedEntry)))));
    ASSERT_TRUE(second.IsSuccess());
    ASSERT_TRUE(second.Reference->GetCatalog());
    ASSERT_EQ(second.Reference->GetCatalog()->Entries.size(), 1u);
    EXPECT_EQ(second.Reference->GetCatalog()->Entries.front().Asset, assetId);
    EXPECT_EQ(second.Reference->GetCatalog()->Entries.front().Locator->GetValue(), "new/location.png");
}

TEST(AssetBundleLoadTest, TextureAndStaticMeshCorruptPayloadsBecomeStructuredFaults) {
    BundleAssetEntry texture;
    texture.Asset = MakeId(69);
    texture.TypeId = runtime_type_id_v<TextureAsset>;
    texture.TypeName = "texture";
    texture.Locator = BundleLocator::TryCreate("missing.texture");
    texture.State = BundleEntryState::Valid;
    texture.Descriptor = make_unique<TextureAssetDescriptor>(true);

    BundleAssetEntry mesh;
    mesh.Asset = MakeId(70);
    mesh.TypeId = runtime_type_id_v<StaticMesh>;
    mesh.TypeName = "staticMesh";
    mesh.Locator = BundleLocator::TryCreate("missing.mesh");
    mesh.State = BundleEntryState::Valid;
    mesh.Descriptor = make_unique<StaticMeshAssetDescriptor>();

    vector<BundleAssetEntry> entries;
    entries.push_back(std::move(texture));
    entries.push_back(std::move(mesh));
    AssetManager manager;
    BundleMountResult mount = manager.MountBundle(
        "gpu-payloads", make_unique<MemoryBundleCatalogSource>(MakeCatalog(MakeId(71), std::move(entries))));
    ASSERT_TRUE(mount.IsSuccess());

    AssetRequestResult textureRequest = manager.LoadCatalog(MakeId(69), runtime_type_id_v<TextureAsset>);
    AssetRequestResult meshRequest = manager.LoadCatalog(MakeId(70), runtime_type_id_v<StaticMesh>);
    ASSERT_TRUE(textureRequest.IsSubmitted());
    ASSERT_TRUE(meshRequest.IsSubmitted());
    manager.Pump();
    ASSERT_TRUE(textureRequest.Reference->IsFaulted());
    ASSERT_TRUE(meshRequest.Reference->IsFaulted());
    EXPECT_EQ(textureRequest.Reference->GetErrorCode(), AssetLoadErrorCode::PayloadFailure);
    EXPECT_EQ(meshRequest.Reference->GetErrorCode(), AssetLoadErrorCode::PayloadFailure);
}

TEST(AssetBundleLoadTest, XmlTextureAndStaticMeshEntriesReachReadyWithGpuServices) {
    const std::optional<render::RenderBackend> backend = ProbeAssetGpuBackend();
    if (!backend.has_value()) {
        GTEST_SKIP() << "no render backend is available on this machine";
    }

    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "radray_bundle_gpu_assets_test";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    ASSERT_TRUE(std::filesystem::create_directories(root, error));

    ImageData sourceImage = MakeSolidImage(20, 40, 60, 255);
    ASSERT_TRUE(sourceImage.WritePNG(PNGWriteSettings{.FilePath = (root / "texture.png").string()}));
    const vector<byte> meshPayload = MakeSimpleMeshPayload();
    ASSERT_TRUE(WriteBytes(root / "mesh.rrmesh", meshPayload));

    const AssetId textureId = MakeId(72);
    const AssetId meshId = MakeId(73);
    const string xml = fmt::format(
        R"xml(<bundle schemaVersion="1" bundleId="00000048-0001-4000-8000-000000000001"><assets>
          <texture typeId="{}" assetId="{}" path="texture.png" srgb="true" />
          <staticMesh typeId="{}" assetId="{}" path="mesh.rrmesh" />
        </assets></bundle>)xml",
        runtime_type_id_v<TextureAsset>.ToString(),
        textureId.ToString(),
        runtime_type_id_v<StaticMesh>.ToString(),
        meshId.ToString());

    Application app;
    GpuSystem gpu(&app, MakeAssetGpuDescriptor(*backend));
    AssetManager manager;
    manager.SetGpuSystem(&gpu);
    manager.SetWaitFrameProcessor(&gpu);

    BundleMountResult mount = manager.MountBundle(root, make_unique<XmlBundleCatalogSource>(xml));
    ASSERT_TRUE(mount.IsSuccess());
    AssetRequestResult textureRequest = manager.LoadCatalog(textureId, runtime_type_id_v<TextureAsset>);
    AssetRequestResult meshRequest = manager.LoadCatalog(meshId, runtime_type_id_v<StaticMesh>);
    ASSERT_TRUE(textureRequest.IsSubmitted());
    ASSERT_TRUE(meshRequest.IsSubmitted());
    ASSERT_TRUE(textureRequest.Reference.has_value());
    ASSERT_TRUE(meshRequest.Reference.has_value());

    PumpAssetGpuFrame(manager, gpu, 0);
    ASSERT_TRUE(textureRequest.Reference->IsReady()) << textureRequest.Reference->GetErrorMessage();
    ASSERT_TRUE(meshRequest.Reference->IsReady()) << meshRequest.Reference->GetErrorMessage();
    const auto* texture = static_cast<const TextureAsset*>(textureRequest.Reference->Get());
    const auto* mesh = static_cast<const StaticMesh*>(meshRequest.Reference->Get());
    ASSERT_NE(texture, nullptr);
    ASSERT_NE(mesh, nullptr);
    EXPECT_TRUE(texture->IsValid());
    EXPECT_TRUE(mesh->IsValid());
    EXPECT_FALSE(mesh->GetRenderMesh().Buffers.empty());
    ASSERT_EQ(mesh->GetRenderMesh().Draws.size(), 1u);
    EXPECT_NE(mesh->GetRenderMesh().Draws.front().Vbv.Target, nullptr);
    EXPECT_NE(mesh->GetRenderMesh().Draws.front().Ibv.Target, nullptr);

    textureRequest.Reference->Reset();
    meshRequest.Reference->Reset();
    mount.Reference.reset();
    manager.Pump();
    gpu.WaitAndCleanupCompletedFlights();
    std::filesystem::remove_all(root, error);
}

TEST(AssetBundleLoadTest, ShaderJitBundlePathIsASeparateIncludeService) {
    const std::filesystem::path sourceFile = std::filesystem::path{__FILE__};
    const std::filesystem::path projectRoot =
        sourceFile.parent_path().parent_path().parent_path().parent_path();
    const std::filesystem::path shaderlib = projectRoot / "shaderlib";
    if (!std::filesystem::exists(shaderlib / "passes" / "forward.hlsl")) {
        GTEST_SKIP() << "shaderlib source tree is not available in this checkout";
    }

    ShaderJit jit(vector<std::filesystem::path>{shaderlib});
    if (!jit.IsAvailable()) {
        GTEST_SKIP() << "shader compiler package is unavailable";
    }

    const AssetId assetId = MakeId(68);
    const string xml = fmt::format(
        R"xml(<bundle schemaVersion="1" bundleId="00000045-0001-4000-8000-000000000001"><assets><shader typeId="{}" assetId="{}" path="passes/forward.hlsl" representation="jit-source" target="dxil" /></assets></bundle>)xml",
        runtime_type_id_v<ShaderAsset>.ToString(),
        assetId.ToString());

    AssetManager manager;
    manager.SetShaderJit(&jit);
    BundleMountResult mount = manager.MountBundle(
        shaderlib, make_unique<XmlBundleCatalogSource>(xml));
    ASSERT_TRUE(mount.IsSuccess());
    AssetRequestResult request = manager.LoadCatalog(assetId, runtime_type_id_v<ShaderAsset>);
    ASSERT_TRUE(request.IsSubmitted());
    ASSERT_TRUE(request.Reference.has_value());
    manager.Pump();
    ASSERT_TRUE(request.Reference->IsReady()) << request.Reference->GetErrorMessage();
    const auto* shader = static_cast<const ShaderAsset*>(request.Reference->Get());
    ASSERT_NE(shader, nullptr);
    EXPECT_EQ(shader->GetSourceName(), "passes/forward.hlsl");
    EXPECT_EQ(shader->GetTarget(), shader::ShaderTarget::DXIL);
}

TEST(XmlAssetBundleSourceTest, ParsesV1WithoutRetainingXmlDom) {
    const string xml = fmt::format(R"xml(<?xml version="1.0" encoding="UTF-8"?>
<bundle schemaVersion="1" bundleId="00000001-0001-4000-8000-000000000001">
  <assets>
    <shader typeId="{}"
            assetId="00000003-0001-4000-8000-000000000001"
            path="passes/a&amp;b.hlsl" representation="jit-source" />
  </assets>
</bundle>)xml", runtime_type_id_v<ShaderAsset>.ToString());

    XmlBundleCatalogSource source{xml};
    BundleCatalogSourceResult decoded = source.Read();
    ASSERT_TRUE(decoded.IsSuccess());
    ASSERT_TRUE(decoded.Catalog.has_value());
    EXPECT_EQ(decoded.Catalog->Id, MakeId(1));
    ASSERT_EQ(decoded.Catalog->Entries.size(), 1u);
    const BundleAssetEntry& entry = decoded.Catalog->Entries.front();
    EXPECT_EQ(entry.TypeName, "shader");
    EXPECT_EQ(entry.TypeId, runtime_type_id_v<ShaderAsset>);
    EXPECT_EQ(entry.Asset, MakeId(3));
    ASSERT_TRUE(entry.Locator.has_value());
    EXPECT_EQ(entry.Locator->GetValue(), "passes/a&b.hlsl");
    EXPECT_EQ(entry.State, BundleEntryState::Valid);
    ASSERT_NE(entry.Descriptor, nullptr);
    EXPECT_EQ(entry.Descriptor->GetTypeId(), runtime_type_id_v<ShaderAsset>);
    EXPECT_FALSE(source.Read().IsSuccess()) << "source output is consumed after one read";
}

TEST(XmlAssetBundleSourceTest, KeepsEntryErrorsLocalButRejectsDocumentErrors) {
    const string entryErrors = R"xml(
<bundle schemaVersion="1" bundleId="00000004-0001-4000-8000-000000000001">
  <assets>
    <shader typeId="00000005-0001-4000-8000-000000000001" assetId="00000006-0001-4000-8000-000000000001" path="passes/Foo.hlsl" />
    <future typeId="00000007-0001-4000-8000-000000000001" assetId="00000008-0001-4000-8000-000000000001" path="passes/foo.hlsl" />
    <broken typeId="00000009-0001-4000-8000-000000000001" assetId="0000000a-0001-4000-8000-000000000001" path="../escape.bin" />
  </assets>
</bundle>)xml";
    XmlBundleCatalogSource source{entryErrors};
    BundleCatalogSourceResult decoded = source.Read();
    ASSERT_TRUE(decoded.IsSuccess());
    ASSERT_EQ(decoded.Catalog->Entries.size(), 3u);
    EXPECT_EQ(decoded.Catalog->Entries[0].State, BundleEntryState::Invalid);
    EXPECT_EQ(decoded.Catalog->Entries[0].Diagnostics.front().Code, BundleDiagnosticCode::TypeIdMismatch);
    EXPECT_EQ(decoded.Catalog->Entries[1].State, BundleEntryState::Invalid);
    EXPECT_EQ(decoded.Catalog->Entries[1].Diagnostics.front().Code, BundleDiagnosticCode::LocatorCaseCollision);
    EXPECT_EQ(decoded.Catalog->Entries[2].State, BundleEntryState::Invalid);
    EXPECT_EQ(decoded.Catalog->Entries[2].Diagnostics.front().Code, BundleDiagnosticCode::InvalidLocator);

    const string externalEntity = R"xml(<!DOCTYPE bundle [<!ENTITY x SYSTEM "file:///secret">]><bundle schemaVersion="1" bundleId="0000000b-0001-4000-8000-000000000001"><assets /></bundle>)xml";
    ASSERT_FALSE(XmlBundleCatalogSource{externalEntity}.Read().IsSuccess());

    const string duplicateId = R"xml(<bundle schemaVersion="1" bundleId="0000000c-0001-4000-8000-000000000001"><assets><x typeId="0000000d-0001-4000-8000-000000000001" assetId="0000000e-0001-4000-8000-000000000001" path="a"/><y typeId="0000000f-0001-4000-8000-000000000001" assetId="0000000e-0001-4000-8000-000000000001" path="b"/></assets></bundle>)xml";
    ASSERT_FALSE(XmlBundleCatalogSource{duplicateId}.Read().IsSuccess());
}

TEST(XmlAssetBundleSourceTest, AotShaderDescriptorIsKnownButRuntimeReportsCapability) {
    const AssetId assetId = MakeId(60);
    const string xml = fmt::format(
        R"xml(<bundle schemaVersion="1" bundleId="0000003c-0001-4000-8000-000000000001"><assets><shader typeId="{}" assetId="{}" path="artifacts/example.shader" representation="aot-artifact" target="spirv" /></assets></bundle>)xml",
        runtime_type_id_v<ShaderAsset>.ToString(),
        assetId.ToString());

    XmlBundleCatalogSource source{xml};
    BundleCatalogSourceResult decoded = source.Read();
    ASSERT_TRUE(decoded.IsSuccess());
    ASSERT_EQ(decoded.Catalog->Entries.size(), 1u);
    const BundleAssetEntry& entry = decoded.Catalog->Entries.front();
    ASSERT_EQ(entry.State, BundleEntryState::Valid);
    const auto* descriptor = dynamic_cast<const ShaderAssetDescriptor*>(entry.Descriptor.get());
    ASSERT_NE(descriptor, nullptr);
    EXPECT_EQ(descriptor->Representation, ShaderAssetRepresentation::AotArtifact);
    EXPECT_EQ(descriptor->Target, shader::ShaderTarget::SPIRV);

    AssetManager manager;
    BundleMountResult mount = manager.MountBundle(
        "shader-aot", make_unique<MemoryBundleCatalogSource>(std::move(decoded.Catalog.value())));
    ASSERT_TRUE(mount.IsSuccess());
    AssetRequestResult request = manager.LoadCatalog(assetId, runtime_type_id_v<ShaderAsset>);
    ASSERT_TRUE(request.IsSubmitted());
    ASSERT_TRUE(request.Reference.has_value());
    manager.Pump();
    EXPECT_TRUE(request.Reference->IsFaulted());
    EXPECT_EQ(request.Reference->GetErrorCode(), AssetLoadErrorCode::CapabilityUnavailable);
}

TEST(XmlAssetBundleSourceTest, XMLAndMemorySourcesProduceEquivalentValueCatalog) {
    const AssetId assetId = MakeId(63);
    const BundleId bundleId = MakeId(64);
    const string xml = fmt::format(
        R"xml(<bundle schemaVersion="1" bundleId="{}"><assets><shader typeId="{}" assetId="{}" path="passes/example.hlsl" representation="jit-source" target="dxil" /></assets></bundle>)xml",
        bundleId.ToString(),
        runtime_type_id_v<ShaderAsset>.ToString(),
        assetId.ToString());

    XmlBundleCatalogSource xmlSource{xml};
    BundleCatalogSourceResult xmlResult = xmlSource.Read();
    ASSERT_TRUE(xmlResult.IsSuccess());
    ASSERT_EQ(xmlResult.Catalog->Entries.size(), 1u);

    BundleCatalog expected;
    expected.Id = bundleId;
    BundleAssetEntry expectedEntry;
    expectedEntry.Asset = assetId;
    expectedEntry.TypeId = runtime_type_id_v<ShaderAsset>;
    expectedEntry.TypeName = "shader";
    expectedEntry.Locator = BundleLocator::TryCreate("passes/example.hlsl");
    expectedEntry.State = BundleEntryState::Valid;
    expectedEntry.Descriptor = make_unique<ShaderAssetDescriptor>(
        ShaderAssetRepresentation::JitSource, shader::ShaderTarget::DXIL);
    expected.Entries.push_back(std::move(expectedEntry));

    MemoryBundleCatalogSource memorySource{std::move(expected)};
    BundleCatalogSourceResult memoryResult = memorySource.Read();
    ASSERT_TRUE(memoryResult.IsSuccess());
    ASSERT_EQ(memoryResult.Catalog->Id, xmlResult.Catalog->Id);
    ASSERT_EQ(memoryResult.Catalog->Entries.size(), xmlResult.Catalog->Entries.size());
    const BundleAssetEntry& fromXml = xmlResult.Catalog->Entries.front();
    const BundleAssetEntry& fromMemory = memoryResult.Catalog->Entries.front();
    EXPECT_EQ(fromMemory.Asset, fromXml.Asset);
    EXPECT_EQ(fromMemory.TypeId, fromXml.TypeId);
    EXPECT_EQ(fromMemory.TypeName, fromXml.TypeName);
    EXPECT_EQ(fromMemory.Locator->GetValue(), fromXml.Locator->GetValue());
    EXPECT_EQ(fromMemory.State, fromXml.State);
    const auto* xmlDescriptor = dynamic_cast<const ShaderAssetDescriptor*>(fromXml.Descriptor.get());
    const auto* memoryDescriptor = dynamic_cast<const ShaderAssetDescriptor*>(fromMemory.Descriptor.get());
    ASSERT_NE(xmlDescriptor, nullptr);
    ASSERT_NE(memoryDescriptor, nullptr);
    EXPECT_EQ(memoryDescriptor->Representation, xmlDescriptor->Representation);
    EXPECT_EQ(memoryDescriptor->Target, xmlDescriptor->Target);
}

TEST(XmlAssetBundleSourceTest, ParserLimitsFailClosed) {
    const string oversizedAttribute = fmt::format(
        R"xml(<bundle schemaVersion="1" bundleId="00000041-0001-4000-8000-000000000001"><assets><future typeId="00000042-0001-4000-8000-000000000001" assetId="00000043-0001-4000-8000-000000000001" path="{}" /></assets></bundle>)xml",
        string(64u * 1024u + 1u, 'x'));
    EXPECT_FALSE(XmlBundleCatalogSource{oversizedAttribute}.Read().IsSuccess());

    string oversizedDocument(4u * 1024u * 1024u + 1u, 'x');
    EXPECT_FALSE(XmlBundleCatalogSource{std::move(oversizedDocument)}.Read().IsSuccess());
}

}  // namespace radray
