#pragma once
#include "external_resource_test_utils.h"
#include "external_resource_ipc.h"

namespace radray::render {
class ExternalResourceConfigurationTest : public testing::TestWithParam<RenderBackend> {
protected:
    void CheckConfiguration(bool enabled);
};
class ExternalResourceFixture : public testing::Test {
protected:
    test::DeviceContext Source, Destination;
    Nullable<test::DeviceContext*> Target{nullptr};
    Nullable<ExternalResourceExtension*> Exporter{nullptr};
    Nullable<ExternalResourceExtension*> Importer{nullptr};
    RenderBackend Producer() const;
    RenderBackend Consumer() const;
    ExternalResourceDescriptor MemoryDescriptor() const;
    ExternalFenceHandleType FenceType() const;
    virtual int Route() const = 0;
    void SetUp() override;
    void TearDown() override;
    void TextureRoundTrips(std::span<const TextureFormat> formats, std::span<const test::ExternalTextureTestUse> uses);
};
class ExternalResourceTest : public ExternalResourceFixture, public testing::WithParamInterface<int> {
protected:
    int Route() const override;
};
class ExternalResourceOpaqueTest : public ExternalResourceTest {};
class ExternalErrorLogProbe {
public:
    ExternalErrorLogProbe();
    ~ExternalErrorLogProbe();
    vector<string> Take();

private:
    std::mutex _mutex;
    vector<string> _messages;
};
#if defined(RADRAY_PLATFORM_WINDOWS)
class ExternalResourceIpcTest : public testing::TestWithParam<int> {};
#endif
}  // namespace radray::render
