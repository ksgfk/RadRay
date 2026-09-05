#include "upload_test_support.h"

#include <atomic>
#include <semaphore>
#include <thread>

#include <gtest/gtest.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/texture_asset.h>

namespace radray {
namespace {

class FrameUploadTest : public testing::Test {
protected:
    test::UploadTestDevice Device;
    ResourceUploader Uploader{&Device, 2};
    FrameUploadScheduler Uploads;
    AssetManager Assets;
    HostWriteBatch Writes;
    test::UploadTestCommand Command;

    StreamingAssetRef<StaticMesh> LoadMesh() {
        return Assets.Load<StaticMesh>({test::kUploadTestId, LoadStaticMesh(Uploads, test::MakeUploadTestMesh()), "upload test"});
    }
    void Record(uint32_t flight) {
        Uploader.BeginFlight(flight, Writes);
        Uploads.RunUploadPhase(&Command, Uploader, flight);
        Uploader.EndFlight(flight);
    }
    void Complete(uint32_t flight) {
        Uploads.NotifyFlightComplete(flight);
        Uploads.PumpCompletedUploads();
        Assets.Pump();
    }
};

TEST_F(FrameUploadTest, CancelBeforeRecordingCreatesNoGpuResources) {
    auto mesh = LoadMesh();
    mesh.Cancel();
    Assets.Pump();
    EXPECT_TRUE(mesh.IsCanceled());
    Record(0);
    EXPECT_EQ(Command.Copies, 0u);
    EXPECT_EQ(Device.DeviceAllocations, 0);
}

TEST_F(FrameUploadTest, CanceledMeshSurvivesUntilItsFlightCompletes) {
    auto mesh = LoadMesh();
    Record(0);
    ASSERT_EQ(Command.Copies, 2u);
    ASSERT_EQ(Device.LiveDeviceBuffers, 2);
    mesh.Cancel();
    Uploads.PumpCompletedUploads();
    Assets.Pump();
    EXPECT_FALSE(mesh.IsCanceled());
    EXPECT_EQ(Device.LiveDeviceBuffers, 2);
    Complete(1);
    EXPECT_EQ(Device.LiveDeviceBuffers, 2);
    std::thread completion([&] { Uploads.NotifyFlightComplete(0); });
    completion.join();
    EXPECT_EQ(Device.LiveDeviceBuffers, 2);
    Uploads.PumpCompletedUploads();
    Assets.Pump();
    EXPECT_EQ(Device.LiveDeviceBuffers, 0);
    EXPECT_TRUE(mesh.IsCanceled());
}

TEST_F(FrameUploadTest, CanceledTextureRetainsTextureAndViewUntilFence) {
    ImageData pixels;
    pixels.Width = pixels.Height = 2;
    pixels.Format = ImageFormat::RGBA8_BYTE;
    pixels.Data = make_unique<byte[]>(16);
    auto texture = LoadTextureAssetFromImage(Assets, Uploads, test::kUploadTestId, "upload texture", std::move(pixels));
    Record(0);
    ASSERT_EQ(Command.Copies, 1u);
    texture.Cancel();
    Uploads.PumpCompletedUploads();
    EXPECT_EQ(Device.LiveTextures, 1);
    EXPECT_EQ(Device.LiveTextureViews, 1);
    Complete(0);
    EXPECT_TRUE(texture.IsCanceled());
    EXPECT_EQ(Device.LiveTextures, 0);
    EXPECT_EQ(Device.LiveTextureViews, 0);
}

TEST_F(FrameUploadTest, CancellationBeforeWaitGpuStillRetainsRecordedResources) {
    StreamingAssetRef<StaticMesh> mesh;
    auto load = [](FrameUploadScheduler& uploads, StreamingAssetRef<StaticMesh>* ref) -> task<AssetLoadResult> {
        auto frame = co_await uploads.BeginUpload();
        auto payload = frame.GetUploader().UploadMeshResource(frame.GetCommandBuffer(), test::MakeUploadTestMesh());
        ref->Cancel();
        co_await frame.WaitGpu();
        co_return AssetLoadResult::Failure("A canceled upload must not reach this point");
    };
    mesh = Assets.Load<StaticMesh>({test::kUploadTestId, load(Uploads, &mesh), "cancel while recording"});
    Record(0);
    EXPECT_EQ(Command.Copies, 2u);
    EXPECT_EQ(Device.LiveDeviceBuffers, 2);
    Uploads.PumpCompletedUploads();
    EXPECT_EQ(Device.LiveDeviceBuffers, 2);
    Complete(0);
    EXPECT_EQ(Device.LiveDeviceBuffers, 0);
    EXPECT_TRUE(mesh.IsCanceled());
}

TEST_F(FrameUploadTest, AllocationFailureLeavesNoRecordedMeshCopies) {
    for (int failure = 1; failure <= 2; ++failure) {
        Device.DeviceAllocations = 0;
        Device.FailDeviceAllocation = failure;
        Uploader.BeginFlight(0, Writes);
        auto mesh = Uploader.UploadMeshResource(&Command, test::MakeUploadTestMesh());
        Uploader.EndFlight(0);
        EXPECT_FALSE(mesh);
        EXPECT_EQ(Command.Copies, 0u);
        EXPECT_EQ(Device.LiveDeviceBuffers, 0);
    }
}

TEST_F(FrameUploadTest, ReusedFlightDoesNotConsumeItsPreviousCompletion) {
    auto first = LoadMesh();
    Record(0);
    first.Cancel();
    Uploads.NotifyFlightComplete(0);
    const AssetId secondId{0x1234, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    auto second = Assets.Load<StaticMesh>({secondId, LoadStaticMesh(Uploads, test::MakeUploadTestMesh()), "second upload"});
    Uploader.CollectFlight(0);
    Record(0);
    second.Cancel();
    Uploads.PumpCompletedUploads();
    Assets.Pump();
    EXPECT_TRUE(first.IsCanceled());
    EXPECT_FALSE(second.IsCanceled());
    EXPECT_EQ(Device.LiveDeviceBuffers, 2);
    Complete(0);
    EXPECT_TRUE(second.IsCanceled());
    EXPECT_EQ(Device.LiveDeviceBuffers, 0);
}

task<void> AwaitCanceledUpload(FrameUploadScheduler& uploads, uint32_t& canceled) {
    const auto stop = co_await CurrentStopToken();
    auto result = co_await AwaitWithStopToken(uploads.BeginUpload(), stop);
    if (!result) ++canceled;
}

TEST_F(FrameUploadTest, ConcurrentCompletionsAndGameThreadCancellationDrainExactlyOnce) {
    std::binary_semaphore start{0};
    std::atomic_bool done{false};
    std::thread completion([&] {
        start.acquire();
        while (!done.load(std::memory_order_acquire)) {
            Uploads.NotifyFlightComplete(0);
            std::this_thread::yield();
        }
    });
    uint32_t canceled = 0;
    start.release();
    for (uint32_t i = 0; i < 2000; ++i) {
        TaskScope scope;
        scope.Spawn(AwaitCanceledUpload(Uploads, canceled));
        scope.RequestStop();
        Uploads.PumpCompletedUploads();
    }
    done.store(true, std::memory_order_release);
    completion.join();
    Uploads.PumpCompletedUploads();
    EXPECT_EQ(canceled, 2000u);
    EXPECT_EQ(Device.LiveDeviceBuffers, 0);
}

}  // namespace
}  // namespace radray
